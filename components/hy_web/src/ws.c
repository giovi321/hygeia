/* Live events over WebSocket. The event bus subscriber runs in the publisher's task (the LVGL task): it only encodes
 * the event into a ring of 16 and queues one flush to the httpd task without waiting. The flush and the 30 s ping
 * run in the httpd task, the only task that writes to the sockets. */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hy/events.h"
#include "web_internal.h"

static const char *TAG = "web_ws";

#define WS_MAX_CLIENTS 2
#define WS_QUEUE 16
#define WS_MSG_LEN 192
#define WS_PING_US (30 * 1000000LL)
#define WS_RX_MAX 128

static const char RESYNC[] = "{\"ev\":\"resync\"}";

/* httpd task only, except the count read by the subscriber */
static struct {
    int fd;
    int sess;
    uint32_t gen;
    int64_t since_us;
} s_cli[WS_MAX_CLIENTS];
static atomic_int s_ncli;
static httpd_handle_t s_srv;   /* httpd task: the server the clients belong to */

/* ring, under s_qmux */
static portMUX_TYPE s_qmux = portMUX_INITIALIZER_UNLOCKED;
static HY_EXT_BSS char s_q[WS_QUEUE][WS_MSG_LEN];
static uint8_t s_qhead, s_qcount;
static bool s_resync;
static uint32_t s_dropped, s_sent;
static atomic_bool s_flush_queued;

static esp_timer_handle_t s_ping;
static int s_hs_sess = -1;     /* pre-handshake to post-handshake, httpd task */
static uint32_t s_hs_gen;

static void cli_remove_at(int i)
{
    for (int k = i; k < WS_MAX_CLIENTS - 1; k++) {
        s_cli[k] = s_cli[k + 1];
    }
    s_cli[WS_MAX_CLIENTS - 1].fd = -1;
    atomic_fetch_sub(&s_ncli, 1);
}

void web_ws_on_close(int fd)
{
    int n = atomic_load(&s_ncli);
    for (int i = 0; i < n; i++) {
        if (s_cli[i].fd == fd) {
            cli_remove_at(i);
            ESP_LOGI(TAG, "live-event socket %d closed", fd);
            return;
        }
    }
}

static void send_text(int fd, const char *text)
{
    httpd_ws_frame_t f = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };
    esp_err_t err = httpd_ws_send_frame_async(s_srv, fd, &f);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send to %d: %s, closing", fd, esp_err_to_name(err));
        httpd_sess_trigger_close(s_srv, fd);
    }
}

/* httpd task */
static void flush_work(void *arg)
{
    (void)arg;
    atomic_store(&s_flush_queued, false);
    char msg[WS_MSG_LEN];
    for (;;) {
        bool have = false;
        bool resync = false;
        portENTER_CRITICAL(&s_qmux);
        if (s_resync) {
            s_resync = false;
            resync = true;
        } else if (s_qcount) {
            memcpy(msg, s_q[s_qhead], WS_MSG_LEN);
            s_qhead = (uint8_t)((s_qhead + 1) % WS_QUEUE);
            s_qcount--;
            have = true;
        }
        portEXIT_CRITICAL(&s_qmux);
        if (!have && !resync) {
            break;
        }
        int n = atomic_load(&s_ncli);
        for (int i = 0; i < n; i++) {
            send_text(s_cli[i].fd, resync ? RESYNC : msg);
        }
        s_sent++;
    }
}

/* Any task (the event bus publisher): encode, push, queue the flush; never waits */
static void on_event(const hy_event_t *ev, void *ctx)
{
    (void)ctx;
    if (atomic_load(&s_ncli) == 0) {
        return;   /* a client fetches /api/state when it connects */
    }
    char msg[WS_MSG_LEN];
    if (hy_api_event_json(ev, msg, sizeof msg) < 0) {
        return;
    }
    portENTER_CRITICAL(&s_qmux);
    if (s_qcount == WS_QUEUE) {   /* full: the oldest goes and the client is told to reload the state */
        s_qhead = (uint8_t)((s_qhead + 1) % WS_QUEUE);
        s_qcount--;
        s_resync = true;
        s_dropped++;
    }
    memcpy(s_q[(s_qhead + s_qcount) % WS_QUEUE], msg, WS_MSG_LEN);
    s_qcount++;
    portEXIT_CRITICAL(&s_qmux);
    if (!atomic_exchange(&s_flush_queued, true)) {
        if (!web_queue_work(flush_work, NULL)) {
            atomic_store(&s_flush_queued, false);
        }
    }
}

static void ping_work(void *arg)
{
    (void)arg;
    int n = atomic_load(&s_ncli);
    for (int i = 0; i < n; i++) {
        httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_PING, .payload = NULL, .len = 0};
        if (httpd_ws_send_frame_async(s_srv, s_cli[i].fd, &f) != ESP_OK) {
            httpd_sess_trigger_close(s_srv, s_cli[i].fd);
        }
    }
}

static void ping_timer(void *arg)
{
    (void)arg;
    if (atomic_load(&s_ncli) > 0) {
        web_queue_work(ping_work, NULL);
    }
}

static void sess_check_work(void *arg)
{
    (void)arg;
    int n = atomic_load(&s_ncli);
    for (int i = 0; i < n; i++) {
        if (!web_sess_valid(s_cli[i].sess, s_cli[i].gen)) {
            ESP_LOGI(TAG, "session of live-event socket %d forgotten: closing", s_cli[i].fd);
            httpd_sess_trigger_close(s_srv, s_cli[i].fd);
        }
    }
}

void web_ws_sessions_changed(void)
{
    if (atomic_load(&s_ncli) > 0) {
        web_queue_work(sess_check_work, NULL);
    }
}

/* Upgrade checks, before the handshake answer: Host, session cookie, Origin host equal to Host. */
static esp_err_t ws_pre(httpd_req_t *req)
{
    web_net_t net;
    web_net_now(&net);
    if (!web_host_check(req, &net, true)) {
        return ESP_FAIL;
    }
    char host[64] = "", origin[96] = "", cookie[256] = "";
    bool has_origin = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin) == ESP_OK;
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
    bool has_cookie = httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof cookie) == ESP_OK;
    uint32_t gen = 0;
    int sess = has_cookie ? web_sess_check(cookie, &gen) : -1;
    memset(cookie, 0, sizeof cookie);
    /* The write guard's Origin rule, with the Origin required: a page from another site cannot open the stream */
    hy_guard_in_t g = {.m = HY_M_POST, .open_route = false, .session_ok = sess >= 0, .x_hygeia = "1",
                       .origin = has_origin ? origin : NULL, .host = host};
    hy_guard_t res = has_origin ? hy_auth_guard(&g) : HY_GUARD_FORBIDDEN;
    web_req_t r = {.req = req, .sess = sess};
    if (res == HY_GUARD_FORBIDDEN) {
        web_send_err(&r, 403, "forbidden", NULL, "Origin does not match the host");
        return ESP_FAIL;
    }
    if (res == HY_GUARD_UNPAIRED) {
        web_send_err(&r, 401, "unpaired", NULL, "This browser is not paired");
        return ESP_FAIL;
    }
    s_hs_sess = sess;
    s_hs_gen = gen;
    return ESP_OK;
}

static esp_err_t ws_post(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (atomic_load(&s_ncli) == WS_MAX_CLIENTS) {   /* a third client closes the oldest */
        int old = s_cli[0].fd;
        cli_remove_at(0);
        ESP_LOGI(TAG, "third live-event client: closing socket %d", old);
        httpd_sess_trigger_close(req->handle, old);
    }
    int n = atomic_load(&s_ncli);
    s_cli[n].fd = fd;
    s_cli[n].sess = s_hs_sess;
    s_cli[n].gen = s_hs_gen;
    s_cli[n].since_us = esp_timer_get_time();
    atomic_fetch_add(&s_ncli, 1);
    s_hs_sess = -1;
    ESP_LOGI(TAG, "live-event socket %d open (%d client(s))", fd, n + 1);
    return ESP_OK;
}

/* Frames from the browser carry nothing the device needs: read and drop them. */
static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (httpd_ws_get_fd_info(req->handle, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        web_net_t net;
        web_net_now(&net);
        if (!web_host_check(req, &net, true)) {
            return ESP_OK;
        }
        web_req_t r = {.req = req, .sess = -1};
        return web_send_err(&r, 400, "invalid", NULL, "WebSocket upgrade required");
    }
    httpd_ws_frame_t f = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (f.len > WS_RX_MAX) {
        return ESP_FAIL;   /* closes the socket */
    }
    if (f.len) {
        uint8_t buf[WS_RX_MAX];
        f.payload = buf;
        err = httpd_ws_recv_frame(req, &f, f.len);
    }
    return err;
}

esp_err_t web_ws_register(httpd_handle_t srv)
{
    static const httpd_uri_t uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .ws_pre_handshake_cb = ws_pre,
        .ws_post_handshake_cb = ws_post,
    };
    return httpd_register_uri_handler(srv, &uri);
}

void web_ws_init(void)
{
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        s_cli[i].fd = -1;
    }
    if (hy_ev_subscribe(on_event, NULL) < 0) {
        ESP_LOGE(TAG, "event subscribe failed: no live events");
    }
    const esp_timer_create_args_t a = {.callback = ping_timer, .name = "ws_ping"};
    if (esp_timer_create(&a, &s_ping) == ESP_OK) {
        esp_timer_start_periodic(s_ping, WS_PING_US);
    }
}

/* Called by the server control task while the httpd task is not running (before start, after stop) */
void web_ws_attach(httpd_handle_t srv)
{
    s_srv = srv;
    atomic_store(&s_ncli, 0);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        s_cli[i].fd = -1;
    }
    portENTER_CRITICAL(&s_qmux);
    s_qcount = 0;
    s_resync = false;
    portEXIT_CRITICAL(&s_qmux);
    atomic_store(&s_flush_queued, false);
}

void web_ws_detach(void)
{
    atomic_store(&s_ncli, 0);
    s_srv = NULL;
}

#if CONFIG_HY_DEBUG_CONSOLE
void web_ws_debug(void)
{
    int n = atomic_load(&s_ncli);
    portENTER_CRITICAL(&s_qmux);
    unsigned qc = s_qcount, dropped = s_dropped, sent = s_sent;
    portEXIT_CRITICAL(&s_qmux);
    printf("ws: %d client(s), queue %u/%d, sent %u, dropped %u\n", n, qc, WS_QUEUE, sent, dropped);
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < n && i < WS_MAX_CLIENTS; i++) {
        printf("  socket %d, session slot %d, open %lld s\n", s_cli[i].fd, s_cli[i].sess,
               (long long)((now - s_cli[i].since_us) / 1000000));
    }
}
#endif
