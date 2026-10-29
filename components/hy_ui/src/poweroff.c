/* Power-off confirmation and critical-battery countdown: two modes of one full-screen overlay on lv_layer_top
 * (above the top bar and the idle clock).
 *
 * Confirmation (PWR long): title, an amber warning when a dose is due within 24 h or open now, the next dose, a slide
 * track and Cancel. Completing the slide powers off. Cancel, PWR short or 30 s without a touch close it.
 * Countdown (critical battery): title, "Powering off in N s", a hint. No slider, no Cancel: only USB cancels it.
 * At 0 it powers off; with fakebatt active it only logs and closes.
 *
 * Both set the nav poweroff flag (gestures do nothing) and the idle hold, and wake a dimmed screen.
 * Everything runs in the LVGL task. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "hy/power_run.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "hy/summary.h"
#include "ui_internal.h"

static const char *TAG = "poweroff";

#define TRACK_X       26
#define TRACK_Y       250
#define TRACK_W       (HY_SCREEN - 2 * TRACK_X)
#define TRACK_H       88
#define KNOB_D        72
#define KNOB_PAD      ((TRACK_H - KNOB_D) / 2)
#define TRAVEL        (TRACK_W - 2 * KNOB_PAD - KNOB_D)
#define COMPLETE_PCT  90
#define CANCEL_Y      370
#define CANCEL_H      64
#define FADE_MS       150
#define BACK_MS       150
#define TIMEOUT_MS    30000
#define COUNTDOWN_S   60
#define TICK_MS       500
#define DUE_HORIZON_S (24 * 3600)

typedef enum { M_NONE, M_CONFIRM, M_COUNTDOWN } mode_t_;

static mode_t_ s_mode;
static lv_obj_t *s_ov, *s_title, *s_line1, *s_line2, *s_track, *s_knob, *s_cancel;
static lv_timer_t *s_tmr;
static uint32_t s_cd_t0;     /* lv_tick at the countdown start */
static int s_cd_shown = -1;  /* seconds on the label */

/* knob drag */
static bool s_drag;
static int32_t s_x0, s_off0, s_off;

static const lv_point_precise_t power_line[] = {{36, 18}, {36, 37}};

static void opa_exec(void *o, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
}

static void x_exec(void *o, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)o, v);
}

static void knob_reset(void)
{
    lv_anim_delete(s_knob, x_exec);
    lv_obj_set_x(s_knob, KNOB_PAD);
    s_off = 0;
    s_drag = false;
}

bool poweroff_visible(void)
{
    return s_mode == M_CONFIRM;
}

bool poweroff_countdown_visible(void)
{
    return s_mode == M_COUNTDOWN;
}

/* ---- open and close ---- */

static void open_common(void)
{
    ui_nav_set_poweroff(true);
    idle_set_hold(true);
    idle_wake();   /* a dimmed screen comes back; the hold keeps the idle timer from dimming it again */
    lv_display_trigger_activity(NULL);
    knob_reset();
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
}

/* Closes either mode. No log of its own: the callers say why. */
static void close_overlay(void)
{
    s_mode = M_NONE;
    knob_reset();
    lv_timer_pause(s_tmr);
    lv_anim_delete(s_ov, opa_exec);
    lv_obj_set_hidden(s_ov, true);
    ui_nav_set_poweroff(false);
    idle_set_hold(false);
    ui_battery_toast_flush();   /* a low-battery toast held while the overlay was up */
}

static bool warn_needed(void)
{
    const hy_clock_port_t *ck = hy_clock_port();
    if (!ck->valid(NULL)) {
        return false;
    }
    const hy_snapshot_t *sn = hy_sched_snapshot();
    if (!sn->tracking || !hy_sched_generation()) {
        return false;
    }
    return sn->out.n_open > 0 || hy_due_within(sn->in.meds, sn->in.n_meds, sn->in.tz, ck->now(NULL), DUE_HORIZON_S);
}

void poweroff_show(void)
{
    if (!s_ov || s_mode == M_COUNTDOWN) {
        if (s_mode == M_COUNTDOWN) {
            ESP_LOGI(TAG, "pwr long ignored: countdown running");
        }
        return;
    }
    if (s_mode == M_CONFIRM) {
        lv_display_trigger_activity(NULL);   /* already up: restart the 30 s */
        return;
    }
    bool warn = warn_needed();
    char next[40];
    bool has_next = idle_next_text(next, sizeof next);
    lv_label_set_text(s_title, HY_S_POWEROFF_TITLE);
    lv_obj_set_pos(s_title, 0, 96);
    lv_label_set_text(s_line1, warn ? HY_S_POWEROFF_WARN : "");
    lv_obj_set_style_text_font(s_line1, hy_type.status, 0);
    lv_obj_set_style_text_color(s_line1, lv_color_hex(HY_C_AMBER), 0);
    lv_obj_set_pos(s_line1, 0, 140);
    lv_label_set_text(s_line2, has_next ? next : "");
    lv_obj_set_pos(s_line2, 0, 170);
    lv_obj_set_hidden(s_track, false);
    lv_obj_set_hidden(s_cancel, false);
    s_mode = M_CONFIRM;
    open_common();
    ESP_LOGI(TAG, "poweroff: show (warn=%d)", warn ? 1 : 0);
}

void poweroff_cancel(void)
{
    if (s_mode != M_CONFIRM) {
        return;
    }
    close_overlay();
    ESP_LOGI(TAG, "poweroff: hide");
}

static void failed(void)
{
    /* hy_power_off returned: the cut did not happen. The display is back on; restore the brightness. */
    idle_brightness_stop();
    hy_board_set_brightness(hy_settings()->brightness);
    lv_obj_invalidate(lv_screen_active());
    close_overlay();
    ESP_LOGE(TAG, "poweroff: failed, still running");
    toast_show_info(HY_S_POWEROFF_FAILED);
}

static void power_off_now(void)
{
    hy_power_off();   /* does not return when the cut works */
    failed();
}

/* ---- countdown ---- */

static void countdown_render(int left)
{
    if (left == s_cd_shown) {
        return;
    }
    s_cd_shown = left;
    char b[40];
    snprintf(b, sizeof b, HY_S_CRIT_LINE, (unsigned)left);
    lv_label_set_text(s_line1, b);
}

void poweroff_countdown_show(void)
{
    if (!s_ov || s_mode == M_COUNTDOWN) {
        return;
    }
    /* a confirmation already up is taken over: open_common resets the knob and the timer */
    lv_label_set_text(s_title, HY_S_CRIT_TITLE);
    lv_obj_set_pos(s_title, 0, 120);
    lv_obj_set_style_text_font(s_line1, hy_type.body, 0);
    lv_obj_set_style_text_color(s_line1, lv_color_hex(HY_C_TEXT), 0);
    lv_obj_set_pos(s_line1, 0, 176);
    lv_label_set_text(s_line2, HY_S_CRIT_HINT);
    lv_obj_set_pos(s_line2, 0, 220);
    lv_obj_set_hidden(s_track, true);
    lv_obj_set_hidden(s_cancel, true);
    s_mode = M_COUNTDOWN;
    s_cd_t0 = lv_tick_get();
    s_cd_shown = -1;
    countdown_render(COUNTDOWN_S);
    open_common();
    ESP_LOGW(TAG, "poweroff: countdown start (%d s)", COUNTDOWN_S);
}

static void countdown_cancel_usb(void)
{
    close_overlay();
    ESP_LOGI(TAG, "poweroff: countdown cancelled (usb)");
}

void poweroff_usb(bool in)
{
    if (in && s_mode == M_COUNTDOWN) {
        countdown_cancel_usb();
    }
}

static void countdown_done(void)
{
    if (hy_power_fake_active()) {
        close_overlay();
        ESP_LOGI(TAG, "poweroff: countdown done (fake, not powering off)");
        return;
    }
    ESP_LOGW(TAG, "poweroff: countdown done");
    power_off_now();
}

static void tmr_cb(lv_timer_t *t)
{
    (void)t;
    if (s_mode == M_CONFIRM) {
        if (!s_drag && lv_display_get_inactive_time(NULL) >= TIMEOUT_MS) {
            ESP_LOGI(TAG, "poweroff: timeout");
            poweroff_cancel();
        }
        return;
    }
    if (s_mode != M_COUNTDOWN) {
        return;
    }
    /* A dropped usb callback must not miss the plug-in (the power task gives up after a 1 s lock timeout): read
     * the state here as well. */
    hy_power_state_t st;
    hy_power_state(&st);
    if (st.pmu_ok && st.batt.raw.vbus) {
        countdown_cancel_usb();
        return;
    }
    int left = COUNTDOWN_S - (int)(lv_tick_elaps(s_cd_t0) / 1000);
    if (left <= 0) {
        countdown_done();
        return;
    }
    countdown_render(left);
}

/* ---- key flows ---- */

void poweroff_pwr_short(void)
{
    if (s_mode == M_CONFIRM) {
        ESP_LOGI(TAG, "pwr short: closes the confirmation");
        poweroff_cancel();
        return;
    }
    screen_pwr_short();   /* also while the countdown shows: the countdown keeps running */
}

void poweroff_pwr_long(void)
{
    poweroff_show();
}

/* ---- slide ---- */

static void slide_complete(void)
{
    ESP_LOGI(TAG, "poweroff: slide complete");
    power_off_now();
}

static void drag_end(bool released)
{
    s_drag = false;
    bool complete = released && s_off * 100 >= TRAVEL * COMPLETE_PCT;
    ESP_LOGI(TAG, "poweroff: knob released at %d of %d px: %s", (int)s_off, TRAVEL, complete ? "complete" : "back");
    if (complete) {
        slide_complete();
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_knob);
    lv_anim_set_exec_cb(&a, x_exec);
    lv_anim_set_values(&a, lv_obj_get_x(s_knob), KNOB_PAD);
    lv_anim_set_duration(&a, BACK_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    s_off = 0;
}

/* As the due card knob: it owns the stroke from the first sample; a stroke that only wakes the dimmed screen
 * never moves it. */
static void knob_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_point_t p;
    switch (c) {
    case LV_EVENT_PRESSED:
        if (s_mode != M_CONFIRM || ui_input_waking()) {
            return;
        }
        ui_input_consume();
        s_drag = true;
        lv_anim_delete(s_knob, x_exec);
        s_off = lv_obj_get_x(s_knob) - KNOB_PAD;
        lv_indev_get_point(lv_event_get_indev(e), &p);
        s_x0 = p.x;
        s_off0 = s_off;
        break;
    case LV_EVENT_PRESSING: {
        if (!s_drag) {
            return;
        }
        ui_input_consume();
        lv_indev_get_point(lv_event_get_indev(e), &p);
        int32_t off = s_off0 + (p.x - s_x0);
        s_off = off < 0 ? 0 : off > TRAVEL ? TRAVEL : off;
        lv_obj_set_x(s_knob, KNOB_PAD + s_off);
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (s_drag) {
            drag_end(c == LV_EVENT_RELEASED);
        }
        break;
    default:
        break;
    }
}

static void cancel_tap(void *ud)
{
    (void)ud;
    poweroff_cancel();
}

/* ---- construction ---- */

static lv_obj_t *centred(lv_obj_t *parent, const lv_font_t *f, uint32_t color)
{
    lv_obj_t *l = ui_label(parent, f, color, "");
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, HY_SCREEN, lv_font_get_line_height(f));
    return l;
}

void poweroff_create(void)
{
    s_ov = lv_obj_create(lv_layer_top());   /* after idle_create: above the top bar and the idle clock */
    lv_obj_remove_style_all(s_ov);
    lv_obj_set_size(s_ov, HY_SCREEN, HY_SCREEN);
    lv_obj_set_pos(s_ov, 0, 0);
    lv_obj_set_style_bg_color(s_ov, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_ov, false);
    lv_obj_set_clickable(s_ov, true);   /* absorbs every touch: nothing underneath receives it */
    lv_obj_set_hidden(s_ov, true);

    s_title = centred(s_ov, hy_type.title, HY_C_TEXT);
    s_line1 = centred(s_ov, hy_type.status, HY_C_AMBER);
    s_line2 = centred(s_ov, hy_type.status, HY_C_TEXT2);

    s_track = ui_plain(s_ov);
    lv_obj_set_size(s_track, TRACK_W, TRACK_H);
    lv_obj_set_pos(s_track, TRACK_X, TRACK_Y);
    lv_obj_set_style_radius(s_track, TRACK_H / 2, 0);
    lv_obj_set_style_bg_color(s_track, lv_color_hex(HY_C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(s_track, LV_OPA_COVER, 0);
    lv_obj_center(ui_label(s_track, hy_type.body, HY_C_TEXT2, HY_S_POWEROFF_SLIDE));

    s_knob = ui_plain(s_track);
    lv_obj_set_size(s_knob, KNOB_D, KNOB_D);
    lv_obj_set_pos(s_knob, KNOB_PAD, KNOB_PAD);
    lv_obj_set_style_radius(s_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_knob, lv_color_hex(HY_C_MISS), 0);
    lv_obj_set_style_bg_opa(s_knob, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_knob, true);
    lv_obj_add_event_cb(s_knob, knob_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_knob, knob_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_knob, knob_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_knob, knob_cb, LV_EVENT_PRESS_LOST, NULL);

    /* power glyph in black: an arc of 270 degrees open at the top and a vertical line, 3 px */
    lv_obj_t *glyph = lv_arc_create(s_knob);
    lv_obj_set_size(glyph, 32, 32);
    lv_obj_center(glyph);
    lv_obj_set_clickable(glyph, false);
    lv_obj_remove_style(glyph, NULL, LV_PART_KNOB);
    lv_obj_set_style_pad_all(glyph, 0, 0);
    lv_arc_set_bg_angles(glyph, 315, 225);
    lv_obj_set_style_arc_width(glyph, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(glyph, lv_color_hex(HY_C_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(glyph, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(glyph, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_t *bar = lv_line_create(s_knob);
    lv_line_set_points(bar, power_line, 2);
    lv_obj_set_style_line_width(bar, 3, 0);
    lv_obj_set_style_line_color(bar, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_line_rounded(bar, true, 0);

    s_cancel = ui_button(s_ov, HY_SCREEN - 2 * TRACK_X, CANCEL_H, hy_type.body_bold, HY_S_CANCEL, cancel_tap, NULL);
    lv_obj_set_pos(s_cancel, TRACK_X, CANCEL_Y);

    s_tmr = lv_timer_create(tmr_cb, TICK_MS, NULL);
    lv_timer_pause(s_tmr);
}

/* ---- debug ---- */

#if CONFIG_HY_DEBUG_CONSOLE
void poweroff_debug(const char *arg)
{
    if (strcmp(arg, "show") == 0) {
        poweroff_show();
    } else if (strcmp(arg, "cancel") == 0) {
        if (s_mode != M_CONFIRM) {
            ESP_LOGW(TAG, "cancel: the confirmation is not up");
        }
        poweroff_cancel();
    } else if (strcmp(arg, "slide") == 0) {
        if (s_mode == M_NONE) {
            poweroff_show();
        }
        if (s_mode != M_CONFIRM) {
            ESP_LOGW(TAG, "slide: the confirmation is not up");
            return;
        }
        slide_complete();
    } else if (strcmp(arg, "crit") == 0) {
        poweroff_countdown_show();
    } else {
        ESP_LOGE(TAG, "usage: poweroff [show|slide|cancel|crit]");
    }
}
#endif
