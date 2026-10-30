#include <stdio.h>

#include "esp_log.h"
#include "hy/actions.h"
#include "ui_internal.h"

static const char *TAG = "ui_toast";

#define TOAST_MS (HY_UNDO_WINDOW_S * 1000)
#define ICON 44
#define UNDO_W 140

enum { MODE_NONE, MODE_LOGGED, MODE_INFO };

static lv_obj_t *s_scrim, *s_toast, *s_icon, *s_check, *s_bang, *s_text, *s_undo_area, *s_undo_lbl;
static lv_timer_t *s_timer;
static int s_mode;
static uint32_t s_log_id;
static uint32_t s_start;
static int s_batt_pending = -1;   /* percent of a held low-battery toast, -1 none */

static const lv_point_precise_t check_pts[] = {{12, 23}, {19, 30}, {32, 15}};

static void hide(void)
{
    s_mode = MODE_NONE;
    lv_obj_set_hidden(s_toast, true);
    lv_obj_set_hidden(s_scrim, true);
    lv_timer_pause(s_timer);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t el = lv_tick_elaps(s_start);
    if (el >= TOAST_MS) {
        hide();
        ui_battery_toast_flush();   /* the Undo window ended */
        return;
    }
    if (s_mode == MODE_LOGGED) {
        lv_label_set_text_fmt(s_undo_lbl, HY_S_UNDO, (unsigned)((TOAST_MS - el + 999) / 1000));
    }
}

static void undo_click_cb(lv_event_t *e)
{
    (void)e;
    if (ui_input_waking()) {
        return;   /* the stroke only woke the dimmed screen */
    }
    toast_undo_pressed();
}

void toast_create(lv_obj_t *parent)
{
    s_scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, HY_SCREEN, HY_SCREEN - HY_BAR_H);   /* the top bar stays bright */
    lv_obj_set_pos(s_scrim, 0, HY_BAR_H);
    lv_obj_set_style_bg_color(s_scrim, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_60, 0);
    lv_obj_set_clickable(s_scrim, true);   /* absorbs taps while the logged toast shows */
    lv_obj_set_scrollable(s_scrim, false);
    lv_obj_set_hidden(s_scrim, true);
    lv_obj_set_floating(s_scrim, true);

    s_toast = lv_obj_create(parent);
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, HY_TOAST_W, HY_TOAST_H);
    lv_obj_set_pos(s_toast, (HY_SCREEN - HY_TOAST_W) / 2, HY_TOAST_Y);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(HY_C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 28, 0);
    lv_obj_set_scrollable(s_toast, false);
    lv_obj_set_clickable(s_toast, false);
    lv_obj_set_hidden(s_toast, true);
    lv_obj_set_floating(s_toast, true);

    s_icon = lv_obj_create(s_toast);
    lv_obj_remove_style_all(s_icon);
    lv_obj_set_size(s_icon, ICON, ICON);
    lv_obj_align(s_icon, LV_ALIGN_LEFT_MID, 22, 0);
    lv_obj_set_style_radius(s_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_icon, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_icon, false);
    lv_obj_set_scrollable(s_icon, false);

    s_check = lv_line_create(s_icon);
    lv_line_set_points(s_check, check_pts, 3);
    lv_obj_set_style_line_width(s_check, 4, 0);
    lv_obj_set_style_line_color(s_check, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_line_rounded(s_check, true, 0);

    s_bang = lv_label_create(s_icon);
    lv_label_set_text(s_bang, HY_S_TOAST_BANG);
    lv_obj_set_style_text_font(s_bang, hy_type.body_bold, 0);
    lv_obj_set_style_text_color(s_bang, lv_color_hex(HY_C_BG), 0);
    lv_obj_center(s_bang);

    s_text = lv_label_create(s_toast);
    lv_obj_set_style_text_font(s_text, hy_type.body, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(HY_C_TEXT), 0);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_MODE_WRAP);

    s_undo_area = lv_obj_create(s_toast);
    lv_obj_remove_style_all(s_undo_area);
    lv_obj_set_size(s_undo_area, UNDO_W, HY_TOAST_H);
    lv_obj_align(s_undo_area, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_scrollable(s_undo_area, false);
    lv_obj_set_clickable(s_undo_area, true);
    lv_obj_add_event_cb(s_undo_area, undo_click_cb, LV_EVENT_CLICKED, NULL);
    s_undo_lbl = lv_label_create(s_undo_area);
    lv_obj_set_style_text_font(s_undo_lbl, hy_type.body_bold, 0);
    lv_obj_set_style_text_color(s_undo_lbl, lv_color_hex(HY_C_AMBER), 0);
    lv_label_set_text_fmt(s_undo_lbl, HY_S_UNDO, (unsigned)HY_UNDO_WINDOW_S);
    lv_obj_align(s_undo_lbl, LV_ALIGN_RIGHT_MID, -22, 0);

    s_timer = lv_timer_create(tick_cb, 100, NULL);
    lv_timer_pause(s_timer);
}

static void open(int mode)
{
    s_mode = mode;
    s_start = lv_tick_get();
    lv_obj_set_hidden(s_toast, false);
    lv_obj_move_foreground(s_toast);
    lv_timer_reset(s_timer);
    lv_timer_resume(s_timer);
}

/* shared by the logged and the skipped toast: Undo, the scrim, the 5 s window */
static void show_undoable(uint32_t log_id, uint32_t icon_color, const char *glyph)
{
    s_log_id = log_id;
    lv_obj_set_style_bg_color(s_icon, lv_color_hex(icon_color), 0);
    lv_obj_set_hidden(s_check, glyph != NULL);
    lv_obj_set_hidden(s_bang, glyph == NULL);
    if (glyph) {
        lv_label_set_text(s_bang, glyph);
        lv_obj_center(s_bang);
    }
    lv_obj_set_width(s_text, HY_TOAST_W - 22 - ICON - 16 - UNDO_W);
    lv_obj_align(s_text, LV_ALIGN_LEFT_MID, 22 + ICON + 16, 0);
    lv_label_set_text_fmt(s_undo_lbl, HY_S_UNDO, (unsigned)HY_UNDO_WINDOW_S);
    lv_obj_set_hidden(s_undo_area, false);
    lv_obj_set_hidden(s_scrim, false);
    lv_obj_move_foreground(s_scrim);
    open(MODE_LOGGED);
}

void toast_show_logged(const char *code, const char *hhmm, uint32_t log_id)
{
    lv_label_set_text_fmt(s_text, HY_S_LOGGED, code, hhmm);
    show_undoable(log_id, HY_C_OK, NULL);
}

void toast_show_skipped(const char *code, const char *hhmm, uint32_t log_id)
{
    lv_label_set_text_fmt(s_text, HY_S_SKIPPED, code, hhmm);
    show_undoable(log_id, HY_C_TEXT2, HY_S_RSAQ);
}

void toast_show_info(const char *text)
{
    lv_obj_set_style_bg_color(s_icon, lv_color_hex(HY_C_AMBER), 0);
    lv_obj_set_hidden(s_check, true);
    lv_obj_set_hidden(s_bang, false);
    lv_label_set_text(s_bang, HY_S_TOAST_BANG);
    lv_obj_center(s_bang);
    lv_label_set_text(s_text, text);
    lv_obj_set_width(s_text, HY_TOAST_W - 22 - ICON - 16 - 22);
    lv_obj_align(s_text, LV_ALIGN_LEFT_MID, 22 + ICON + 16, 0);
    lv_obj_set_hidden(s_undo_area, true);
    lv_obj_set_hidden(s_scrim, true);
    open(MODE_INFO);
}

void toast_show_ok(const char *text)
{
    lv_obj_set_style_bg_color(s_icon, lv_color_hex(HY_C_OK), 0);
    lv_obj_set_hidden(s_check, false);
    lv_obj_set_hidden(s_bang, true);
    lv_label_set_text(s_text, text);
    lv_obj_set_width(s_text, HY_TOAST_W - 22 - ICON - 16 - 22);
    lv_obj_align(s_text, LV_ALIGN_LEFT_MID, 22 + ICON + 16, 0);
    lv_obj_set_hidden(s_undo_area, true);
    lv_obj_set_hidden(s_scrim, true);
    open(MODE_INFO);
}

void ui_battery_toast(int pct)
{
    s_batt_pending = pct;
    ui_battery_toast_flush();
}

void ui_battery_toast_flush(void)
{
    if (s_batt_pending < 0 || s_mode == MODE_LOGGED || idle_active() || poweroff_visible() ||
        poweroff_countdown_visible()) {
        return;
    }
    char b[48];
    snprintf(b, sizeof b, HY_S_BATT_LOW, s_batt_pending);
    s_batt_pending = -1;
    toast_show_info(b);
}

bool toast_undo_pressed(void)
{
    if (s_mode != MODE_LOGGED) {
        return false;
    }
    hy_err_t err = hy_act_undo(s_log_id);
    ESP_LOGI(TAG, "undo log %u: %s (%d)", (unsigned)s_log_id, hy_err_str(err), (int)err);
    hide();
    ui_log_hwm("undo");
    if (err == HY_E_UNDO_EXPIRED) {
        toast_show_info(HY_S_UNDO_EXPIRED);
    } else {
        ui_battery_toast_flush();   /* the Undo toast is gone */
    }
    return true;
}

bool toast_logged_visible(void)
{
    return s_mode == MODE_LOGGED;
}
