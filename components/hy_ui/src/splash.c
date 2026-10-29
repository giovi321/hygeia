/* Boot splash from docs/public/assets/logo.svg (viewBox 256, drawn at 240 px, centred): a capsule of two halves
 * rotated -12 degrees drops into the gap of the ring (0-400 ms, ease-in), the ring draws from one end of the gap
 * to the other (400-900 ms, ease-out), a hold to 1000 ms, then the content screen fades in over 200 ms. */

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hy/board.h"
#include "hy/ui.h"
#include "ui_internal.h"

static const char *TAG = "splash";

#define LOGO_PX   240
#define ORIGIN    ((HY_SCREEN - LOGO_PX) / 2)
#define HOLD_MS   1000
#define FADE_MS   200
#define C_RING    0xD9DAD6
#define C_CAPSULE 0xD0646B

/* logo geometry scaled by 240/256: ring centre (128,146) r 82 stroke 32, capsule 84 x 40 at (86,14) (rounded ends
 * at x 86 and 170) with the split at x 124..132, rotated -12 degrees about its centre */
#define RING_D      184                 /* 2 * (82 * 0.9375 + 30 / 2) */
#define RING_W      30
#define RING_X      (ORIGIN + 120 - RING_D / 2)
#define RING_Y      (ORIGIN + 137 - RING_D / 2)
#define CAP_W       79
#define CAP_H       38
#define HALF_W      36
#define HALF2_X     43                  /* the split is 7 px: 36 + 7 + 36 */
#define CAP_X       (ORIGIN + 81)
#define CAP_Y       (ORIGIN + 13)
#define DROP_MS     400
#define DRAW_MS     500

static lv_obj_t *s_scr, *s_ring, *s_cap;
static int64_t s_t0_us;
static bool s_started, s_seen;
static int64_t s_last_frame_us;
static int s_max_gap_ms;

/* The splash counts from its first drawn frame, not from the call that created it */
static void first_frame_cb(lv_event_t *e)
{
    (void)e;
    if (!s_started) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!s_seen) {
        s_seen = true;
        s_t0_us = now;
    } else if (now - s_t0_us <= (DROP_MS + DRAW_MS) * 1000) {   /* frames inside the animated 900 ms */
        int gap = (int)((now - s_last_frame_us) / 1000);
        if (gap > s_max_gap_ms) {
            s_max_gap_ms = gap;
        }
    }
    s_last_frame_us = now;
}

static void cap_y_exec(void *o, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)o, v);
}

static void ring_exec(void *o, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)o, (int16_t)v);
}

static void ring_start_cb(lv_anim_t *a)
{
    lv_obj_set_hidden((lv_obj_t *)lv_anim_get_user_data(a), false);
}

static lv_obj_t *half(lv_obj_t *parent, int32_t x, int32_t pivot_x)
{
    lv_obj_t *h = ui_plain(parent);
    lv_obj_set_size(h, HALF_W, CAP_H);
    lv_obj_set_pos(h, x, 0);
    lv_obj_t *c = ui_plain(h);   /* the whole capsule; the half shows its own part through the clip */
    lv_obj_set_size(c, CAP_W, CAP_H);
    lv_obj_set_pos(c, -x, 0);
    lv_obj_set_style_radius(c, CAP_H / 2, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CAPSULE), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_pivot_x(h, pivot_x, 0);
    lv_obj_set_style_transform_pivot_y(h, CAP_H / 2, 0);
    lv_obj_set_style_transform_rotation(h, -120, 0);
    return h;
}

esp_err_t hy_ui_splash_start(void)
{
    if (!hy_board_display()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!hy_board_lock(2000)) {
        return ESP_ERR_TIMEOUT;
    }
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, HY_SCREEN, HY_SCREEN);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_scr, false);

    s_ring = lv_arc_create(s_scr);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_clickable(s_ring, false);
    lv_obj_set_size(s_ring, RING_D, RING_D);
    lv_obj_set_pos(s_ring, RING_X, RING_Y);
    lv_arc_set_range(s_ring, 0, 1000);
    lv_arc_set_rotation(s_ring, 0);
    lv_arc_set_bg_angles(s_ring, 304, 236);   /* the gap is the 68 degrees at the top */
    lv_arc_set_value(s_ring, 0);
    lv_obj_set_style_arc_opa(s_ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(C_RING), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ring, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_ring, true, LV_PART_INDICATOR);
    lv_obj_set_hidden(s_ring, true);

    s_cap = ui_plain(s_scr);
    lv_obj_set_size(s_cap, CAP_W, CAP_H);
    lv_obj_set_pos(s_cap, CAP_X, -CAP_H);
    half(s_cap, 0, CAP_W / 2);
    half(s_cap, HALF2_X, CAP_W / 2 - HALF2_X);

    lv_obj_set_style_opa(lv_layer_top(), LV_OPA_TRANSP, 0);   /* the top bar waits for the content */
    lv_screen_load(s_scr);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cap);
    lv_anim_set_exec_cb(&a, cap_y_exec);
    lv_anim_set_values(&a, -CAP_H, CAP_Y);
    lv_anim_set_duration(&a, DROP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ring);
    lv_anim_set_exec_cb(&a, ring_exec);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_delay(&a, DROP_MS);
    lv_anim_set_duration(&a, DRAW_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_user_data(&a, s_ring);
    lv_anim_set_start_cb(&a, ring_start_cb);
    lv_anim_start(&a);

    s_t0_us = esp_timer_get_time();   /* until the first frame is drawn */
    lv_display_add_event_cb(lv_display_get_default(), first_frame_cb, LV_EVENT_REFR_READY, NULL);
    s_started = true;
    hy_board_unlock();
    return ESP_OK;
}

int splash_remaining_ms(void)
{
    if (!s_started) {
        return 0;
    }
    int ms = HOLD_MS - (int)((esp_timer_get_time() - s_t0_us) / 1000);
    return ms > 0 ? ms : 0;
}

/* No LVGL lock held: the animations run in the LVGL task meanwhile */
void splash_wait(void)
{
    while (s_started && (esp_timer_get_time() - s_t0_us) / 1000 < HOLD_MS) {
        vTaskDelay(1);
    }
}

void splash_yield(void)
{
    if (!s_started) {
        return;
    }
    hy_board_unlock();
    vTaskDelay(2);
    while (!hy_board_lock(2000)) {   /* never continue without the lock: the caller unlocks once */
        ESP_LOGW(TAG, "splash: waiting for the LVGL lock");
    }
}

static void top_opa_exec(void *o, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
}

/* LVGL lock held: loads the content screen with a 200 ms fade (the top layer fades with it) */
void splash_finish(lv_obj_t *next)
{
    if (!s_started) {
        lv_screen_load(next);
        return;
    }
    s_started = false;
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), first_frame_cb, NULL);
    ESP_LOGI(TAG, "splash: done in %d ms", (int)((esp_timer_get_time() - s_t0_us) / 1000));
    ESP_LOGI(TAG, "splash: max frame gap %d ms", s_max_gap_ms);
    lv_screen_load_anim(next, LV_SCREEN_LOAD_ANIM_FADE_IN, FADE_MS, 0, true);   /* deletes the splash screen */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lv_layer_top());
    lv_anim_set_exec_cb(&a, top_opa_exec);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, FADE_MS);
    lv_anim_start(&a);
    s_scr = s_ring = s_cap = NULL;
}
