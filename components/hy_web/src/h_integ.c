/* Integration status and the webhook test: GET /api/integrations, POST /api/webhook/test.
 *
 * Both handlers run in the httpd task and call hy_integ_* directly: the status is a copy under a mutex and the test
 * only queues a request for the delivery task. A change of the status reaches the open browsers as {"ev":"integ"},
 * at most once a second (the last change wins). The message carries no data and an open page does not refetch because
 * of it, so it does not count as activity for the battery idle limit. */

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "web_internal.h"

static const char *TAG = "web_integ";

#define PUSH_GAP_US 1000000

static esp_timer_handle_t s_timer;
static atomic_bool s_pending;
static atomic_llong s_last_us;

static esp_err_t h_integ_get(web_req_t *r)
{
    hy_integ_status_t st;
    hy_integ_status(&st);
    char *js = hy_api_integ_status_json(&st);
    if (!js) {
        return web_oom(r);
    }
    esp_err_t e = web_send_json(r, 200, js);
    free(js);
    return e;
}

static esp_err_t h_webhook_test(web_req_t *r)
{
    cJSON *o = cJSON_Parse(web_body_or_empty(r));
    const cJSON *ix = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, "index") : NULL;
    bool ok = cJSON_IsNumber(ix) && ix->valuedouble == floor(ix->valuedouble) && ix->valuedouble >= 0 &&
              ix->valuedouble < HY_HOOK_MAX;
    int index = ok ? (int)ix->valuedouble : 0;
    cJSON_Delete(o);
    if (!ok) {
        return web_send_err(r, 400, "invalid", "index", "Send the webhook number as {\"index\":0} to {\"index\":3}");
    }
    switch (hy_integ_test((uint8_t)index)) {
    case ESP_OK:
        return web_send_json(r, 202, "{\"started\":true}");
    case ESP_ERR_INVALID_ARG:
        return web_send_err(r, 404, "not_found", "index", "No webhook is saved at that number");
    case ESP_ERR_INVALID_STATE:
        return web_send_err(r, 409, "offline", NULL, "Connect the device to a Wi-Fi network first");
    case ESP_ERR_NO_MEM:
        return web_send_err(r, 409, "memory", NULL, "The device is short of memory. Try again in a minute");
    case ESP_ERR_NOT_FINISHED:
        return web_send_err(r, 409, "busy", NULL, "A test or an update, upload, export or import is running");
    default:
        return web_send_err(r, 500, "internal", NULL, "The test could not be started");
    }
}

/* Delivery task, then the timer task: encode and queue only */
static void push_now(void)
{
    atomic_store(&s_last_us, esp_timer_get_time());
    web_ws_push("{\"ev\":\"integ\"}");
}

static void on_timer(void *arg)
{
    (void)arg;
    atomic_store(&s_pending, false);
    push_now();
}

static void on_integ(void *ctx)
{
    (void)ctx;
    int64_t wait = PUSH_GAP_US - (esp_timer_get_time() - atomic_load(&s_last_us));
    if (wait <= 0 && !atomic_load(&s_pending)) {
        push_now();
        return;
    }
    if (!atomic_exchange(&s_pending, true) && esp_timer_start_once(s_timer, wait > 1000 ? (uint64_t)wait : 1000) != ESP_OK) {
        atomic_store(&s_pending, false);
    }
}

void web_integ_start(void)
{
    const esp_timer_create_args_t a = {.callback = on_timer, .name = "web_integ"};
    if (esp_timer_create(&a, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "timer not created: the page polls for a test result");
        return;
    }
    if (hy_integ_subscribe(on_integ, NULL) < 0) {
        ESP_LOGE(TAG, "integ subscribe failed: the page polls for a test result");
    }
}

web_handler_t web_integ_route(hy_route_id_t id)
{
    switch (id) {
    case HY_R_INTEG: return h_integ_get;
    case HY_R_WEBHOOK_TEST: return h_webhook_test;
    default: return NULL;
    }
}
