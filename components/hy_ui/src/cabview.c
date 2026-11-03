/* Cabinet open on the device: the page under Settings, the trigger list, the teach view and the test view.
 * Every button acts on a still, awake tap only (ui_on_tap); the list scrolls and consumes its scroll strokes.
 * The teach flow itself (waiting for stillness, GO, captures) is the runner's; these views reflect its events and
 * play the cues. Nothing here assumes a door: the person teaches whatever movement they choose. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hy/audio.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "ui_internal.h"

static const char *TAG = "ui_cabview";

#define MARGIN   18
#define ROW_H    66
#define BTN      56
#define EDGE_X   28   /* side margin of full-width buttons near the bottom corners (HY_CORNER_R) */

/* page rows */
enum { P_ON, P_TRIGGER, P_SENS, P_WINDOW, P_CHIME, P_LIGHT, P_TEACH, P_TEST, P_FORGET, P_N };
/* what a stepper button changes: ud = row * 2 + (0 minus, 1 plus) */

static lv_obj_t *s_pg, *s_list, *s_row[P_N], *s_val[P_N], *s_fg;
static lv_obj_t *s_tr, *s_tr_row[2];
static lv_obj_t *s_te, *s_t_body, *s_t_dot[HY_CAB_TAKES], *s_t_count, *s_t_status;
static lv_obj_t *s_xt, *s_x_hint, *s_x_a, *s_x_b;
static bool s_t_running, s_t_done[HY_CAB_TAKES];
static bool s_x_on;
static int s_x_last;   /* 0 none, 1 match, 2 other */

static bool is_taught(void)
{
    hy_cab_status_t st;
    hy_cab_run_status(&st);
    return st.taught;
}

static const char *mode_text(uint8_t mode)
{
    return mode == HY_CAB_TAUGHT ? HY_S_CAB_MODE_TAUGHT : HY_S_CAB_MODE_ANY;
}

const char *cab_settings_value(void)
{
    const hy_settings_t *s = hy_settings();
    return s->cab_on ? mode_text(s->cab_mode) : HY_S_OFF;
}

/* ---- saving ---- */

static void cab_save(const hy_settings_t *n)
{
    uint8_t old_win = hy_settings()->cab_window_min;
    esp_err_t err = hy_settings_update(n);
    hy_cab_run_config(hy_settings());
    if (hy_settings()->cab_window_min != old_win) {
        hy_sched_run_invalidate();   /* the attention instant depends on the window */
    }
    ESP_LOGI(TAG, "cabinet settings saved: %s (on %d, mode %u, chime %d, light %d, sens %u, window %u)",
             esp_err_to_name(err), hy_settings()->cab_on, (unsigned)hy_settings()->cab_mode, hy_settings()->cab_chime,
             hy_settings()->cab_light, (unsigned)hy_settings()->cab_sens, (unsigned)hy_settings()->cab_window_min);
    if (err != ESP_OK) {
        toast_show_info(HY_S_ERR_IO);
    }
}

/* ---- the page ---- */

static void win_text(char *out, size_t len, unsigned min)
{
    if (min == 0) {
        snprintf(out, len, "%s", HY_S_CAB_WIN_OVERDUE);
    } else {
        snprintf(out, len, HY_S_CAB_WIN_MIN, min);
    }
}

static void page_refresh(void)
{
    const hy_settings_t *s = hy_settings();
    char b[32];
    lv_label_set_text(s_val[P_ON], s->cab_on ? HY_S_ON : HY_S_OFF);
    lv_label_set_text(s_val[P_TRIGGER], mode_text(s->cab_mode));
    snprintf(b, sizeof b, HY_S_CAB_SENS_VAL, (unsigned)s->cab_sens);
    lv_label_set_text(s_val[P_SENS], b);
    win_text(b, sizeof b, s->cab_window_min);
    lv_label_set_text(s_val[P_WINDOW], b);
    lv_label_set_text(s_val[P_CHIME], s->cab_chime ? HY_S_ON : HY_S_OFF);
    lv_label_set_text(s_val[P_LIGHT], s->cab_light ? HY_S_ON : HY_S_OFF);
    bool taught = is_taught();
    lv_label_set_text(s_val[P_TEACH], taught ? HY_S_CAB_TAUGHT : HY_S_CAB_NOT_TAUGHT);
    lv_obj_set_hidden(s_row[P_FORGET], !taught);
}

static void trig_refresh(void)
{
    uint8_t mode = hy_settings()->cab_mode;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = s_tr_row[i];
        lv_obj_set_style_text_color(lv_obj_get_child(row, 0), lv_color_hex(mode == i ? HY_C_OK : HY_C_TEXT), 0);
    }
    lv_label_set_text(lv_obj_get_child(s_tr_row[1], 1), is_taught() ? HY_S_CAB_TAUGHT : HY_S_CAB_NOT_TAUGHT);
}

static void toggle_tap(void *ud)
{
    hy_settings_t n = *hy_settings();
    switch ((int)(uintptr_t)ud) {
    case P_ON:
        n.cab_on = !n.cab_on;
        break;
    case P_CHIME:
        n.cab_chime = !n.cab_chime;
        break;
    case P_LIGHT:
        n.cab_light = !n.cab_light;
        break;
    default:
        return;
    }
    cab_save(&n);
    page_refresh();
}

static void step_tap(void *ud)
{
    int code = (int)(uintptr_t)ud, row = code / 2, dir = (code & 1) ? 1 : -1;
    hy_settings_t n = *hy_settings();
    if (row == P_SENS) {
        int v = (int)n.cab_sens + dir;
        n.cab_sens = (uint8_t)(v < 1 ? 1 : v > 5 ? 5 : v);
    } else if (row == P_WINDOW) {
        int v = (int)n.cab_window_min + 5 * dir;
        n.cab_window_min = (uint8_t)(v < 0 ? 0 : v > 120 ? 120 : v);
    } else {
        return;
    }
    cab_save(&n);
    page_refresh();
}

static void page_nav_tap(void *ud)
{
    switch ((int)(uintptr_t)ud) {
    case P_TRIGGER:
        ui_nav_push(HY_V_CAB_TRIGGER);
        break;
    case P_TEACH:
        ui_nav_push(HY_V_CAB_TEACH);
        break;
    case P_TEST:
        if (hy_settings()->cab_mode == HY_CAB_TAUGHT && !is_taught()) {
            toast_show_info(HY_S_CAB_TEACH_FIRST);
            return;
        }
        ui_nav_push(HY_V_CAB_TEST);
        break;
    case P_FORGET:
        lv_obj_set_hidden(s_fg, false);
        break;
    default:
        break;
    }
}

static void list_scroll_cb(lv_event_t *e)
{
    (void)e;
    ui_input_consume();   /* a stroke that scrolls the list must not also navigate */
}

static lv_obj_t *simple_row(int idx, const char *title, ui_action_cb cb)
{
    lv_obj_t *v = ui_list_row(s_list, ROW_H, title, cb, (void *)(uintptr_t)idx);
    s_val[idx] = v;
    s_row[idx] = lv_obj_get_parent(v);
    return v;
}

/* Title above the value on the left, the minus and plus buttons on the right */
static void stepper_row(int idx, const char *title)
{
    lv_obj_t *v = ui_list_row(s_list, ROW_H, title, NULL, NULL);
    lv_obj_t *row = lv_obj_get_parent(v);
    lv_obj_t *t = lv_obj_get_child(row, 0);
    lv_obj_set_style_text_font(t, hy_type.status, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(HY_C_TEXT2), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 22, 7);
    lv_obj_set_style_text_font(v, hy_type.body, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(HY_C_TEXT), 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(v, 220);
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, 22, 31);
    lv_obj_t *m = ui_button(row, BTN, BTN, hy_type.title, HY_S_CAB_MINUS, step_tap, (void *)(uintptr_t)(idx * 2));
    lv_obj_align(m, LV_ALIGN_RIGHT_MID, -(8 + BTN + 8), 0);
    lv_obj_t *p = ui_button(row, BTN, BTN, hy_type.title, HY_S_PLUS, step_tap, (void *)(uintptr_t)(idx * 2 + 1));
    lv_obj_align(p, LV_ALIGN_RIGHT_MID, -8, 0);
    s_val[idx] = v;
    s_row[idx] = row;
}

static void forget_yes_tap(void *ud)
{
    (void)ud;
    esp_err_t err = hy_cab_run_forget();
    ESP_LOGI(TAG, "taught movement forgotten: %s", esp_err_to_name(err));
    lv_obj_set_hidden(s_fg, true);
    page_refresh();
    if (err != ESP_OK) {
        toast_show_info(HY_S_ERR_IO);
    }
}

static void forget_no_tap(void *ud)
{
    (void)ud;
    lv_obj_set_hidden(s_fg, true);
}

static void page_create(void)
{
    s_pg = ui_view_box();
    ui_register_view(HY_V_CAB, s_pg);
    ui_view_title(s_pg, HY_S_SET_CABINET);

    s_list = lv_obj_create(s_pg);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, HY_SCREEN, HY_CONTENT_H - 44);
    lv_obj_set_pos(s_list, 0, 44);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_list, 12, 0);
    lv_obj_set_style_pad_left(s_list, MARGIN, 0);
    lv_obj_set_style_pad_bottom(s_list, MARGIN, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    ui_scroller(s_list);
    lv_obj_add_event_cb(s_list, list_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);

    simple_row(P_ON, HY_S_SET_CABINET, toggle_tap);
    simple_row(P_TRIGGER, HY_S_CAB_TRIGGER, page_nav_tap);
    stepper_row(P_SENS, HY_S_CAB_SENS);
    stepper_row(P_WINDOW, HY_S_CAB_WINDOW);
    simple_row(P_CHIME, HY_S_CAB_CHIME, toggle_tap);
    simple_row(P_LIGHT, HY_S_CAB_LIGHT, toggle_tap);
    simple_row(P_TEACH, HY_S_CAB_TEACH, page_nav_tap);
    simple_row(P_TEST, HY_S_CAB_TEST, page_nav_tap);
    simple_row(P_FORGET, HY_S_CAB_FORGET, page_nav_tap);

    /* the forget confirmation covers the page */
    s_fg = lv_obj_create(s_pg);
    lv_obj_remove_style_all(s_fg);
    lv_obj_set_size(s_fg, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_style_bg_color(s_fg, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(s_fg, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_fg, false);
    lv_obj_set_clickable(s_fg, true);
    lv_obj_t *q = ui_label(s_fg, hy_type.title, HY_C_TEXT, HY_S_CAB_FORGET_Q);
    lv_obj_set_width(q, HY_SCREEN - 2 * EDGE_X - 20);
    lv_label_set_long_mode(q, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_t *y = ui_button(s_fg, HY_SCREEN - 2 * EDGE_X, ROW_H, hy_type.body_bold, HY_S_CAB_FORGET_YES, forget_yes_tap,
                            NULL);
    lv_obj_set_pos(y, EDGE_X, 200);
    lv_obj_set_style_text_color(lv_obj_get_child(y, 0), lv_color_hex(HY_C_RED), 0);
    lv_obj_t *k = ui_button(s_fg, HY_SCREEN - 2 * EDGE_X, ROW_H, hy_type.body_bold, HY_S_CAB_FORGET_NO, forget_no_tap,
                            NULL);
    lv_obj_set_pos(k, EDGE_X, 278);
    lv_obj_set_hidden(s_fg, true);
}

/* ---- the trigger list ---- */

static void trig_tap(void *ud)
{
    uint8_t mode = (uint8_t)(uintptr_t)ud;
    if (mode == HY_CAB_TAUGHT && !is_taught()) {
        toast_show_info(HY_S_CAB_TEACH_FIRST);   /* the mode stays as it was */
        return;
    }
    hy_settings_t n = *hy_settings();
    n.cab_mode = mode;
    cab_save(&n);
    ui_nav_pop();
}

static void trig_create(void)
{
    s_tr = ui_view_box();
    ui_register_view(HY_V_CAB_TRIGGER, s_tr);
    ui_view_title(s_tr, HY_S_CAB_TRIGGER);
    static const char *const names[2] = {HY_S_CAB_MODE_ANY, HY_S_CAB_MODE_TAUGHT};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *v = ui_list_row(s_tr, ROW_H, names[i], trig_tap, (void *)(uintptr_t)i);
        s_tr_row[i] = lv_obj_get_parent(v);
        lv_obj_set_pos(s_tr_row[i], MARGIN, 64 + i * (ROW_H + 12));
    }
}

/* ---- the teach view ---- */

static void dots_refresh(int cur)   /* cur: the take in progress (0 based), -1 none */
{
    for (int i = 0; i < HY_CAB_TAKES; i++) {
        lv_obj_t *d = s_t_dot[i];
        lv_obj_set_style_bg_opa(d, s_t_done[i] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(d, lv_color_hex(i == cur && !s_t_done[i] ? HY_C_TEXT : HY_C_TEXT2), 0);
    }
}

static void t_status(const char *text, uint32_t color)
{
    lv_label_set_text(s_t_status, text);
    lv_obj_set_style_text_color(s_t_status, lv_color_hex(color), 0);
}

static void t_count(unsigned take)
{
    lv_label_set_text_fmt(s_t_count, HY_S_CAB_T_OF, take);
    lv_obj_set_hidden(s_t_count, false);
}

/* idle: the instructions and the empty dots; running: the status line, the counter and the dots */
static void teach_reset(void)
{
    s_t_running = false;
    memset(s_t_done, 0, sizeof s_t_done);
    dots_refresh(-1);
    lv_obj_set_hidden(s_t_body, false);
    lv_obj_set_hidden(s_t_status, true);
    lv_obj_set_hidden(s_t_count, true);
}

static void t_start_tap(void *ud)
{
    (void)ud;
    if (s_t_running) {
        return;
    }
    esp_err_t err = hy_cab_run_teach(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "teach did not start: %s", esp_err_to_name(err));
        toast_show_info(HY_S_CAB_T_STOPPED);
        return;
    }
    memset(s_t_done, 0, sizeof s_t_done);
    dots_refresh(0);
    s_t_running = true;
    lv_obj_set_hidden(s_t_body, true);
    t_status("", HY_C_TEXT);
    lv_obj_set_hidden(s_t_status, false);
}

static void t_cancel_tap(void *ud)
{
    (void)ud;
    hy_cab_run_teach(false);
    s_t_running = false;
    ui_nav_pop();
}

static void teach_event(const hy_cabev_t *e)
{
    if (!s_t_running) {
        return;
    }
    int cur = e->take >= 1 && e->take <= HY_CAB_TAKES ? e->take - 1 : -1;
    switch (e->kind) {
    case HY_CABEV_TEACH_STILL:
        t_count(e->take);
        dots_refresh(cur);
        t_status(HY_S_CAB_T_STILL, HY_C_TEXT2);
        break;
    case HY_CABEV_TEACH_GO:
        t_status(HY_S_CAB_T_NOW, HY_C_OK);
        hy_audio_tone(HY_TONE_GO);   /* at the set volume, whatever the sound and do not disturb switches say */
        break;
    case HY_CABEV_TEACH_TAKE:
        if (cur >= 0) {
            s_t_done[cur] = true;
        }
        t_count(e->take);
        dots_refresh(-1);
        t_status(HY_S_CAB_T_GOT, HY_C_OK);
        hy_audio_tone(HY_TONE_TAKE);
        break;
    case HY_CABEV_TOO_SMALL:
        t_status(HY_S_CAB_T_SMALL, HY_C_AMBER);
        break;
    case HY_CABEV_TOO_LONG:
        t_status(HY_S_CAB_T_LONG, HY_C_AMBER);
        break;
    case HY_CABEV_TEACH_ODD:
        if (cur >= 0) {
            s_t_done[cur] = false;
        }
        t_count(e->take);
        dots_refresh(cur);
        t_status(HY_S_CAB_T_ODD, HY_C_AMBER);
        break;
    case HY_CABEV_TEACH_DONE:
        for (int i = 0; i < HY_CAB_TAKES; i++) {
            s_t_done[i] = true;
        }
        t_count(HY_CAB_TAKES);
        dots_refresh(-1);
        t_status(HY_S_CAB_T_SAVED, HY_C_OK);
        s_t_running = false;
        break;
    case HY_CABEV_TEACH_FAIL:
        t_status(e->take == HY_CAB_FAIL_NO_MOVE ? HY_S_CAB_T_NOMOVE : e->take == HY_CAB_FAIL_SAVE ? HY_S_ERR_IO :
                 HY_S_CAB_T_STOPPED, HY_C_AMBER);
        s_t_running = false;
        break;
    default:
        break;
    }
}

static lv_obj_t *wide_button(lv_obj_t *parent, int32_t x, int32_t w, const char *text, ui_action_cb cb)
{
    lv_obj_t *b = ui_button(parent, w, ROW_H, hy_type.body_bold, text, cb, NULL);
    lv_obj_set_pos(b, x, 346);
    return b;
}

static void teach_create(void)
{
    s_te = ui_view_box();
    ui_register_view(HY_V_CAB_TEACH, s_te);
    ui_view_title(s_te, HY_S_CAB_TEACH);

    s_t_body = ui_label(s_te, hy_type.body, HY_C_TEXT, HY_S_CAB_T_BODY);
    lv_obj_set_width(s_t_body, HY_SCREEN - 2 * EDGE_X);
    lv_label_set_long_mode(s_t_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(s_t_body, EDGE_X, 72);

    const int32_t dot = 24, gap = 24;
    int32_t x0 = (HY_SCREEN - (HY_CAB_TAKES * dot + (HY_CAB_TAKES - 1) * gap)) / 2;
    for (int i = 0; i < HY_CAB_TAKES; i++) {
        lv_obj_t *d = ui_plain(s_te);
        lv_obj_set_size(d, dot, dot);
        lv_obj_set_pos(d, x0 + i * (dot + gap), 212);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 2, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(HY_C_OK), 0);
        lv_obj_set_style_border_color(d, lv_color_hex(HY_C_TEXT2), 0);
        s_t_dot[i] = d;
    }
    s_t_count = ui_label(s_te, hy_type.time, HY_C_TEXT, "");
    lv_obj_set_width(s_t_count, HY_SCREEN);
    lv_obj_set_style_text_align(s_t_count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_t_count, 0, 156);
    s_t_status = ui_label(s_te, hy_type.title, HY_C_TEXT, "");
    lv_obj_set_width(s_t_status, HY_SCREEN - 2 * EDGE_X);
    lv_label_set_long_mode(s_t_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_t_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_t_status, EDGE_X, 262);

    int32_t w = (HY_SCREEN - 2 * EDGE_X - 12) / 2;
    wide_button(s_te, EDGE_X, w, HY_S_CAB_T_START, t_start_tap);
    wide_button(s_te, EDGE_X + w + 12, w, HY_S_CANCEL, t_cancel_tap);
    teach_reset();
}

/* ---- the test view ---- */

static void test_counts(void)
{
    hy_cab_status_t st;
    hy_cab_run_status(&st);
    bool any = st.mode == HY_CAB_ANY;
    lv_label_set_text_fmt(s_x_a, any ? HY_S_CAB_X_ANY : HY_S_CAB_X_MATCHES, (unsigned)st.test_matches);
    lv_label_set_text_fmt(s_x_b, HY_S_CAB_X_OTHER, (unsigned)st.test_others);
    lv_obj_set_hidden(s_x_b, any);
    /* the counter that moved last shows it */
    lv_obj_set_style_text_color(s_x_a, lv_color_hex(s_x_last == 1 ? HY_C_OK : HY_C_TEXT), 0);
    lv_obj_set_style_text_color(s_x_b, lv_color_hex(s_x_last == 2 ? HY_C_AMBER : HY_C_TEXT), 0);
}

static void test_start(void)
{
    s_x_last = 0;
    esp_err_t err = hy_cab_run_test(true);
    s_x_on = err == ESP_OK;
    if (!s_x_on) {
        ESP_LOGW(TAG, "test mode did not start: %s", esp_err_to_name(err));
    }
    test_counts();
}

static void test_event(const hy_cabev_t *e)
{
    if (!s_x_on) {
        return;
    }
    switch (e->kind) {
    case HY_CABEV_MATCH:
        s_x_last = 1;
        hy_audio_tone(HY_TONE_TAKE);   /* the low tone, at the set volume whatever the switches say */
        break;
    case HY_CABEV_OTHER:
        s_x_last = 2;
        break;
    default:
        return;
    }
    test_counts();
}

static void test_create(void)
{
    s_xt = ui_view_box();
    ui_register_view(HY_V_CAB_TEST, s_xt);
    ui_view_title(s_xt, HY_S_CAB_TEST);
    s_x_hint = ui_label(s_xt, hy_type.body, HY_C_TEXT2, HY_S_CAB_X_HINT);
    lv_obj_set_pos(s_x_hint, EDGE_X, 80);
    s_x_a = ui_label(s_xt, hy_type.time, HY_C_TEXT, "");
    lv_obj_set_pos(s_x_a, EDGE_X, 150);
    s_x_b = ui_label(s_xt, hy_type.time, HY_C_TEXT, "");
    lv_obj_set_pos(s_x_b, EDGE_X, 210);
}

/* ---- entry points ---- */

void cab_create(void)
{
    page_create();
    trig_create();
    teach_create();
    test_create();
}

void cab_show(hy_view_t v)
{
    switch (v) {
    case HY_V_CAB:
        lv_obj_set_hidden(s_fg, true);
        page_refresh();
        break;
    case HY_V_CAB_TRIGGER:
        trig_refresh();
        break;
    case HY_V_CAB_TEACH:
        teach_reset();
        break;
    case HY_V_CAB_TEST:
        test_start();
        break;
    default:
        break;
    }
}

void cab_hide(hy_view_t next)
{
    (void)next;
    if (s_t_running) {
        hy_cab_run_teach(false);
        s_t_running = false;
    }
    if (s_x_on) {
        hy_cab_run_test(false);
        s_x_on = false;
    }
}

void cab_values_refresh(void)
{
    switch (ui_current_view()) {
    case HY_V_CAB:
        page_refresh();
        break;
    case HY_V_CAB_TRIGGER:
        trig_refresh();
        break;
    default:
        break;
    }
}

void cab_event(const hy_cabev_t *e)
{
    if (!s_pg) {
        return;   /* the views are not built yet */
    }
    switch (e->kind) {
    case HY_CABEV_TEACH_STILL:
    case HY_CABEV_TEACH_GO:
    case HY_CABEV_TEACH_TAKE:
    case HY_CABEV_TEACH_ODD:
    case HY_CABEV_TEACH_DONE:
    case HY_CABEV_TEACH_FAIL:
    case HY_CABEV_TOO_SMALL:
    case HY_CABEV_TOO_LONG:
        teach_event(e);
        break;
    case HY_CABEV_MATCH:
    case HY_CABEV_OTHER:
    case HY_CABEV_TRIGGER:
        test_event(e);
        break;
    default:
        break;
    }
}
