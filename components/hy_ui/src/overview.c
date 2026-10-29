/* Overview page (pager page 1): today's count ring, one row per dose of today and a 24 h timeline.
 *
 * Everything here runs in the LVGL task. The scheduler snapshot is read only from overview_refresh(), which
 * runs when the evaluation generation changed while the page is shown, or when the page becomes visible. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "hy/sched_run.h"
#include "hy/summary.h"
#include "ui_internal.h"

static const char *TAG = "ui_overview";

#define RING_D       130
#define RING_W       12
#define RING_X       24
#define RING_Y       44
#define ROWS_X       178
#define ROWS_Y       50
#define ROWS_W       (HY_SCREEN - ROWS_X - 22)
#define ROW_H        30
#define ROWS_H       (4 * ROW_H)
#define DOT_D        10
#define TL_Y         191          /* timeline y on the page, from the mockup */
#define TL_X0        24
#define TL_W         432
#define TL_DOT       20
#define TL_NOW_H     60
#define MAX_ROWS     64

static lv_obj_t *s_page, *s_arc, *s_count, *s_rows, *s_tl;
static lv_obj_t *s_now;
static uint32_t s_gen_seen;
static uint32_t s_sig;
static bool s_drawn;
static uint32_t s_last_us;
static hy_today_row_t s_trows[MAX_ROWS];

static const lv_point_precise_t s_check_pts[] = {{0, 6}, {4, 11}, {12, 1}};

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void rows_scroll_cb(lv_event_t *e)
{
    (void)e;
    ui_input_consume();   /* a stroke that scrolls the rows must not also page */
}

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

static const hy_med_t *med_of(const hy_snapshot_t *sn, uint32_t id)
{
    for (size_t i = 0; i < sn->in.n_meds; i++) {
        if (sn->in.meds[i].id == id) {
            return &sn->in.meds[i];
        }
    }
    return NULL;
}

/* minutes after local midnight of an "HH:MM" string */
static int minutes_of(const char *hhmm)
{
    int h = 0, m = 0;
    if (sscanf(hhmm, "%d:%d", &h, &m) != 2) {
        return -1;
    }
    return h * 60 + m;
}

static void add_row(const hy_snapshot_t *sn, const hy_today_row_t *r, const hy_med_t *m)
{
    lv_color_t col = hy_med_color(m->color);
    lv_obj_t *row = plain(s_rows);
    lv_obj_set_size(row, ROWS_W, ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *dot = plain(row);
    lv_obj_set_size(dot, DOT_D, DOT_D);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_margin_right(dot, 9, 0);
    if (r->as_needed) {
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(HY_C_TEXT2), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_set_style_bg_color(dot, col, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }

    char text[48], hhmm[8];
    uint32_t color = HY_C_TEXT;
    bool check = false;
    if (r->as_needed) {
        color = HY_C_TEXT2;
        if (r->taken_today > 0) {
            char tail[32];
            snprintf(tail, sizeof tail, HY_S_OV_AS_NEEDED_N, (unsigned)r->taken_today);
            snprintf(text, sizeof text, "%s %s", m->code, tail);
        } else {
            snprintf(text, sizeof text, "%s %s", m->code, HY_S_OV_AS_NEEDED);
        }
    } else {
        int64_t at = r->state == HY_OCC_TAKEN && r->ts > 0 ? r->ts : r->slot;
        ui_fmt_hhmm(at, sn->in.tz, hhmm, sizeof hhmm);
        const char *word = NULL;
        switch (r->state) {
        case HY_OCC_TAKEN:     check = true; break;
        case HY_OCC_DUE:       word = HY_S_OV_DUE; color = 0; break;
        case HY_OCC_LATE:      word = HY_S_OV_LATE; color = 0; break;
        case HY_OCC_MISSED:    word = HY_S_OV_MISSED; color = HY_C_MISS; break;
        case HY_OCC_SKIPPED:   word = HY_S_OV_SKIPPED; color = HY_C_TEXT2; break;
        case HY_OCC_UNTRACKED: word = HY_S_OV_UNTRACKED; color = HY_C_TEXT2; break;
        case HY_OCC_UPCOMING:  break;
        }
        if (word) {
            snprintf(text, sizeof text, "%s %s %s", m->code, hhmm, word);
        } else {
            snprintf(text, sizeof text, "%s %s", m->code, hhmm);
        }
    }
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, hy_type.status, 0);
    lv_obj_set_style_text_color(l, color ? lv_color_hex(color) : col, 0);
    lv_label_set_text(l, text);

    if (check) {
        lv_obj_t *c = lv_line_create(row);
        lv_obj_set_size(c, 12, 12);
        lv_obj_set_style_margin_left(c, 7, 0);
        lv_obj_set_style_line_width(c, 2, 0);
        lv_obj_set_style_line_color(c, lv_color_hex(HY_C_OK), 0);
        lv_obj_set_style_line_rounded(c, true, 0);
        lv_line_set_points(c, s_check_pts, 3);
    }
}

static void add_tl_dot(const hy_snapshot_t *sn, const hy_today_row_t *r, const hy_med_t *m)
{
    char hhmm[8];
    ui_fmt_hhmm(r->slot, sn->in.tz, hhmm, sizeof hhmm);
    int min = minutes_of(hhmm);
    if (min < 0) {
        return;
    }
    lv_color_t col = hy_med_color(m->color);
    lv_obj_t *d = plain(s_tl);
    lv_obj_set_size(d, TL_DOT, TL_DOT);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(d, TL_X0 + min * TL_W / 1440 - TL_DOT / 2, TL_NOW_H / 2 - TL_DOT / 2);
    switch (r->state) {
    case HY_OCC_TAKEN:
    case HY_OCC_DUE:
    case HY_OCC_LATE:
        lv_obj_set_style_bg_color(d, col, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        break;
    case HY_OCC_UPCOMING:
    case HY_OCC_MISSED:
        lv_obj_set_style_bg_opa(d, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(d, 3, 0);
        lv_obj_set_style_border_color(d, r->state == HY_OCC_MISSED ? lv_color_hex(HY_C_MISS) : col, 0);
        break;
    case HY_OCC_SKIPPED:
    case HY_OCC_UNTRACKED:
        lv_obj_set_style_bg_color(d, lv_color_hex(HY_C_GRAB), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        break;
    }
}

static void set_now_line(const hy_snapshot_t *sn)
{
    if (!s_now) {
        return;
    }
    char hhmm[8];
    ui_fmt_hhmm(sn->in.now, sn->in.tz, hhmm, sizeof hhmm);
    int min = minutes_of(hhmm);
    if (min < 0) {
        min = 0;
    }
    lv_obj_set_x(s_now, TL_X0 + min * TL_W / 1440 - 1);
}

void overview_refresh(void)
{
    if (hy_sched_generation() == 0) {
        return;   /* no evaluation yet */
    }
    int64_t t0 = esp_timer_get_time();
    const hy_snapshot_t *sn = hy_sched_snapshot();
    s_gen_seen = hy_sched_generation();

    uint16_t taken = 0, total = 0;
    size_t n = 0;
    if (sn->tracking) {
        n = hy_today_rows(&sn->in, sn->sched, s_trows, MAX_ROWS, &taken, &total);
    }
    set_now_line(sn);

    /* what the rows and the timeline show: rebuild them only when it changed */
    uint32_t sig = 2166136261u;
    sig = fnv(sig, &taken, sizeof taken);
    sig = fnv(sig, &total, sizeof total);
    for (size_t i = 0; i < n; i++) {
        const hy_today_row_t *r = &s_trows[i];
        const hy_med_t *m = med_of(sn, r->med_id);
        sig = fnv(sig, r, sizeof *r);
        if (m) {
            sig = fnv(sig, m->code, sizeof m->code);
            sig = fnv(sig, &m->color, sizeof m->color);
        }
    }
    if (s_drawn && sig == s_sig) {
        s_last_us = (uint32_t)(esp_timer_get_time() - t0);
        return;
    }
    s_sig = sig;
    s_drawn = true;

    char buf[16];
    snprintf(buf, sizeof buf, "%u/%u", (unsigned)taken, (unsigned)total);
    lv_label_set_text(s_count, buf);
    lv_arc_set_value(s_arc, total ? (int32_t)taken * HY_RING_MAX / total : 0);

    lv_obj_clean(s_rows);
    lv_obj_clean(s_tl);
    s_now = NULL;
    for (size_t i = 0; i < n; i++) {
        const hy_med_t *m = med_of(sn, s_trows[i].med_id);
        if (!m) {
            continue;
        }
        add_row(sn, &s_trows[i], m);
        if (!s_trows[i].as_needed) {
            add_tl_dot(sn, &s_trows[i], m);
        }
    }
    /* the now line last, above the dots; lv_obj_clean removed the old one */
    s_now = plain(s_tl);
    lv_obj_set_size(s_now, 2, TL_NOW_H);
    lv_obj_set_style_bg_color(s_now, lv_color_hex(HY_C_TEXT), 0);
    lv_obj_set_style_bg_opa(s_now, LV_OPA_COVER, 0);
    lv_obj_set_y(s_now, 0);
    set_now_line(sn);
    s_last_us = (uint32_t)(esp_timer_get_time() - t0);
    ESP_LOGI(TAG, "overview_refresh: %u rows, %u/%u doses, %u us", (unsigned)n, (unsigned)taken,
             (unsigned)total, (unsigned)s_last_us);
}

void overview_refresh_if_changed(void)
{
    if (ui_current_view() == HY_V_OVERVIEW && hy_sched_generation() != s_gen_seen) {
        overview_refresh();
    }
}

void overview_create(lv_obj_t *page)
{
    s_page = page;
    lv_obj_t *t = label(page, hy_type.title, HY_C_TEXT, HY_S_OV_TITLE);
    lv_obj_set_pos(t, 24, 2);

    lv_obj_t *disc = plain(page);
    lv_obj_set_size(disc, RING_D - 2 * RING_W, RING_D - 2 * RING_W);
    lv_obj_set_pos(disc, RING_X + RING_W, RING_Y + RING_W);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(HY_C_INNER), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);

    s_arc = lv_arc_create(page);
    lv_obj_set_size(s_arc, RING_D, RING_D);
    lv_obj_set_pos(s_arc, RING_X, RING_Y);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, HY_RING_MAX);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_arc, false);
    lv_obj_set_style_pad_all(s_arc, 0, 0);
    lv_obj_set_style_arc_width(s_arc, RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(HY_C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(HY_C_OK), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, false, LV_PART_INDICATOR);

    const int cy = RING_Y + RING_D / 2;
    s_count = label(page, hy_type.count, HY_C_TEXT, "0/0");
    lv_obj_set_width(s_count, RING_D - 2 * RING_W);
    lv_obj_set_style_text_align(s_count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_count, RING_X + RING_W, cy - 6 - lv_font_get_line_height(hy_type.count) / 2);
    lv_obj_t *doses = label(page, hy_type.small, HY_C_TEXT2, HY_S_OV_DOSES);
    lv_obj_set_width(doses, RING_D - 2 * RING_W);
    lv_obj_set_style_text_align(doses, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(doses, RING_X + RING_W, cy + 26 - lv_font_get_line_height(hy_type.small) / 2);

    s_rows = lv_obj_create(page);
    lv_obj_remove_style_all(s_rows);
    lv_obj_set_size(s_rows, ROWS_W, ROWS_H);
    lv_obj_set_pos(s_rows, ROWS_X, ROWS_Y);
    lv_obj_set_flex_flow(s_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_rows, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_elastic(s_rows, false);
    lv_obj_set_scroll_momentum(s_rows, false);
    lv_obj_add_event_cb(s_rows, rows_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);

    /* timeline: the line, the labels and (rebuilt on refresh) the dots and the now line */
    lv_obj_t *line = plain(page);
    lv_obj_set_size(line, TL_W, 2);
    lv_obj_set_pos(line, TL_X0, TL_Y - 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(HY_C_TRACK), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    const int lh = lv_font_get_line_height(hy_type.small);
    lv_obj_t *l0 = label(page, hy_type.small, HY_C_TEXT2, "00");
    lv_obj_set_pos(l0, TL_X0, TL_Y + 27 - lh / 2);
    lv_obj_t *l12 = label(page, hy_type.small, HY_C_TEXT2, "12");
    lv_obj_set_pos(l12, TL_X0 + TL_W * 47 / 100, TL_Y + 27 - lh / 2);
    lv_obj_t *l24 = label(page, hy_type.small, HY_C_TEXT2, "24");
    lv_obj_align(l24, LV_ALIGN_TOP_RIGHT, -(HY_SCREEN - TL_X0 - TL_W), TL_Y + 27 - lh / 2);

    s_tl = plain(page);
    lv_obj_set_size(s_tl, HY_SCREEN, TL_NOW_H);
    lv_obj_set_pos(s_tl, 0, TL_Y - TL_NOW_H / 2);
    s_now = NULL;
}

void overview_timing(uint32_t *us)
{
    *us = s_last_us;
}
