/* HTTP server: life cycle (follows the network state), the /api dispatcher, static assets, the Host check and the
 * hand-off of UI callbacks to the LVGL task. */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "hy/board.h"
#include "hy/net.h"
#include "hy/power_run.h"
#include "lvgl.h"
#include "lwip/ip4_addr.h"
#include "hy/job.h"
#include "web_internal.h"

static const char *TAG = "web";

#define PORTAL_URL "http://192.168.4.1/"
#define AP_IP "192.168.4.1"
#define CTL_STACK 3072

/* The running server. s_srv_users counts callers between reading the handle and returning from httpd_queue_work;
 * the control task clears the handle, then waits for them before httpd_stop frees the server. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static httpd_handle_t s_srv;
static int s_srv_users;
static TaskHandle_t s_ctl;
static const hy_web_ui_t *s_ui;
static bool s_started;

/* last requests, httpd task writes, console reads (debug figures only) */
#define REQ_LOG 5
static struct {
    char method;
    char uri[40];
    int route;
    int status;
} s_reqlog[REQ_LOG];
static unsigned s_reqlog_next;

/* ---------------------------------------------------------------- helpers */

const char *web_status_line(int status)
{
    switch (status) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 202: return "202 Accepted";
    case 204: return "204 No Content";
    case 302: return "302 Found";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 408: return "408 Request Timeout";
    case 409: return "409 Conflict";
    case 410: return "410 Gone";
    case 412: return "412 Precondition Failed";
    case 413: return "413 Content Too Large";
    case 421: return "421 Misdirected Request";
    case 422: return "422 Unprocessable Content";
    case 428: return "428 Precondition Required";
    case 429: return "429 Too Many Requests";
    case 501: return "501 Not Implemented";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
    }
}

esp_err_t web_send_json(web_req_t *r, int status, const char *json)
{
    r->status = status;
    httpd_resp_set_status(r->req, web_status_line(status));
    httpd_resp_set_type(r->req, "application/json");
    httpd_resp_set_hdr(r->req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r->req, json);
}

esp_err_t web_send_err(web_req_t *r, int status, const char *code, const char *field, const char *message)
{
    hy_api_err_t e = {.err = HY_E_INVALID, .http = status, .code = code};
    if (field) {
        strlcpy(e.field, field, sizeof e.field);
    }
    char buf[256];
    if (hy_api_err_json(&e, message, buf, sizeof buf) < 0) {
        snprintf(buf, sizeof buf, "{\"error\":\"%s\"}", code);
    }
    return web_send_json(r, status, buf);
}

esp_err_t web_send_hy_err(web_req_t *r, hy_err_t err, const char *message)
{
    hy_api_err_t e;
    hy_api_err_from(err, &e);
    char buf[256];
    if (hy_api_err_json(&e, message, buf, sizeof buf) < 0) {
        snprintf(buf, sizeof buf, "{\"error\":\"internal\"}");
    }
    return web_send_json(r, e.http, buf);
}

esp_err_t web_refuse(web_req_t *r, int status, const char *code, const char *field, const char *msg)
{
    if (status == 503) {
        httpd_resp_set_hdr(r->req, "Retry-After", "2");
    }
    web_send_err(r, status, code, field, msg);
    return r->req->content_len > WEB_BODY_MAX ? ESP_FAIL : ESP_OK;   /* a large unread body: close */
}

esp_err_t web_send_busy(web_req_t *r)
{
    httpd_resp_set_hdr(r->req, "Retry-After", "2");
    return web_send_err(r, 503, "busy", NULL, "The device is busy, try again");
}

void web_net_now(web_net_t *out)
{
    hy_net_status_t st;
    hy_net_status(&st);
    memset(out, 0, sizeof *out);
    out->ap = st.state == HY_NS_AP;
    if (st.ip) {
        ip4_addr_t a = {.addr = st.ip};
        ip4addr_ntoa_r(&a, out->ip, sizeof out->ip);
    }
    strlcpy(out->host, st.host, sizeof out->host);
}

/* DNS rebinding defence, on every request before the auth guard: the device's IP, the access point address while the
 * access point runs, <host>, <host>.local and any name whose first label is <host>. */
bool web_host_check(httpd_req_t *req, const web_net_t *net, bool api)
{
    char host[WEB_HDR_HOST];
    bool ok = false;
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) == ESP_OK) {
        char local[40];
        snprintf(local, sizeof local, "%s.local", net->host);
        const char *allowed[4];
        size_t n = 0;
        if (net->ip[0]) {
            allowed[n++] = net->ip;
        }
        if (net->ap) {
            allowed[n++] = AP_IP;
        }
        allowed[n++] = net->host;
        allowed[n++] = local;
        ok = hy_host_allowed(host, allowed, n) || hy_host_first_label_is(host, net->host);
    }
    if (ok) {
        return true;
    }
    if (net->ap) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", PORTAL_URL);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, NULL, 0);
        return false;
    }
    if (api) {
        web_req_t r = {.req = req, .sess = -1};
        web_send_err(&r, 421, "host", NULL, "Unknown host name");
    } else {
        httpd_resp_set_status(req, "421 Misdirected Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Unknown host name\n");
    }
    return false;
}

static void reqlog(httpd_req_t *req, int route, int status)
{
    unsigned i = s_reqlog_next++ % REQ_LOG;
    s_reqlog[i].method = req->method == HTTP_GET ? 'G' : req->method == HTTP_POST ? 'P'
                       : req->method == HTTP_PUT ? 'U' : req->method == HTTP_DELETE ? 'D' : '?';
    strlcpy(s_reqlog[i].uri, req->uri, sizeof s_reqlog[i].uri);
    s_reqlog[i].route = route;
    s_reqlog[i].status = status;
}

/* ---------------------------------------------------------------- /api dispatcher */

static bool method_of(int http, hy_method_t *m)
{
    switch (http) {
    case HTTP_GET: *m = HY_M_GET; return true;
    case HTTP_POST: *m = HY_M_POST; return true;
    case HTTP_PUT: *m = HY_M_PUT; return true;
    case HTTP_DELETE: *m = HY_M_DELETE; return true;
    default: return false;
    }
}

/* Reads the whole body into PSRAM, within BODY_DEADLINE_US and at most BODY_MAX_TIMEOUTS consecutive receive timeouts
 * (5 s each): a client that announces a body and never sends it cannot hold the single httpd task. */
#define BODY_DEADLINE_US (10 * 1000000LL)
#define BODY_MAX_TIMEOUTS 2
typedef enum { BODY_OK = 0, BODY_NOMEM, BODY_TIMEOUT, BODY_SOCK } body_res_t;

static body_res_t read_body(web_req_t *r, size_t *consumed)
{
    size_t len = r->req->content_len;
    r->body = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->body) {
        return BODY_NOMEM;
    }
    size_t got = 0;
    int timeouts = 0;
    int64_t deadline = esp_timer_get_time() + BODY_DEADLINE_US;
    while (got < len) {
        if (esp_timer_get_time() > deadline) {
            *consumed = got;
            return BODY_TIMEOUT;
        }
        int n = httpd_req_recv(r->req, r->body + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= BODY_MAX_TIMEOUTS) {
                *consumed = got;
                return BODY_TIMEOUT;
            }
            continue;
        }
        if (n <= 0) {
            *consumed = got;
            return BODY_SOCK;
        }
        timeouts = 0;
        got += (size_t)n;
    }
    r->body[len] = 0;
    r->body_len = len;
    *consumed = len;
    return BODY_OK;
}

/* Order: Host, route, guard (X-Hygeia and Origin on writes, then the session), activity, body (16 KB), handler. The
 * guard runs before the body is read, so an unpaired client cannot make the device buffer anything. */
static esp_err_t api_handler(httpd_req_t *req)
{
    web_req_t r = {.req = req, .sess = -1};
    char empty[1] = "";
    size_t consumed = 0;
    esp_err_t ret = ESP_OK;
    int route = -1;

    web_net_t net;
    web_net_now(&net);
    if (!web_host_check(req, &net, true)) {
        reqlog(req, -1, net.ap ? 302 : 421);
        return req->content_len > WEB_BODY_MAX ? ESP_FAIL : ESP_OK;
    }

    hy_method_t m;
    uint32_t id = 0;
    const hy_route_t *rt = method_of(req->method, &m) ? hy_route_match(m, req->uri, &id) : NULL;
    if (!rt) {
        web_send_err(&r, 404, "not_found", NULL, "No such API path");
        goto done;
    }
    route = rt->id;
    r.route = rt;
    r.id = id;

    char host[WEB_HDR_HOST] = "", xh[8] = "", origin[WEB_HDR_ORIGIN] = "", cookie[WEB_HDR_COOKIE] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
    bool has_xh = httpd_req_get_hdr_value_str(req, "X-Hygeia", xh, sizeof xh) == ESP_OK;
    bool has_origin = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin) == ESP_OK;
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof cookie) == ESP_OK) {
        r.sess = web_sess_check(cookie, NULL);
    }
    memset(cookie, 0, sizeof cookie);
    hy_guard_in_t g = {.m = m, .open_route = (rt->flags & HY_RF_OPEN) != 0, .session_ok = r.sess >= 0,
                       .x_hygeia = has_xh ? xh : NULL, .origin = has_origin ? origin : NULL, .host = host};
    switch (hy_auth_guard(&g)) {
    case HY_GUARD_FORBIDDEN:
        web_send_err(&r, 403, "forbidden", NULL, "Write requests need X-Hygeia: 1 and a matching Origin");
        goto done;
    case HY_GUARD_UNPAIRED:
        web_send_err(&r, 401, "unpaired", NULL, "This browser is not paired");
        goto done;
    default:
        break;
    }
    if (m != HY_M_GET && rt->id != HY_R_PAIR_START && rt->id != HY_R_PAIR && web_backup_busy()) {
        web_send_err(&r, 409, "busy", NULL, "An import is running, try again when it is done");
        goto done;
    }
    if (rt->id != HY_R_INTEG) {
        hy_net_activity();   /* the Settings page reads the integration status by itself: not someone acting */
    }

    web_handler_t h = web_route_handler(rt->id);
    if (!h) {
        web_send_err(&r, 501, "not_implemented", NULL, "Not available in this firmware");
        goto done;
    }
    if (rt->id != HY_R_IMPORT && rt->id != HY_R_OTA) {   /* those two read their own bodies with their own limits */
        if (req->content_len > WEB_BODY_MAX) {
            web_send_err(&r, 413, "too_large", NULL, "Request body above 16 KB");
            goto done;
        }
        body_res_t br = read_body(&r, &consumed);
        if (br != BODY_OK) {
            ESP_LOGW(TAG, "body read failed on %s (%s)", req->uri,
                     br == BODY_NOMEM ? "no memory" : br == BODY_TIMEOUT ? "timeout" : "socket");
            if (r.body) {   /* the part that did arrive may hold a password or a secret */
                volatile char *v = (volatile char *)r.body;
                for (size_t k = 0; k < consumed; k++) {
                    v[k] = 0;
                }
            }
            heap_caps_free(r.body);
            if (br == BODY_NOMEM) {
                web_send_busy(&r);
            } else if (br == BODY_TIMEOUT) {
                web_send_err(&r, 408, "timeout", NULL, "The request body did not arrive in time");
            }
            reqlog(req, route, r.status);
            return ESP_FAIL;   /* closes the socket */
        }
    } else {
        r.body = NULL;
    }
    if (!r.body) {
        r.body = empty;
    }
    ret = h(&r);
    if (r.body != empty) {
        heap_caps_free(r.body);
    }
    if (rt->id == HY_R_IMPORT || rt->id == HY_R_OTA) {
        consumed = req->content_len;   /* the handler owns the stream */
    }

done:
    reqlog(req, route, r.status);
    /* A large unread body is not drained: closing the socket is cheaper */
    if (req->content_len > consumed && req->content_len - consumed > WEB_BODY_MAX) {
        return ESP_FAIL;
    }
    return ret;
}

/* ---------------------------------------------------------------- static assets */

static esp_err_t static_handler(httpd_req_t *req)
{
    web_net_t net;
    web_net_now(&net);
    if (!web_host_check(req, &net, false)) {
        reqlog(req, -1, net.ap ? 302 : 421);
        return ESP_OK;
    }
    char path[48];
    size_t n = strcspn(req->uri, "?#");
    if (n >= sizeof path) {
        n = 0;   /* too long for any asset */
        path[0] = 0;
    } else {
        memcpy(path, req->uri, n);
        path[n] = 0;
    }
    if (strcmp(path, "/") == 0) {
        strcpy(path, "/index.html");
    }
    for (size_t i = 0; i < hy_web_assets_n; i++) {
        const hy_web_asset_t *a = &hy_web_assets[i];
        if (strcmp(a->path, path) == 0) {
            hy_net_activity();
            httpd_resp_set_type(req, a->mime);
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            httpd_resp_set_hdr(req, "Cache-Control", a->immutable ? "max-age=31536000, immutable" : "no-cache");
            reqlog(req, -1, 200);
            return httpd_resp_send(req, (const char *)a->gz, a->len);
        }
    }
    if (net.ap) {   /* the captive portal: any other path (generate_204, hotspot-detect.html, ...) goes to the UI */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", PORTAL_URL);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        reqlog(req, -1, 302);
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    reqlog(req, -1, 404);
    return httpd_resp_sendstr(req, "Not found\n");
}

/* ---------------------------------------------------------------- life cycle */

static void on_close(httpd_handle_t hd, int fd)
{
    (void)hd;
    web_ws_on_close(fd);
    close(fd);
}

bool web_queue_work(httpd_work_fn_t fn, void *arg)
{
    portENTER_CRITICAL(&s_mux);
    httpd_handle_t h = s_srv;
    if (h) {
        s_srv_users++;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!h) {
        return false;
    }
    bool ok = httpd_queue_work(h, fn, arg) == ESP_OK;
    portENTER_CRITICAL(&s_mux);
    s_srv_users--;
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

static httpd_handle_t server_start(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = 80;
    c.stack_size = 6144;
    c.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    /* Socket budget, CONFIG_LWIP_MAX_SOCKETS 14: httpd 7, lwIP internal 3, captive DNS 1 (access point only), MQTT 1,
     * a webhook POST or the GitHub update 1, the debug "integ tcp" or spare 1 */
    c.max_open_sockets = 7;
    c.lru_purge_enable = true;
    c.max_uri_handlers = 12;
    c.max_req_hdr_len = 2048;
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.close_fn = on_close;
    httpd_handle_t h = NULL;
    esp_err_t err = httpd_start(&h, &c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return NULL;
    }
    web_ws_attach(h);
    static const httpd_uri_t api[] = {
        {.uri = "/api/*", .method = HTTP_GET, .handler = api_handler},
        {.uri = "/api/*", .method = HTTP_POST, .handler = api_handler},
        {.uri = "/api/*", .method = HTTP_PUT, .handler = api_handler},
        {.uri = "/api/*", .method = HTTP_DELETE, .handler = api_handler},
    };
    static const httpd_uri_t files = {.uri = "/*", .method = HTTP_GET, .handler = static_handler};
    err = web_ws_register(h);   /* before the catch-all file handler, which would also match "/ws" */
    for (size_t i = 0; err == ESP_OK && i < sizeof api / sizeof api[0]; i++) {
        err = httpd_register_uri_handler(h, &api[i]);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(h, &files);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uri handlers: %s", esp_err_to_name(err));
        httpd_stop(h);
        web_ws_detach();
        return NULL;
    }
    ESP_LOGI(TAG, "server up on port 80 (%u assets, %u bytes gzipped)", (unsigned)hy_web_assets_n,
             (unsigned)hy_web_assets_bytes);
    return h;
}

static void server_stop(void)
{
    portENTER_CRITICAL(&s_mux);
    httpd_handle_t h = s_srv;
    s_srv = NULL;
    portEXIT_CRITICAL(&s_mux);
    if (!h) {
        return;
    }
    for (;;) {
        portENTER_CRITICAL(&s_mux);
        int users = s_srv_users;
        portEXIT_CRITICAL(&s_mux);
        if (!users) {
            break;
        }
        vTaskDelay(1);
    }
    web_backup_stop();   /* an export or import worker holds a request of this server */
    httpd_stop(h);   /* waits for a handler in progress (at most its job timeout) */
    web_backup_allow();
    web_ws_detach();
    ESP_LOGI(TAG, "server stopped");
}

static bool wanted(void)
{
    hy_net_status_t st;
    hy_net_status(&st);
    return st.state != HY_NS_OFF && st.state != HY_NS_PAUSED;
}

/* Starts and stops the server outside the net task, whose callbacks must not block (httpd_stop waits for the httpd
 * task). PSRAM stack: it never touches flash. */
static void ctl_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool want = wanted();
        portENTER_CRITICAL(&s_mux);
        bool running = s_srv != NULL;
        portEXIT_CRITICAL(&s_mux);
        if (want && !running) {
            httpd_handle_t h = server_start();
            portENTER_CRITICAL(&s_mux);
            s_srv = h;
            portEXIT_CRITICAL(&s_mux);
        } else if (!want && running) {
            server_stop();
        }
        /* A failed start is retried on the next status change or after 30 s */
        ulTaskNotifyTake(pdTRUE, (want && !s_srv) ? pdMS_TO_TICKS(30000) : portMAX_DELAY);
    }
}

static void on_net(const hy_net_status_t *st, void *ctx)
{
    (void)st;
    (void)ctx;
    if (s_ctl) {
        xTaskNotifyGive(s_ctl);
    }
}

esp_err_t hy_web_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    esp_err_t err = web_sess_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sessions: %s", esp_err_to_name(err));
    }
    web_pair_init();
    web_ws_init();
    web_update_start();
    web_integ_start();
    if (xTaskCreateWithCaps(ctl_task, "web_ctl", CTL_STACK, NULL, 3, &s_ctl,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "control task not created");
        return ESP_ERR_NO_MEM;
    }
    if (hy_net_subscribe(on_net, NULL) < 0) {
        ESP_LOGE(TAG, "net subscribe failed: the server follows no state changes");
    }
    s_started = true;
    return ESP_OK;
}

/* ---------------------------------------------------------------- UI hand-off */

void hy_web_set_ui(const hy_web_ui_t *ui)
{
    s_ui = ui;
}

static bool ui_post(void (*fn)(void *), void *arg)
{
    if (!hy_board_display()) {
        return false;
    }
    if (!hy_board_lock(1000)) {
        ESP_LOGW(TAG, "ui call dropped (LVGL busy)");
        return false;
    }
    bool ok = lv_async_call(fn, arg) == LV_RESULT_OK;
    if (!ok) {
        ESP_LOGW(TAG, "ui call dropped (no memory)");
    }
    hy_board_unlock();
    return ok;
}

static void a_pair_hide(void *arg)
{
    if (s_ui && s_ui->pair_hide) {
        s_ui->pair_hide((int)(intptr_t)arg);
    }
}

static void a_toast_ok(void *arg)
{
    if (s_ui && s_ui->toast_ok) {
        s_ui->toast_ok((const char *)arg);
    }
}

static void a_toast_info(void *arg)
{
    if (s_ui && s_ui->toast_info) {
        s_ui->toast_info((const char *)arg);
    }
}

static void a_power_off(void *arg)
{
    (void)arg;
    if (s_ui && s_ui->power_off) {
        s_ui->power_off();
    } else {
        hy_power_off();
    }
}

static void a_settings_refresh(void *arg)
{
    (void)arg;
    if (s_ui && s_ui->settings_applied) {
        const hy_settings_t *s = hy_settings();
        s_ui->settings_applied(s, s);   /* nothing changed in the settings: the open views redraw their values */
    }
}

static void a_restart(void *arg)
{
    (void)arg;
    esp_restart();
}

const hy_web_ui_t *web_ui(void) { return s_ui; }
bool web_ui_restart(void) { return ui_post(a_restart, NULL); }
bool hy_web_restart(void) { return web_ui_restart(); }
bool web_ui_power_off(void) { return ui_post(a_power_off, NULL); }
void web_ui_pair_hide(int reason) { ui_post(a_pair_hide, (void *)(intptr_t)reason); }
void web_ui_settings_refresh(void) { ui_post(a_settings_refresh, NULL); }
void web_ui_toast_ok(const char *text) { ui_post(a_toast_ok, (void *)text); }
void web_ui_toast_info(const char *text) { ui_post(a_toast_info, (void *)text); }

/* ---------------------------------------------------------------- sessions from the screen */

esp_err_t hy_web_forget_all(void)
{
    return web_sess_forget(-1);
}

size_t hy_web_paired_count(void)
{
    return web_sess_count();
}

/* ---------------------------------------------------------------- debug */

#if CONFIG_HY_DEBUG_CONSOLE
void hy_web_debug(const char *args)
{
    (void)args;
    portENTER_CRITICAL(&s_mux);
    httpd_handle_t h = s_srv;
    portEXIT_CRITICAL(&s_mux);
    printf("web: server %s, assets %u files %u bytes gzipped\n", h ? "running" : "stopped",
           (unsigned)hy_web_assets_n, (unsigned)hy_web_assets_bytes);
    if (h) {
        int fds[8];
        size_t n = sizeof fds / sizeof fds[0];
        if (httpd_get_client_list(h, &n, fds) == ESP_OK) {
            printf("sockets: %u open:", (unsigned)n);
            for (size_t i = 0; i < n; i++) {
                printf(" %d%s", fds[i], httpd_ws_get_fd_info(h, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET ? "(ws)" : "");
            }
            printf("\n");
        }
    }
    web_ws_debug();
    web_sess_info_t s[WEB_SESS_MAX];
    size_t ns = web_sess_list(s, WEB_SESS_MAX);
    printf("sessions: %u of %d\n", (unsigned)ns, WEB_SESS_MAX);
    for (size_t i = 0; i < ns; i++) {
        printf("  slot %d: %s, created %lld\n", s[i].slot, s[i].label, (long long)s[i].created);
    }
    printf("last requests (oldest first):\n");
    for (unsigned k = 0; k < REQ_LOG; k++) {
        unsigned i = (s_reqlog_next + k) % REQ_LOG;
        if (!s_reqlog[i].method) {
            continue;
        }
        const char *rn = "-";
        char rb[12];
        if (s_reqlog[i].route >= 0) {
            snprintf(rb, sizeof rb, "%d", s_reqlog[i].route);
            rn = rb;
        }
        printf("  %c %-39s route %s status %d\n", s_reqlog[i].method, s_reqlog[i].uri, rn, s_reqlog[i].status);
    }
}

void hy_web_debug_job(const char *args)
{
    unsigned ms = 0;
    if (sscanf(args, "slow %u", &ms) != 1 || ms > 60000) {
        printf("usage: webjob slow <ms, at most 60000>\n");
        return;
    }
    hy_job_debug_slow(ms);
    printf("webjob: the next job sleeps %u ms in the LVGL task\n", ms);
}
#endif
