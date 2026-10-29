#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hy/board.h"
#include "hy/events.h"
#include "hy/sched_run.h"
#include "hy/ui.h"
#include "ui_internal.h"

static const char *TAG = "ui";

static lv_obj_t *s_screen;
static bool s_refresh_queued, s_rebuild_queued;

static void async_refresh(void *arg)
{
    (void)arg;
    bool rebuild = s_rebuild_queued;
    s_refresh_queued = s_rebuild_queued = false;
    hy_sched_run_tick();   /* the event marked the runner dirty: evaluate before redrawing */
    if (rebuild) {
        home_rebuild();
    } else {
        home_refresh_if_changed();
    }
}

/* Events arrive in the LVGL task (actions run there); defer the redraw out of the action's call stack. */
static void event_cb(const hy_event_t *ev, void *ctx)
{
    (void)ctx;
    if (ev->type == HY_EV_BATTERY_LOW || ev->type == HY_EV_BATTERY_CRITICAL) {
        return;
    }
    if (ev->type == HY_EV_MEDS_CHANGED) {
        s_rebuild_queued = true;
    }
    if (!s_refresh_queued) {
        s_refresh_queued = true;
        lv_async_call(async_refresh, NULL);
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    hy_sched_run_tick();
    home_refresh_if_changed();
}

void ui_log_hwm(const char *what)
{
    static const char *seen[4];
    for (size_t i = 0; i < 4; i++) {
        if (seen[i] && strcmp(seen[i], what) == 0) {
            return;
        }
    }
    for (size_t i = 0; i < 4; i++) {
        if (!seen[i]) {
            seen[i] = what;
            break;
        }
    }
    ESP_LOGI(TAG, "stack margin after first %s: task \"%s\" never below %u bytes free",
             what, pcTaskGetName(NULL), (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

esp_err_t hy_ui_start(void)
{
    if (!hy_board_display()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!hy_board_lock(2000)) {
        return ESP_ERR_TIMEOUT;
    }
    hy_sched_run_tick();   /* the first snapshot, so the tiles never start from an empty one */
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, HY_SCREEN, HY_SCREEN);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_screen, false);
    home_create(s_screen);
    toast_create(s_screen);
    lv_screen_load(s_screen);
    lv_timer_create(tick_cb, 1000, NULL);
    int sub = hy_ev_subscribe(event_cb, NULL);
    hy_board_unlock();

    if (sub < 0) {
        ESP_LOGE(TAG, "event subscribe failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "home screen up");
    return ESP_OK;
}
