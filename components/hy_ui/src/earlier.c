/* Log earlier: two rollers (hour 00-23, minute 00-55 in steps of 5, default now rounded down to 5 minutes), a
 * line "Today 07:35" or "Yesterday 22:10" and a "Log at 07:35" button. A time after now means yesterday. The
 * entry is a deliberate one: no too-soon check. The rollers consume their strokes. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "hy/actions.h"
#include "hy/clock.h"
#include "hy/tzlock.h"
#include "ui_internal.h"

static const char *TAG = "ui_earlier";

#define STEP_MIN 5

static lv_obj_t *s_root, *s_name, *s_line, *s_hour, *s_min, *s_btn_lbl;
static uint32_t s_med_id;

/* The instant of hour:minute today, or yesterday when that is after now, in the clock zone */
static bool resolve(int hour, int minute, int64_t *ts, bool *yesterday)
{
    const hy_clock_port_t *ck = hy_clock_port();
    if (!ck->valid(NULL)) {
        return false;
    }
    time_t now = (time_t)ck->now(NULL);
    struct tm tm;
    hy_tz_lock();                 /* the process TZ is hy_clock's zone outside this lock */
    localtime_r(&now, &tm);
    bool y = hour * 60 + minute > tm.tm_hour * 60 + tm.tm_min;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;
    if (y) {
        tm.tm_mday -= 1;
    }
    tm.tm_isdst = -1;
    *ts = (int64_t)mktime(&tm);
    hy_tz_unlock();
    *yesterday = y;
    return true;
}

static int sel_hour(void) { return (int)lv_roller_get_selected(s_hour); }
static int sel_minute(void) { return (int)lv_roller_get_selected(s_min) * STEP_MIN; }

static void update(void)
{
    char hhmm[8], t[40];
    snprintf(hhmm, sizeof hhmm, "%02d:%02d", sel_hour(), sel_minute());
    int64_t ts;
    bool y = false;
    resolve(sel_hour(), sel_minute(), &ts, &y);
    snprintf(t, sizeof t, y ? HY_S_YESTERDAY_AT : HY_S_TODAY_AT, hhmm);
    lv_label_set_text(s_line, t);
    snprintf(t, sizeof t, HY_S_LOG_AT, hhmm);
    lv_label_set_text(s_btn_lbl, t);
}

static void roller_changed_cb(lv_event_t *e)
{
    (void)e;
    update();
}

static void consume_cb(lv_event_t *e)
{
    (void)e;
    ui_input_consume();   /* turning a roller is the roller's stroke */
}

static void btn_click_cb(lv_event_t *e)
{
    (void)e;
    if (!ui_input_last_was_tap()) {
        return;   /* a waking stroke or a swipe that ended on the button */
    }
    earlier_submit();
}

void earlier_submit(void)
{
    if (toast_logged_visible()) {
        ESP_LOGI(TAG, "log ignored: logged toast is showing");
        return;
    }
    const hy_med_t *m = ui_find_med(s_med_id);
    if (!m || m->archived) {
        toast_show_info(HY_S_ERR_NOT_FOUND);
        return;
    }
    int64_t ts;
    bool y;
    if (!resolve(sel_hour(), sel_minute(), &ts, &y)) {
        toast_show_info(ui_err_text(HY_E_CLOCK));
        return;
    }
    char code[3];
    snprintf(code, sizeof code, "%s", m->code);   /* m is invalid after an action */
    char tz[HY_TZ_LEN];
    hy_clock_tz_copy(tz, sizeof tz);
    uint32_t id = 0;
    hy_err_t err = hy_act_log_taken(s_med_id, ts, NULL, HY_SRC_SCREEN, &id);
    ESP_LOGI(TAG, "log_taken med %s at %02d:%02d%s: %s (%d), log id %u", code, sel_hour(), sel_minute(),
             y ? " yesterday" : "", hy_err_str(err), (int)err, (unsigned)id);
    if (err != HY_OK) {
        toast_show_info(ui_err_text(err));
        return;
    }
    char hhmm[8];
    ui_fmt_hhmm(ts, tz, hhmm, sizeof hhmm);
    toast_show_logged(code, hhmm, id);
    ui_log_hwm("log earlier");
    ui_nav_pop();
}

void earlier_show(uint32_t med_id)
{
    s_med_id = med_id;
    const hy_med_t *m = ui_find_med(med_id);
    lv_label_set_text(s_name, m ? m->name : "");
    int h = 0, mi = 0;
    const hy_clock_port_t *ck = hy_clock_port();
    if (ck->valid(NULL)) {
        char tz[HY_TZ_LEN], hhmm[8];
        hy_clock_tz_copy(tz, sizeof tz);
        ui_fmt_hhmm(ck->now(NULL), tz, hhmm, sizeof hhmm);
        sscanf(hhmm, "%d:%d", &h, &mi);
    }
    lv_roller_set_selected(s_hour, (uint32_t)h, LV_ANIM_OFF);
    lv_roller_set_selected(s_min, (uint32_t)(mi / STEP_MIN), LV_ANIM_OFF);
    update();
}

#if CONFIG_HY_DEBUG_CONSOLE
void earlier_debug_set(int hour, int minute)
{
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        ESP_LOGE(TAG, "earlier: %d:%d is not a time", hour, minute);
        return;
    }
    lv_roller_set_selected(s_hour, (uint32_t)hour, LV_ANIM_OFF);
    lv_roller_set_selected(s_min, (uint32_t)(minute / STEP_MIN), LV_ANIM_OFF);
    update();
}
#endif

static lv_obj_t *make_roller(const char *opts, int x)
{
    lv_obj_t *r = lv_roller_create(s_root);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(r, 5);
    lv_obj_set_width(r, 120);
    lv_obj_set_pos(r, x, 92);
    lv_obj_set_style_text_font(r, hy_type.body, 0);
    lv_obj_set_style_text_color(r, lv_color_hex(HY_C_TEXT2), 0);
    lv_obj_set_style_text_line_space(r, 12, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(HY_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 22, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_outline_width(r, 0, 0);
    lv_obj_set_style_text_font(r, hy_type.time, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(HY_C_TEXT), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r, lv_color_hex(HY_C_SURFACE2), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_add_event_cb(r, consume_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(r, consume_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(r, consume_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(r, roller_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return r;
}

void earlier_create(void)
{
    s_root = ui_view_box();
    ui_register_view(HY_V_EARLIER, s_root);

    s_name = ui_label(s_root, hy_type.title, HY_C_TEXT, "");
    lv_obj_set_pos(s_name, 24, 2);
    s_line = ui_label(s_root, hy_type.body, HY_C_TEXT2, "");
    lv_obj_set_width(s_line, HY_SCREEN);
    lv_obj_set_style_text_align(s_line, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_line, 0, 48);

    char hours[24 * 3 + 1], mins[12 * 3 + 1];
    size_t o = 0;
    for (int h = 0; h < 24; h++) {
        o += (size_t)snprintf(hours + o, sizeof hours - o, "%s%02d", h ? "\n" : "", h);
    }
    o = 0;
    for (int m = 0; m < 60; m += STEP_MIN) {
        o += (size_t)snprintf(mins + o, sizeof mins - o, "%s%02d", m ? "\n" : "", m);
    }
    s_hour = make_roller(hours, 108);
    s_min = make_roller(mins, 252);
    lv_obj_update_layout(s_hour);
    lv_obj_t *colon = ui_label(s_root, hy_type.time, HY_C_TEXT, ":");
    lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, 92 + lv_obj_get_height(s_hour) / 2 - lv_font_get_line_height(hy_type.time) / 2);

    lv_obj_t *btn = lv_obj_create(s_root);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, HY_SCREEN - 36, 66);
    lv_obj_set_pos(btn, 18, 340);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(HY_C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(HY_C_GRAB), LV_STATE_PRESSED);
    lv_obj_set_scrollable(btn, false);
    lv_obj_add_event_cb(btn, btn_click_cb, LV_EVENT_SHORT_CLICKED, NULL);
    s_btn_lbl = ui_label(btn, hy_type.body_bold, HY_C_TEXT, "");
    lv_obj_center(s_btn_lbl);
}
