/* Wi-Fi routes. hy_net is safe from any task, so these run in the httpd task without a job. Passwords are never
 * logged and never sent back; a body that carried one is wiped after parsing. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hy/net.h"
#include "web_internal.h"

static const char *TAG = "web_wifi";

static const char *const MODE_NAME[] = {"off", "client", "ap"};
static const char *const STATE_NAME[] = {"off", "paused", "connecting", "connected", "failing", "ap"};

/* Zeroes the parsed copy of a password before the tree is freed */
static void wipe_str(const cJSON *it)
{
    if (cJSON_IsString(it) && it->valuestring) {
        memset(it->valuestring, 0, strlen(it->valuestring));
    }
}

static void wipe_body(web_req_t *r)
{
    if (r->body && r->body_len) {
        memset(r->body, 0, r->body_len);
    }
}

static cJSON *networks_json(void)
{
    hy_known_net_t k[HY_NET_MAX_KNOWN];
    size_t n = hy_net_known(k);
    cJSON *a = cJSON_CreateArray();
    for (size_t i = 0; a && i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            break;
        }
        cJSON_AddStringToObject(o, "ssid", k[i].ssid);
        cJSON_AddNumberToObject(o, "last_ok", k[i].last_ok);
        cJSON_AddItemToArray(a, o);
    }
    return a;
}

static bool known(const char *ssid)
{
    hy_known_net_t k[HY_NET_MAX_KNOWN];
    size_t n = hy_net_known(k);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(k[i].ssid, ssid) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t send_networks(web_req_t *r, int status)
{
    cJSON *o = cJSON_CreateObject();
    if (o) {
        cJSON_AddItemToObject(o, "networks", networks_json());
    }
    return web_send_cjson(r, status, o);
}

/* ---------------------------------------------------------------- GET /api/wifi */

static esp_err_t h_wifi_get(web_req_t *r)
{
    hy_net_status_t st;
    hy_net_status(&st);
    cJSON *o = cJSON_CreateObject();
    if (o) {
        cJSON_AddStringToObject(o, "mode", MODE_NAME[(unsigned)st.saved <= HY_NET_AP ? st.saved : 0]);
        cJSON_AddStringToObject(o, "state", STATE_NAME[(unsigned)st.state <= HY_NS_AP ? st.state : 0]);
        if (st.ssid[0]) {
            cJSON_AddStringToObject(o, "ssid", st.ssid);
        } else {
            cJSON_AddNullToObject(o, "ssid");
        }
        if (st.rssi) {
            cJSON_AddNumberToObject(o, "rssi", st.rssi);
        } else {
            cJSON_AddNullToObject(o, "rssi");
        }
        if (st.ip) {
            char ip[16];
            snprintf(ip, sizeof ip, "%u.%u.%u.%u", (unsigned)(st.ip & 0xff), (unsigned)((st.ip >> 8) & 0xff),
                     (unsigned)((st.ip >> 16) & 0xff), (unsigned)(st.ip >> 24));
            cJSON_AddStringToObject(o, "ip", ip);
        } else {
            cJSON_AddNullToObject(o, "ip");
        }
        cJSON_AddStringToObject(o, "host", st.host);
        cJSON_AddStringToObject(o, "ap_ssid", st.ap_ssid);
        cJSON_AddNumberToObject(o, "ap_clients", st.ap_clients);
        cJSON_AddItemToObject(o, "networks", networks_json());
    }
    return web_send_cjson(r, 200, o);
}

/* ---------------------------------------------------------------- POST /api/wifi/mode */

#define MODE_DELAY_US 1000000

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    hy_net_mode_t mode;
    char ssid[33];
    bool has_ssid;
} s_pending;
static esp_timer_handle_t s_mode_timer;

/* httpd task: the net call takes a mutex and writes NVS, which the shared esp_timer task should not */
static void mode_work(void *arg)
{
    (void)arg;
    hy_net_mode_t m;
    char ssid[33];
    bool has;
    portENTER_CRITICAL(&s_mux);
    m = s_pending.mode;
    has = s_pending.has_ssid;
    memcpy(ssid, s_pending.ssid, sizeof ssid);
    portEXIT_CRITICAL(&s_mux);
    esp_err_t e = hy_net_set_mode(m, has ? ssid : NULL);
    ESP_LOGI(TAG, "mode %s: %s", MODE_NAME[m], esp_err_to_name(e));
}

static void mode_fire(void *arg)
{
    (void)arg;
    if (!web_queue_work(mode_work, NULL)) {   /* the server is stopping: apply from here */
        mode_work(NULL);
    }
}

/* {"mode":"client"|"ap"|"off","ssid":"HomeNet"}: the answer goes out first, and the change follows one second later,
 * because it can drop the very connection the browser uses. ssid (client only) is tried first and must be saved. */
static esp_err_t h_wifi_mode(web_req_t *r)
{
    cJSON *o = cJSON_Parse(web_body_or_empty(r));
    const cJSON *mode = o ? cJSON_GetObjectItemCaseSensitive(o, "mode") : NULL;
    const cJSON *ssid = o ? cJSON_GetObjectItemCaseSensitive(o, "ssid") : NULL;
    int m = -1;
    for (int i = 0; cJSON_IsString(mode) && i < 3; i++) {
        if (strcmp(mode->valuestring, MODE_NAME[i]) == 0) {
            m = i;
        }
    }
    esp_err_t e;
    if (m < 0) {
        e = web_send_err(r, 400, "invalid", "mode", "mode is off, client or ap");
    } else if (ssid && !cJSON_IsNull(ssid) && (!cJSON_IsString(ssid) || m != HY_NET_CLIENT)) {
        e = web_send_err(r, 400, "invalid", "ssid", "ssid is for the client mode");
    } else if (cJSON_IsString(ssid) && !known(ssid->valuestring)) {
        e = web_send_err(r, 404, "not_found", "ssid", "That network is not saved");
    } else {
        bool has = cJSON_IsString(ssid);
        portENTER_CRITICAL(&s_mux);
        s_pending.mode = (hy_net_mode_t)m;
        s_pending.has_ssid = has;
        memset(s_pending.ssid, 0, sizeof s_pending.ssid);
        if (has) {
            strlcpy(s_pending.ssid, ssid->valuestring, sizeof s_pending.ssid);
        }
        portEXIT_CRITICAL(&s_mux);
        cJSON *a = cJSON_CreateObject();
        if (a) {
            cJSON_AddStringToObject(a, "mode", MODE_NAME[m]);
            if (has) {
                cJSON_AddStringToObject(a, "ssid", ssid->valuestring);
            } else {
                cJSON_AddNullToObject(a, "ssid");
            }
        }
        e = web_send_cjson(r, 200, a);
        if (!s_mode_timer) {
            const esp_timer_create_args_t args = {.callback = mode_fire, .name = "web_mode"};
            if (esp_timer_create(&args, &s_mode_timer) != ESP_OK) {
                s_mode_timer = NULL;
            }
        }
        if (s_mode_timer) {
            esp_timer_stop(s_mode_timer);
            esp_timer_start_once(s_mode_timer, MODE_DELAY_US);
        }
    }
    cJSON_Delete(o);
    return e;
}

/* ---------------------------------------------------------------- POST, DELETE /api/wifi/networks */

/* {"ssid":"HomeNet","password":"..."}: adds a network or replaces the password of the same name; "" or no password
 * is an open network. Five networks fit. */
static esp_err_t h_net_add(web_req_t *r)
{
    cJSON *o = cJSON_Parse(web_body_or_empty(r));
    const cJSON *ssid = o ? cJSON_GetObjectItemCaseSensitive(o, "ssid") : NULL;
    const cJSON *pass = o ? cJSON_GetObjectItemCaseSensitive(o, "password") : NULL;
    esp_err_t e;
    if (!cJSON_IsString(ssid) || hy_net_check_ssid(ssid->valuestring) != HY_OK) {
        e = web_send_err(r, 400, "invalid", "ssid", "The name is 1 to 32 characters");
    } else if (pass && !cJSON_IsNull(pass) && !cJSON_IsString(pass)) {
        e = web_send_err(r, 400, "invalid", "password", "The password is text");
    } else {
        const char *p = cJSON_IsString(pass) ? pass->valuestring : "";
        if (hy_net_check_pass(p, false) != HY_OK) {
            e = web_send_err(r, 400, "invalid", "password", "The password is 8 to 63 characters, or empty for an open network");
        } else {
            esp_err_t err = hy_net_add(ssid->valuestring, p);
            if (err == ESP_ERR_NO_MEM) {
                e = web_send_err(r, 409, "full", NULL, "Five networks are saved: forget one first");
            } else if (err != ESP_OK) {
                e = web_fail(r, err == ESP_ERR_INVALID_ARG ? HY_E_INVALID : HY_E_IO);
            } else {
                e = send_networks(r, 201);
            }
        }
    }
    wipe_str(pass);
    cJSON_Delete(o);
    wipe_body(r);
    return e;
}

/* {"ssid":"HomeNet"} */
static esp_err_t h_net_del(web_req_t *r)
{
    cJSON *o = cJSON_Parse(web_body_or_empty(r));
    const cJSON *ssid = o ? cJSON_GetObjectItemCaseSensitive(o, "ssid") : NULL;
    esp_err_t e;
    if (!cJSON_IsString(ssid)) {
        e = web_send_err(r, 400, "invalid", "ssid", "Send {\"ssid\":\"name\"}");
    } else {
        esp_err_t err = hy_net_forget(ssid->valuestring);
        if (err == ESP_ERR_NOT_FOUND) {
            e = web_send_err(r, 404, "not_found", "ssid", "That network is not saved");
        } else if (err != ESP_OK) {
            e = web_fail(r, HY_E_IO);
        } else {
            e = send_networks(r, 200);
        }
    }
    cJSON_Delete(o);
    return e;
}

/* ---------------------------------------------------------------- PUT /api/wifi/ap */

static bool host_ok(const char *h)
{
    size_t n = strlen(h);
    if (n < 1 || n > 32 || h[0] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

/* {"password":"..."} sets the access point password (a running access point restarts with it), {"host":"hygeia"}
 * the host name. Either or both. The access point name is fixed. */
static esp_err_t h_ap(web_req_t *r)
{
    cJSON *o = cJSON_Parse(web_body_or_empty(r));
    const cJSON *pass = o ? cJSON_GetObjectItemCaseSensitive(o, "password") : NULL;
    const cJSON *host = o ? cJSON_GetObjectItemCaseSensitive(o, "host") : NULL;
    bool has_p = cJSON_IsString(pass), has_h = cJSON_IsString(host);
    esp_err_t e = ESP_OK;
    bool sent = false;
    if (!o || !cJSON_IsObject(o) || (!has_p && !has_h)) {
        e = web_send_err(r, 400, "invalid", NULL, "Send \"password\", \"host\" or both");
        sent = true;
    } else if (has_p && hy_net_check_pass(pass->valuestring, true) != HY_OK) {
        e = web_send_err(r, 400, "invalid", "password", "The password is 8 to 63 characters");
        sent = true;
    } else if (has_h && !host_ok(host->valuestring)) {
        e = web_send_err(r, 400, "invalid", "host", "The host name is 1 to 32 of a-z, 0-9 and -, not starting with -");
        sent = true;
    }
    if (!sent) {
        esp_err_t err = ESP_OK;
        if (has_h) {
            err = hy_net_host_set(host->valuestring);
        }
        if (err == ESP_OK && has_p) {
            err = hy_net_ap_pass_set(pass->valuestring);
        }
        if (err != ESP_OK) {
            e = web_fail(r, err == ESP_ERR_INVALID_ARG ? HY_E_INVALID : HY_E_IO);
        } else {
            hy_net_status_t st;
            hy_net_status(&st);
            cJSON *a = cJSON_CreateObject();
            if (a) {
                cJSON_AddStringToObject(a, "host", st.host);
                cJSON_AddStringToObject(a, "ap_ssid", st.ap_ssid);
            }
            e = web_send_cjson(r, 200, a);
        }
    }
    wipe_str(pass);
    cJSON_Delete(o);
    wipe_body(r);
    return e;
}

/* ---------------------------------------------------------------- route table */

web_handler_t web_wifi_route(hy_route_id_t id)
{
    switch (id) {
    case HY_R_WIFI: return h_wifi_get;
    case HY_R_WIFI_MODE: return h_wifi_mode;
    case HY_R_WIFI_NET_ADD: return h_net_add;
    case HY_R_WIFI_NET_DEL: return h_net_del;
    case HY_R_WIFI_AP: return h_ap;
    default: return NULL;
    }
}
