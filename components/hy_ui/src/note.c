/* Log with note: a one-line text area (at most 63 bytes) above the on-screen keyboard and a Log button. Log
 * records a taken dose now with the note, returns to the detail and shows the logged toast. The keyboard and the
 * text area consume their strokes, so a key press never becomes a gesture. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hy/actions.h"
#include "hy/clock.h"
#include "ui_internal.h"

static const char *TAG = "ui_note";

LV_FONT_DECLARE(lv_font_montserrat_14);

static lv_obj_t *s_root, *s_name, *s_ta, *s_kb;
static uint32_t s_med_id;
static char s_prev[HY_NOTE_MAX * 4 + 1];
static lv_font_t s_kb_font;   /* Inter 22 with Montserrat as the fallback for the keyboard's symbol keys */

/* Cuts s to at most max bytes on a UTF-8 boundary */
static void truncate_utf8(char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max) {
        return;
    }
    while (max > 0 && ((uint8_t)s[max] & 0xC0) == 0x80) {
        max--;
    }
    s[max] = 0;
}

/* The text area holds characters, the note is limited in bytes: cut what the typing added past the limit */
static void ta_changed_cb(lv_event_t *e)
{
    (void)e;
    const char *t = lv_textarea_get_text(s_ta);
    if (strlen(t) > HY_NOTE_MAX) {
        char b[HY_NOTE_MAX * 4 + 1];
        snprintf(b, sizeof b, "%s", t);
        truncate_utf8(b, HY_NOTE_MAX);
        lv_textarea_set_text(s_ta, b);
    }
}

static void consume_cb(lv_event_t *e)
{
    (void)e;
    ui_input_consume();   /* a key press or a text selection is the widget's stroke */
}

static void log_click_cb(lv_event_t *e)
{
    (void)e;
    if (!ui_input_last_was_tap()) {
        return;   /* a waking stroke or a swipe that ended on the button */
    }
    note_submit();
}

/* The keyboard's OK key and the text area's enter: the same as Log, but never for a stroke that only woke the
 * dimmed screen */
static void ready_cb(lv_event_t *e)
{
    (void)e;
    if (ui_input_waking()) {
        ESP_LOGI(TAG, "OK ignored: the stroke only woke the screen");
        return;
    }
    note_submit();
}

/* The keyboard's close key: back to the detail */
static void cancel_cb(lv_event_t *e)
{
    (void)e;
    if (ui_input_waking() || ui_current_view() != HY_V_NOTE) {
        return;
    }
    ui_nav_pop();
}

/* The text before the press, so a stroke that only woke the screen can be taken back after the keyboard typed it */
static void kb_press_cb(lv_event_t *e)
{
    snprintf(s_prev, sizeof s_prev, "%s", lv_textarea_get_text(s_ta));
    consume_cb(e);
}

static void kb_value_cb(lv_event_t *e)
{
    (void)e;
    if (ui_input_waking()) {
        lv_textarea_set_text(s_ta, s_prev);
    }
}

void note_submit(void)
{
    if (ui_current_view() != HY_V_NOTE) {
        return;   /* a second event of the same key press, after the view closed */
    }
    if (toast_logged_visible()) {
        ESP_LOGI(TAG, "log ignored: logged toast is showing");
        return;
    }
    const hy_med_t *m = ui_find_med(s_med_id);
    if (!m || m->archived) {
        toast_show_info(HY_S_ERR_NOT_FOUND);
        return;
    }
    const hy_clock_port_t *ck = hy_clock_port();
    bool valid = ck->valid(NULL);
    int64_t now = valid ? ck->now(NULL) : 0;
    char tz[HY_TZ_LEN];
    hy_clock_tz_copy(tz, sizeof tz);
    if (home_too_soon_hold(m, valid, now)) {
        return;   /* R29: the same as-needed warning as a tile tap, a second Log within 5 s logs */
    }
    char note[HY_NOTE_MAX * 4 + 1];
    snprintf(note, sizeof note, "%s", lv_textarea_get_text(s_ta));
    truncate_utf8(note, HY_NOTE_MAX);
    char code[3];
    snprintf(code, sizeof code, "%s", m->code);   /* m is invalid after an action */
    uint32_t id = 0;
    hy_err_t err = hy_act_log_taken(s_med_id, 0, note[0] ? note : NULL, HY_SRC_SCREEN, &id);
    ESP_LOGI(TAG, "log_taken med %s note \"%s\": %s (%d), log id %u", code, note, hy_err_str(err), (int)err,
             (unsigned)id);
    if (err != HY_OK) {
        toast_show_info(ui_err_text(err));
        return;
    }
    char hhmm[8];
    ui_fmt_hhmm(now, tz, hhmm, sizeof hhmm);
    toast_show_logged(code, hhmm, id);
    ui_log_hwm("log note");
    ui_nav_pop();
}

void note_show(uint32_t med_id)
{
    s_med_id = med_id;
    const hy_med_t *m = ui_find_med(med_id);
    lv_label_set_text(s_name, m ? m->name : "");
    lv_textarea_set_text(s_ta, "");
    s_prev[0] = 0;
}

#if CONFIG_HY_DEBUG_CONSOLE
void note_debug_ok(void)
{
    ready_cb(NULL);
}

void note_debug_text(const char *text)
{
    lv_textarea_set_text(s_ta, text);
    ta_changed_cb(NULL);   /* the value-changed event the typing would send */
}
#endif

void note_create(void)
{
    s_root = ui_view_box();
    ui_register_view(HY_V_NOTE, s_root);

    s_kb_font = *hy_type.body;
    s_kb_font.fallback = &lv_font_montserrat_14;
    s_name = ui_label(s_root, hy_type.title, HY_C_TEXT, "");
    lv_obj_set_pos(s_name, 24, 2);

    s_ta = lv_textarea_create(s_root);
    lv_obj_set_size(s_ta, HY_SCREEN - 36, 56);
    lv_obj_set_pos(s_ta, 18, 56);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, HY_NOTE_MAX);
    lv_textarea_set_placeholder_text(s_ta, HY_S_NOTE_HINT);
    lv_obj_set_style_text_font(s_ta, hy_type.body, 0);
    lv_obj_set_style_text_color(s_ta, lv_color_hex(HY_C_TEXT), 0);
    lv_obj_set_style_text_color(s_ta, lv_color_hex(HY_C_TEXT2), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(s_ta, lv_color_hex(HY_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ta, 22, 0);
    lv_obj_set_style_border_width(s_ta, 0, 0);
    lv_obj_set_style_shadow_width(s_ta, 0, 0);
    lv_obj_set_style_outline_width(s_ta, 0, 0);
    lv_obj_set_style_pad_hor(s_ta, 18, 0);
    lv_obj_set_style_pad_ver(s_ta, 14, 0);
    lv_obj_set_style_border_width(s_ta, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_side(s_ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_border_color(s_ta, lv_color_hex(HY_C_TEXT), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(s_ta, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_add_event_cb(s_ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_ta, consume_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ta, consume_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_ta, ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_ta, cancel_cb, LV_EVENT_CANCEL, NULL);

    lv_obj_t *log = lv_obj_create(s_root);
    lv_obj_remove_style_all(log);
    lv_obj_set_size(log, HY_SCREEN - 36, 66);
    lv_obj_set_pos(log, 18, 128);
    lv_obj_set_style_radius(log, 22, 0);
    lv_obj_set_style_bg_color(log, lv_color_hex(HY_C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(log, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(log, lv_color_hex(HY_C_GRAB), LV_STATE_PRESSED);
    lv_obj_set_scrollable(log, false);
    lv_obj_add_event_cb(log, log_click_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_center(ui_label(log, hy_type.body_bold, HY_C_TEXT, HY_S_NOTE_LOG));

    s_kb = lv_keyboard_create(s_root);
    lv_obj_set_size(s_kb, HY_SCREEN, HY_CONTENT_H / 2);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb, s_ta);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kb, 0, 0);
    lv_obj_set_style_radius(s_kb, 0, 0);
    lv_obj_set_style_pad_all(s_kb, 6, 0);
    lv_obj_set_style_pad_gap(s_kb, 6, 0);
    lv_obj_set_style_text_font(s_kb, &s_kb_font, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, lv_color_hex(HY_C_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(HY_C_SURFACE2), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_kb, 12, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(HY_C_FILL), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(HY_C_FILL), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_kb, lv_color_hex(HY_C_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_kb, kb_press_cb, LV_EVENT_PRESSED | LV_EVENT_PREPROCESS, NULL);   /* before the key's own handler */
    lv_obj_add_event_cb(s_kb, consume_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_kb, kb_value_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* READY and CANCEL go to the keyboard and then to its textarea: handle them on the textarea only */
}
