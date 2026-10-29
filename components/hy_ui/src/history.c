/* History page (pager page 2): a grid of day marks per medicine for the last 7 or 30 days, a legend and the
 * adherence over the shown range.
 *
 * Everything here runs in the LVGL task. The scheduler snapshot is read only from history_refresh(), which
 * runs when the evaluation generation changed while the page is shown, when the page becomes visible, or
 * after a range toggle (queued with lv_async_call, because the toggle runs inside a row's own event and
 * the refresh deletes the rows). A double tap anywhere toggles the range; a single tap on a row opens that
 * medicine's detail after 300 ms unless a second tap followed. */

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hy/sched_run.h"
#include "hy/summary.h"
#include "ui_internal.h"

static const char *TAG = "ui_history";

#define MAX_DAYS     30
#define TAP_WAIT_MS  300
#define CODE_X       24
#define CODE_W       40
#define ROWS_Y       78
#define ROWS_H       148
#define INIT_Y       56            /* weekday initials and tick labels, centre */
#define LEGEND_Y     237           /* legend centre */
#define ADH_Y        280           /* adherence centre */
#define SQ           10            /* legend square */

typedef struct {
    int cell, radius, pitch_x, x0, row_h, row_pitch, dot;
} geom_t;

static const geom_t G7 = {.cell = 34, .radius = 9, .pitch_x = 48, .x0 = CODE_X + CODE_W + 14,
                          .row_h = 34, .row_pitch = 44, .dot = 8};
static const geom_t G30 = {.cell = 10, .radius = 3, .pitch_x = 12, .x0 = CODE_X + CODE_W,
                           .row_h = 28, .row_pitch = 30, .dot = 4};

static lv_obj_t *s_page, *s_bg, *s_title, *s_rows, *s_adh;
static lv_obj_t *s_wd[7], *s_tick[5];
static int s_days = 7;
static bool s_force;
static bool s_drawn;
static uint32_t s_gen_seen, s_sig;
static uint32_t s_view_us, s_lvgl_us;
static lv_timer_t *s_tap_timer;
static uint32_t s_tap_med;
static bool s_pressed;
#define HBUF_CAP      512            /* one medicine's logs over the 32 days of the 30 day range */
#define HBUF_WINDOW_S (32 * 86400)
static hy_log_t *s_hbuf;             /* PSRAM */

/* Past days only change when a log, a medicine, the zone or the tracking start changes, and hy_day_mark costs
 * about 4 ms a call (measured), so everything is recomputed only when the recompute key changes (today, zone,
 * last zone change, tracked-since per medicine, medicine schedules, log count and newest log, range), on a
 * range toggle and after any hy event (history_invalidate). A show or a minute tick recomputes today's column. */
static bool s_full = true;
static uint32_t s_key;      /* recompute key of the last full run */
static hy_adherence_t s_ad;
static hy_mark_t s_marks[HY_MAX_MEDS][MAX_DAYS];
static bool s_prog[HY_MAX_MEDS];
static const hy_med_t *s_meds[HY_MAX_MEDS];

/* Monday = 0 ... Sunday = 6 of a YYYYMMDD date */
static int weekday_of(int32_t date)
{
    int y = date / 10000, m = (date / 100) % 100, d = date % 100;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int days = era * 146097 + doe - 719468;   /* days since 1970-01-01, a Thursday */
    return ((days + 3) % 7 + 7) % 7;
}

/* ---- taps ---- */

static void tap_cancel(void)
{
    if (s_tap_timer) {
        lv_timer_delete(s_tap_timer);
        s_tap_timer = NULL;
    }
}

static void tap_fire(lv_timer_t *t)
{
    if (s_pressed) {
        /* A finger is down (a possible second tap or a hold): wait. LVGL has already counted this run
         * against repeat_count and deletes the timer at 0, so give the run back or s_tap_timer dangles. */
        lv_timer_set_repeat_count(t, 1);
        lv_timer_reset(t);
        return;
    }
    s_tap_timer = NULL;      /* the timer deletes itself after this run (repeat count 1) */
    if (ui_current_view() == HY_V_HISTORY) {
        ESP_LOGI(TAG, "row tap: detail of med %u", (unsigned)s_tap_med);
        ui_open_detail(s_tap_med);
    }
}

static void refresh_async(void *arg)
{
    (void)arg;
    history_refresh();
}

void history_invalidate(void)
{
    s_full = true;
}

void history_show(void)
{
    history_refresh_if_changed();
}

void history_hide(void)
{
    tap_cancel();
    s_pressed = false;
}

#if CONFIG_HY_DEBUG_CONSOLE
void history_debug_press(bool down)
{
    s_pressed = down;
}
#endif

void history_toggle_range(void)
{
    s_days = s_days == 7 ? 30 : 7;
    s_force = true;
    s_full = true;
    ESP_LOGI(TAG, "history: %d days", s_days);
    lv_async_call(refresh_async, NULL);
}

/* A TAP stroke ended on a row (med_id) or on the page background (0). Taps that the input layer did not
 * classify as TAP (swipes that ended on the widget) do nothing. */
void history_tap(uint32_t med_id)
{
    if (!ui_input_last_was_tap()) {
        return;
    }
    if (ui_input_last_was_double()) {
        tap_cancel();
        history_toggle_range();
        return;
    }
    tap_cancel();
    if (!med_id) {
        return;
    }
    s_tap_med = med_id;
    s_tap_timer = lv_timer_create(tap_fire, TAP_WAIT_MS, NULL);
    lv_timer_set_repeat_count(s_tap_timer, 1);
}

static void click_cb(lv_event_t *e)
{
    history_tap((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

static void press_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        s_pressed = true;
        if (s_tap_timer) {
            lv_timer_reset(s_tap_timer);   /* the second tap of a double lands within the wait */
        }
    } else {
        s_pressed = false;
    }
}

static void rows_scroll_cb(lv_event_t *e)
{
    (void)e;
    ui_input_consume();   /* a stroke that scrolls the rows must not also page */
    tap_cancel();
}

/* ---- drawing ---- */

static void add_cell(lv_obj_t *row, const geom_t *g, int i, hy_mark_t mk, lv_color_t col, bool dim)
{
    lv_obj_t *c = ui_plain(row);
    lv_obj_set_size(c, g->cell, g->cell);
    lv_obj_set_pos(c, g->x0 + i * g->pitch_x, (g->row_h - g->cell) / 2);
    lv_obj_set_style_radius(c, g->radius, 0);
    switch (mk) {
    case HY_MARK_ON_TIME:
    case HY_MARK_AS_NEEDED_TAKEN:
    case HY_MARK_LATE:
        lv_obj_set_style_bg_color(c, col, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        break;
    case HY_MARK_MISSED:
    case HY_MARK_SKIPPED:
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(c, 2, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(mk == HY_MARK_MISSED ? HY_C_MISS : HY_C_TEXT2), 0);
        break;
    case HY_MARK_UNTRACKED:
        lv_obj_set_style_bg_color(c, lv_color_hex(HY_C_GRAB), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        break;
    case HY_MARK_NONE:
        lv_obj_set_style_bg_color(c, lv_color_hex(HY_C_NA), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        break;
    }
    if (mk == HY_MARK_LATE) {
        lv_obj_t *d = ui_plain(c);
        lv_obj_set_size(d, g->dot, g->dot);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(HY_C_AMBER), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_center(d);
    }
    if (dim) {
        lv_obj_set_style_opa(c, LV_OPA_50, 0);
    }
}

static void add_row(const geom_t *g, int idx, size_t m_idx)
{
    const hy_med_t *m = s_meds[m_idx];
    lv_color_t col = hy_med_color(m->color);
    lv_obj_t *row = lv_obj_create(s_rows);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, HY_SCREEN, g->row_h);
    lv_obj_set_pos(row, 0, idx * g->row_pitch);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, true);
    lv_obj_set_scroll_chain(row, true);
    lv_obj_add_event_cb(row, click_cb, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)m->id);
    lv_obj_add_event_cb(row, press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(row, press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(row, press_cb, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *code = ui_label(row, hy_type.body_bold, 0, m->code);
    lv_obj_set_style_text_color(code, col, 0);
    lv_obj_set_pos(code, CODE_X, (g->row_h - lv_font_get_line_height(hy_type.body_bold)) / 2);

    for (int i = 0; i < s_days; i++) {
        bool dim = i == s_days - 1 && s_prog[m_idx];
        add_cell(row, g, i, s_marks[m_idx][i], col, dim);
    }
}

static void set_headers(const geom_t *g, int32_t first)
{
    for (int i = 0; i < 7; i++) {
        if (s_days == 7) {
            char b[2] = {HY_S_HI_WEEKDAYS[weekday_of(hy_date_add(first, i))], 0};
            lv_label_set_text(s_wd[i], b);
            lv_obj_set_pos(s_wd[i], g->x0 + i * g->pitch_x, INIT_Y - lv_font_get_line_height(hy_type.status) / 2);
            lv_obj_set_hidden(s_wd[i], false);
        } else {
            lv_obj_set_hidden(s_wd[i], true);
        }
    }
    for (int k = 0; k < 5; k++) {
        if (s_days == 30) {
            int32_t d = hy_date_add(first, k * 7);
            int mon = (int)((d / 100) % 100);
            char b[16];
            snprintf(b, sizeof b, HY_S_HI_TICK, (int)(d % 100), mon >= 1 && mon <= 12 ? HY_S_HI_MONTHS + (mon - 1) * 3 : "");
            lv_label_set_text(s_tick[k], b);
            lv_obj_set_pos(s_tick[k], g->x0 + k * 7 * g->pitch_x,
                           INIT_Y - lv_font_get_line_height(hy_type.small) / 2);
            lv_obj_set_hidden(s_tick[k], false);
        } else {
            lv_obj_set_hidden(s_tick[k], true);
        }
    }
}

void history_refresh(void)
{
    if (hy_sched_generation() == 0) {
        return;   /* no evaluation yet */
    }
    int64_t t0 = esp_timer_get_time();
    const hy_snapshot_t *sn = hy_sched_snapshot();
    s_gen_seen = hy_sched_generation();
    const geom_t *g = s_days == 7 ? &G7 : &G30;

    size_t n = 0;
    int32_t today = 0, first = 0;
    hy_adherence_t ad = {0, 0};
    uint32_t sig = 2166136261u;
    if (sn->tracking) {
        today = hy_local_date(sn->in.now, sn->in.tz);
        first = hy_date_add(today, -(s_days - 1));
        for (size_t i = 0; i < sn->in.n_meds && n < HY_MAX_MEDS; i++) {
            if (!sn->in.meds[i].archived) {
                s_meds[n++] = &sn->in.meds[i];
            }
        }
        uint32_t key = ui_fnv(2166136261u, &today, sizeof today);
        key = ui_fnv(key, &s_days, sizeof s_days);
        key = ui_fnv(key, sn->in.tz, strnlen(sn->in.tz, HY_TZ_LEN));
        if (sn->in.last_change) {
            key = ui_fnv(key, &sn->in.last_change->at, sizeof sn->in.last_change->at);
            key = ui_fnv(key, sn->in.last_change->new_tz, strnlen(sn->in.last_change->new_tz, HY_TZ_LEN));
        }
        for (size_t i = 0; i < n; i++) {
            const hy_med_t *m = s_meds[i];
            int64_t since = hy_sched_tracked_since(sn->sched, m->id);
            key = ui_fnv(key, &m->id, sizeof m->id);
            key = ui_fnv(key, &m->type, sizeof m->type);
            key = ui_fnv(key, m->times, sizeof m->times);
            key = ui_fnv(key, &m->n_times, sizeof m->n_times);
            key = ui_fnv(key, &m->weekdays, sizeof m->weekdays);
            key = ui_fnv(key, &m->late_after_min, sizeof m->late_after_min);
            key = ui_fnv(key, &m->missed_after_min, sizeof m->missed_after_min);
            key = ui_fnv(key, &m->early_window_min, sizeof m->early_window_min);
            key = ui_fnv(key, &m->active_from, sizeof m->active_from);
            key = ui_fnv(key, &m->active_until, sizeof m->active_until);
            key = ui_fnv(key, &since, sizeof since);
        }
        key = ui_fnv(key, &sn->in.n_logs, sizeof sn->in.n_logs);
        if (sn->in.n_logs && sn->in.logs) {
            const hy_log_t *last = &sn->in.logs[sn->in.n_logs - 1];
            key = ui_fnv(key, &last->id, sizeof last->id);
            key = ui_fnv(key, &last->ts, sizeof last->ts);
        }
        bool full = s_full || key != s_key;
        /* The snapshot holds the last 8 days of logs. The 30 day range needs every day, so a full recompute
         * queries each medicine's last 32 days (one medicine at a time into one buffer) and sums its adherence. */
        bool deep = full && s_days == 30 && s_hbuf;
        hy_adherence_t deep_ad = {0, 0};
        for (size_t i = 0; i < n; i++) {
            const hy_sched_in_t *in = &sn->in;
            hy_sched_in_t din;
            if (deep) {
                din = sn->in;
                din.logs = s_hbuf;
                din.n_logs = ui_log_window(s_meds[i]->id, sn->in.now - HBUF_WINDOW_S, sn->in.now + 1, s_hbuf, HBUF_CAP);
                in = &din;
            }
            for (int d = full ? 0 : s_days - 1; d < s_days; d++) {
                bool prog = false;
                s_marks[i][d] = hy_day_mark(s_meds[i], hy_date_add(first, d), in, sn->sched,
                                            d == s_days - 1 ? &prog : NULL);
                if (d == s_days - 1) {
                    s_prog[i] = prog;
                }
            }
            if (deep) {
                hy_adherence_t a;
                hy_adherence(s_meds[i]->id, first, s_days, in, sn->sched, &a);
                uint32_t t = (uint32_t)deep_ad.taken + a.taken, r = (uint32_t)deep_ad.resolved + a.resolved;
                deep_ad.taken = (uint16_t)(t > 65535 ? 65535 : t);
                deep_ad.resolved = (uint16_t)(r > 65535 ? 65535 : r);
            }
            sig = ui_fnv(sig, s_marks[i], sizeof s_marks[i][0] * (size_t)s_days);
            sig = ui_fnv(sig, &s_prog[i], sizeof s_prog[i]);
            sig = ui_fnv(sig, &s_meds[i]->id, sizeof s_meds[i]->id);
            sig = ui_fnv(sig, s_meds[i]->code, sizeof s_meds[i]->code);
            sig = ui_fnv(sig, &s_meds[i]->color, sizeof s_meds[i]->color);
        }
        if (full) {
            if (deep) {
                s_ad = deep_ad;
            } else {
                hy_adherence(0, first, s_days, &sn->in, sn->sched, &s_ad);
            }
            s_key = key;
            s_full = false;
        }
        ad = s_ad;
    }
    sig = ui_fnv(sig, &today, sizeof today);
    sig = ui_fnv(sig, &s_days, sizeof s_days);
    sig = ui_fnv(sig, &ad, sizeof ad);
    int64_t t1 = esp_timer_get_time();
    s_view_us = (uint32_t)(t1 - t0);
    if (s_drawn && !s_force && sig == s_sig) {
        return;
    }
    s_sig = sig;
    s_drawn = true;
    s_force = false;

    lv_label_set_text(s_title, s_days == 7 ? HY_S_HI_TITLE_7 : HY_S_HI_TITLE_30);
    set_headers(g, first);
    tap_cancel();
    lv_obj_clean(s_rows);
    for (size_t i = 0; i < n; i++) {
        add_row(g, (int)i, i);
    }
    lv_obj_scroll_to_y(s_rows, 0, LV_ANIM_OFF);

    char b[32];
    if (ad.resolved == 0) {
        snprintf(b, sizeof b, HY_S_ADHERENCE_NONE);
    } else {
        unsigned pct = ((unsigned)ad.taken * 200u + ad.resolved) / (2u * ad.resolved);
        snprintf(b, sizeof b, HY_S_ADHERENCE, pct);
    }
    lv_label_set_text(s_adh, b);

    s_lvgl_us = (uint32_t)(esp_timer_get_time() - t1);
    ESP_LOGI(TAG, "history_refresh: %u meds x %d days: marks %u us, lvgl %u us", (unsigned)n, s_days,
             (unsigned)s_view_us, (unsigned)s_lvgl_us);
}

void history_refresh_if_changed(void)
{
    if (ui_current_view() == HY_V_HISTORY && (hy_sched_generation() != s_gen_seen || s_force || s_full)) {
        history_refresh();
    }
}

void history_timing(uint32_t *view_us, uint32_t *lvgl_us)
{
    *view_us = s_view_us;
    *lvgl_us = s_lvgl_us;
}

/* A legend item: a SQ px square (kind 0 on time, 1 late, 2 missed) and its text, centred on cx */
static void legend_item(lv_obj_t *page, int cx, int kind, uint32_t color, const char *text)
{
    lv_obj_t *item = ui_plain(page);
    lv_obj_set_size(item, LV_SIZE_CONTENT, 22);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *sq = ui_plain(item);
    lv_obj_set_size(sq, SQ, SQ);
    lv_obj_set_style_radius(sq, 2, 0);
    lv_obj_set_style_margin_right(sq, 7, 0);
    if (kind == 2) {
        lv_obj_set_style_bg_opa(sq, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sq, 2, 0);
        lv_obj_set_style_border_color(sq, lv_color_hex(HY_C_MISS), 0);
    } else {
        lv_obj_set_style_bg_color(sq, lv_color_hex(HY_C_TEXT2), 0);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    }
    if (kind == 1) {
        lv_obj_t *d = ui_plain(sq);
        lv_obj_set_size(d, 4, 4);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(HY_C_AMBER), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_center(d);
    }
    ui_label(item, hy_type.status, color, text);
    lv_obj_update_layout(item);
    lv_obj_set_pos(item, cx - lv_obj_get_width(item) / 2, LEGEND_Y - 11);
}

void history_create(lv_obj_t *page)
{
    s_hbuf = heap_caps_malloc(HBUF_CAP * sizeof(hy_log_t), MALLOC_CAP_SPIRAM);
    if (!s_hbuf) {
        ESP_LOGE(TAG, "no PSRAM for the 30 day log window: the 30 day range uses the 8 day snapshot");
    }
    s_page = page;
    s_bg = lv_obj_create(page);   /* the double tap lands anywhere on the page */
    lv_obj_remove_style_all(s_bg);
    lv_obj_set_size(s_bg, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_pos(s_bg, 0, 0);
    lv_obj_set_scrollable(s_bg, false);
    lv_obj_set_clickable(s_bg, true);
    lv_obj_add_event_cb(s_bg, click_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_bg, press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_bg, press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_bg, press_cb, LV_EVENT_PRESS_LOST, NULL);

    s_title = ui_label(page, hy_type.title, HY_C_TEXT, HY_S_HI_TITLE_7);
    lv_obj_set_pos(s_title, 24, 2);

    for (int i = 0; i < 7; i++) {
        s_wd[i] = ui_label(page, hy_type.status, HY_C_TEXT2, "");
        lv_obj_set_width(s_wd[i], G7.cell);
        lv_obj_set_style_text_align(s_wd[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    for (int k = 0; k < 5; k++) {
        s_tick[k] = ui_label(page, hy_type.small, HY_C_TEXT2, "");
        lv_obj_set_hidden(s_tick[k], true);
    }

    s_rows = lv_obj_create(page);
    lv_obj_remove_style_all(s_rows);
    lv_obj_set_size(s_rows, HY_SCREEN, ROWS_H);
    lv_obj_set_pos(s_rows, 0, ROWS_Y);
    lv_obj_set_scroll_dir(s_rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_rows, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_elastic(s_rows, false);
    lv_obj_set_scroll_momentum(s_rows, false);
    lv_obj_set_clickable(s_rows, false);
    lv_obj_add_event_cb(s_rows, rows_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);

    legend_item(page, 104, 0, HY_C_TEXT2, HY_S_HI_ON_TIME);
    legend_item(page, 245, 1, HY_C_TEXT2, HY_S_HI_LATE);
    legend_item(page, 382, 2, HY_C_MISS, HY_S_HI_MISSED);

    s_adh = ui_label(page, hy_type.title, HY_C_TEXT, HY_S_ADHERENCE_NONE);
    lv_obj_set_width(s_adh, HY_SCREEN);
    lv_obj_set_style_text_align(s_adh, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_adh, 0, ADH_Y - lv_font_get_line_height(hy_type.title) / 2);
}
