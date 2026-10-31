/* The pairing overlay and the web server's screen callbacks.
 *
 * Pairing: a browser asked for a code, the overlay shows it ("482 913") with the seconds left and a Cancel button, on
 * lv_layer_top above everything (as the power-off confirmation). It sets the nav pairing flag (gestures do nothing),
 * wakes the screen and holds it on while it shows. It hides when the server says so (paired, expired, five wrong
 * codes, a failed save) or on Cancel. The code is kept only while the overlay shows and is never logged.
 *
 * The other callbacks of hy_web_ui_t are the web UI's reach into the screen: toasts, the views that show a setting,
 * the power-off path and the clock. They run in the LVGL task. Everything here runs in the LVGL task. */

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "hy/web.h"
#include "ui_internal.h"

static const char *TAG = "pair";

#define FADE_MS   150
#define TICK_MS   500
#define CANCEL_W  444
#define CANCEL_H  66

static lv_obj_t *s_ov, *s_code, *s_left, *s_cancel;
static lv_timer_t *s_tmr;
static bool s_shown;
static int64_t s_expires;   /* esp_timer seconds */
static int s_left_shown = -1;

bool pair_visible(void)
{
    return s_shown;
}

static void opa_exec(void *o, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
}

static int seconds_left(void)
{
    int64_t left = s_expires - esp_timer_get_time() / 1000000;
    return left < 0 ? 0 : (int)left;
}

static void left_render(void)
{
    int left = seconds_left();
    if (left == s_left_shown) {
        return;
    }
    s_left_shown = left;
    lv_label_set_text_fmt(s_left, HY_S_PAIR_LEFT, (unsigned)left);
}

static void tmr_cb(lv_timer_t *t)
{
    (void)t;
    if (s_shown) {
        left_render();
    }
}

/* "482913" as "482 913" */
static void code_render(const char *code)
{
    char b[16];
    size_t n = strlen(code);
    if (n == 6) {
        b[0] = code[0];
        b[1] = code[1];
        b[2] = code[2];
        b[3] = ' ';
        b[4] = code[3];
        b[5] = code[4];
        b[6] = code[5];
        b[7] = '\0';
    } else {
        strlcpy(b, code, sizeof b);
    }
    lv_label_set_text(s_code, b);
    memset(b, 0, sizeof b);
}

static void close_overlay(void)
{
    if (!s_shown) {
        return;
    }
    s_shown = false;
    lv_timer_pause(s_tmr);
    lv_anim_delete(s_ov, opa_exec);
    lv_label_set_text(s_code, "");   /* the code does not stay in the widget tree */
    lv_obj_set_hidden(s_ov, true);
    ui_nav_set_pairing(false);   /* the hold ends at the next screen event: the dim timer starts then */
}

/* hy_web_ui_t.pair_show: LVGL task. A new code while one shows replaces it. The code is never logged. */
static void pair_show(const char *code, int64_t expires_mono)
{
    if (!s_ov) {
        return;
    }
    s_expires = expires_mono;
    s_left_shown = -1;
    code_render(code);
    left_render();
    if (!s_shown) {
        s_shown = true;
        ui_nav_set_pairing(true);
        screen_wake("pair");   /* a dimmed or dark screen comes back; the hold keeps it on */
        lv_display_trigger_activity(NULL);
        lv_obj_set_style_opa(s_ov, LV_OPA_TRANSP, 0);
        lv_obj_set_hidden(s_ov, false);
        lv_obj_move_foreground(s_ov);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_ov);
        lv_anim_set_exec_cb(&a, opa_exec);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a, FADE_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
        lv_timer_reset(s_tmr);
        lv_timer_resume(s_tmr);
    } else {
        screen_wake("pair");
    }
    ESP_LOGI(TAG, "pairing overlay shown");
}

/* hy_web_ui_t.pair_hide: LVGL task; reason is a hy_web_pair_end_t */
static void pair_hide(int reason)
{
    bool was = s_shown;
    close_overlay();
    ESP_LOGI(TAG, "pairing overlay hidden (reason %d)", reason);
    if (!was) {
        return;
    }
    switch (reason) {
    case HY_WEB_PAIR_DONE:
        toast_show_ok(HY_S_PAIR_DONE);
        break;
    case HY_WEB_PAIR_EXPIRED:
        toast_show_info(HY_S_PAIR_EXPIRED);
        break;
    case HY_WEB_PAIR_BLOCKED:
        toast_show_info(HY_S_PAIR_BLOCKED);
        break;
    case HY_WEB_PAIR_FAILED:
        toast_show_info(HY_S_PAIR_FAILED);
        break;
    default:
        break;
    }
}

static void cancel_tap(void *ud)
{
    (void)ud;
    hy_web_pair_cancel();   /* the running code ends; the server may call pair_hide too, which finds the overlay closed */
    close_overlay();
    ESP_LOGI(TAG, "pairing cancelled on the device");
}

/* ---- the rest of the web server's screen callbacks ---- */

/* A setting changed through the web UI (already saved and applied to the audio and the radio): the rotation lock, the
 * brightness and the views that show a setting follow. A dimmed screen keeps its idle brightness. */
static void settings_applied(const hy_settings_t *old, const hy_settings_t *now)
{
    if (now->rot_lock != old->rot_lock) {
        rotate_set_lock(now->rot_lock);
    }
    if (now->brightness != old->brightness && !idle_active()) {
        settings_brightness_live(now->brightness);
    }
    qs_values_refresh();
    settings_values_refresh();
}

static void power_off(void)
{
    poweroff_now();
}

static void time_changed(void)
{
    ui_time_changed();
}

void pair_register(void)
{
    s_ov = lv_obj_create(lv_layer_top());   /* after poweroff_create: above the top bar, the idle clock and that overlay */
    lv_obj_remove_style_all(s_ov);
    lv_obj_set_size(s_ov, HY_SCREEN, HY_SCREEN);
    lv_obj_set_pos(s_ov, 0, 0);
    lv_obj_set_style_bg_color(s_ov, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_ov, false);
    lv_obj_set_clickable(s_ov, true);   /* absorbs every touch: nothing underneath receives it */
    lv_obj_set_hidden(s_ov, true);

    lv_obj_t *t = ui_label(s_ov, hy_type.title, HY_C_TEXT, HY_S_PAIR_TITLE);
    lv_obj_set_width(t, HY_SCREEN);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(t, 0, 86);
    s_code = ui_label(s_ov, hy_type.code, HY_C_TEXT, "");
    lv_obj_set_width(s_code, HY_SCREEN);
    lv_obj_set_style_text_align(s_code, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_code, 0, 134);
    lv_obj_t *h = ui_label(s_ov, hy_type.status, HY_C_TEXT2, HY_S_PAIR_HINT);
    lv_obj_set_width(h, HY_SCREEN);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(h, 0, 224);
    s_left = ui_label(s_ov, hy_type.status, HY_C_TEXT2, "");
    lv_obj_set_width(s_left, HY_SCREEN);
    lv_obj_set_style_text_align(s_left, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_left, 0, 256);
    s_cancel = ui_button(s_ov, CANCEL_W, CANCEL_H, hy_type.body_bold, HY_S_CANCEL, cancel_tap, NULL);
    lv_obj_set_pos(s_cancel, (HY_SCREEN - CANCEL_W) / 2, 356);
    s_tmr = lv_timer_create(tmr_cb, TICK_MS, NULL);
    lv_timer_pause(s_tmr);

    static const hy_web_ui_t web_ui = {.pair_show = pair_show,
                                       .pair_hide = pair_hide,
                                       .toast_ok = toast_show_ok,
                                       .toast_info = toast_show_info,
                                       .settings_applied = settings_applied,
                                       .power_off = power_off,
                                       .time_changed = time_changed};
    hy_web_set_ui(&web_ui);
}

#if CONFIG_HY_DEBUG_CONSOLE
/* `pair show|hide`: the overlay with a fixed example code and 120 s, or closed. The server is not involved. */
void pair_debug(const char *arg)
{
    if (strcmp(arg, "show") == 0) {
        pair_show("482913", esp_timer_get_time() / 1000000 + 120);
    } else if (strcmp(arg, "hide") == 0) {
        close_overlay();
    } else {
        ESP_LOGE(TAG, "usage: pair [show|hide]");
    }
}
#endif
