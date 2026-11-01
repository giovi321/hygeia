/* Firmware update from GitHub: GET /api/update, POST /api/update/check, POST /api/update/install.
 *
 * The handlers run in the httpd task and call hy_update_* directly: that module has its own lock and its own task,
 * so nothing goes through the LVGL bridge. Status changes reach the open browsers as {"ev":"update",...} live events
 * (subscribed once at server start), and GET /api/update answers the same state for a page that was opened later. */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hy/fwupdate.h"
#include "hy/update.h"
#include "web_internal.h"

static const char *TAG = "web_update";

#define STATUS_JSON_MAX 512

static esp_err_t send_status(web_req_t *r, int status)
{
    hy_fw_status_t st;
    char buf[STATUS_JSON_MAX];
    hy_update_status(&st);
    if (hy_fw_status_json(&st, esp_timer_get_time() / 1000000, buf, sizeof buf) < 0) {
        return web_send_err(r, 500, "internal", NULL, "The update status did not fit");
    }
    return web_send_json(r, status, buf);
}

/* An immediate refusal: the status code follows the error, 503 answers carry Retry-After */
static esp_err_t send_refusal(web_req_t *r, hy_fw_err_t e)
{
    int status;
    switch (e) {
    case HY_FWE_SELF_PENDING:
    case HY_FWE_MEMORY:
        status = 503;
        httpd_resp_set_hdr(r->req, "Retry-After", "2");
        break;
    default:
        status = 409;   /* no_network, busy, battery, stale */
        break;
    }
    return web_send_err(r, status, hy_fw_err_code(e), NULL, hy_fw_err_message(e));
}

static esp_err_t h_update_get(web_req_t *r)
{
    return send_status(r, 200);
}

static esp_err_t h_update_check(web_req_t *r)
{
    hy_fw_err_t e = hy_update_check();
    if (e != HY_FWE_NONE) {
        return send_refusal(r, e);
    }
    return send_status(r, 202);
}

static esp_err_t h_update_install(web_req_t *r)
{
    cJSON *o = cJSON_Parse(web_body_or_empty(r));
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, "version") : NULL;
    if (!cJSON_IsString(v) || !v->valuestring) {
        cJSON_Delete(o);
        return web_send_err(r, 400, "invalid", "version", "Send the version to install as {\"version\":\"vX.Y.Z\"}");
    }
    char version[32];
    snprintf(version, sizeof version, "%s", v->valuestring);
    bool cut = strlen(v->valuestring) >= sizeof version;
    cJSON_Delete(o);
    /* A name that does not fit can never equal a checked tag */
    hy_fw_err_t e = cut ? HY_FWE_STALE : hy_update_install(version);
    if (e != HY_FWE_NONE) {
        return send_refusal(r, e);
    }
    return send_status(r, 202);
}

/* Update task: encode and queue only */
static void on_status(const hy_fw_status_t *st, void *ctx)
{
    (void)ctx;
    char msg[192];
    if (hy_fw_event_json(st, msg, sizeof msg) > 0) {
        web_ws_push(msg);
    }
}

void web_update_start(void)
{
    if (hy_update_subscribe(on_status, NULL) < 0) {
        ESP_LOGE(TAG, "update subscribe failed: the page falls back to polling");
    }
}

web_handler_t web_update_route(hy_route_id_t id)
{
    switch (id) {
    case HY_R_UPDATE: return h_update_get;
    case HY_R_UPDATE_CHECK: return h_update_check;
    case HY_R_UPDATE_INSTALL: return h_update_install;
    default: return NULL;
    }
}
