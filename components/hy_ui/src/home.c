#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hy/actions.h"
#include "hy/clock.h"
#include "hy/occ.h"
#include "hy/store.h"
#include "ui_internal.h"

static const char *TAG = "ui_home";

#define DAY_S 86400
#define LOG_MAX 128
#define OCC_MAX 64
#define RING_MAX 1000
#define RING_ANIM_MS 200
#define TILE_BORDER 3

typedef struct {
    uint32_t med_id;
    char code[3];
    uint8_t color;
    lv_obj_t *tile, *arc, *code_lbl, *status;
    int32_t ring;
} tile_t;

static tile_t s_tiles[HY_MAX_MEDS];
static size_t s_ntiles;
static lv_obj_t *s_grid, *s_time, *s_batt;
static int64_t s_last_minute = -1;
static hy_log_t *s_logs;   /* LOG_MAX entries, PSRAM */

static uint32_t s_pending_id;
static uint32_t s_pending_tick;
static bool s_pending;

/* ---- helpers ---- */

/* Duration as "2 h 14 m"; the spaces inside a number-unit pair are no-break spaces (U+00A0) so a
 * wrapped toast never separates "4" from "h". Under a minute gives "<1 m". */
#define NB " "
static void fmt_dur(int64_t min, char *buf, size_t len)
{
    if (min < 1) {
        snprintf(buf, len, "<1" NB "m");
        return;
    }
    int d = (int)(min / 1440), h = (int)((min % 1440) / 60), m = (int)(min % 60);
    if (d > 0) {
        if (h > 0) {
            snprintf(buf, len, "%d" NB "d %d" NB "h", d, h);
        } else {
            snprintf(buf, len, "%d" NB "d", d);
        }
    } else if (h > 0) {
        if (m > 0) {
            snprintf(buf, len, "%d" NB "h %d" NB "m", h, m);
        } else {
            snprintf(buf, len, "%d" NB "h", h);
        }
    } else {
        snprintf(buf, len, "%d" NB "m", m);
    }
}

/* "HH:MM" of ts in tz: the tail of hy_format_local's "Tue 27 Oct 22:04" */
static void fmt_hhmm(int64_t ts, const char *tz, char *out, size_t len)
{
    char full[32];
    hy_format_local(ts, tz, full, sizeof full);
    size_t n = strlen(full);
    snprintf(out, len, "%s", n >= 5 ? full + n - 5 : full);
}

static const hy_med_t *find_med(uint32_t id)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    for (size_t i = 0; i < n; i++) {
        if (ms[i].id == id) {
            return &ms[i];
        }
    }
    return NULL;
}

bool home_med_id_by_code(const char *code, uint32_t *id)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    for (size_t i = 0; i < n; i++) {
        if (!ms[i].archived && strcmp(ms[i].code, code) == 0) {
            *id = ms[i].id;
            return true;
        }
    }
    return false;
}

/* Logs window by medicine kind. As-needed: the last max(minimum interval, 1 day), short enough that
 * the 128-entry cap (the store keeps the earliest entries) cannot hide the newest dose. Fixed:
 * from the previous slot (up to 1 day + early window back) to now. */
static size_t load_logs(const hy_med_t *m, int64_t now)
{
    int64_t span = DAY_S;
    if (m->type == HY_SCHED_AS_NEEDED) {
        int64_t iv = (int64_t)m->min_interval_min * 60;
        if (iv > span) {
            span = iv;
        }
    } else {
        span = DAY_S + (int64_t)m->early_window_min * 60;
    }
    return hy_store_port()->query_log(NULL, now - span, now + 1, m->id, s_logs, LOG_MAX);
}

/* latest taken dose at or before now; 0 if none */
static int64_t last_taken(size_t n, int64_t now)
{
    int64_t best = 0;
    for (size_t i = 0; i < n; i++) {
        if (s_logs[i].status == HY_TAKEN && s_logs[i].ts <= now && s_logs[i].ts > best) {
            best = s_logs[i].ts;
        }
    }
    return best;
}

typedef struct {
    int32_t ring;      /* 0..RING_MAX */
    bool due;          /* outline */
    int style;         /* 0 secondary text, 1 medicine colour bold, 2 red bold */
    char text[32];
} view_t;

static int32_t frac(int64_t from, int64_t span_s, int64_t now)
{
    if (from <= 0 || span_s <= 0) {
        return 0;
    }
    int64_t v = (now - from) * RING_MAX / span_s;
    return v < 0 ? 0 : v > RING_MAX ? RING_MAX : (int32_t)v;
}

static void compute(const hy_med_t *m, int64_t now, const char *tz, view_t *v)
{
    memset(v, 0, sizeof *v);
    size_t n = load_logs(m, now);
    int64_t taken = last_taken(n, now);

    if (m->type == HY_SCHED_AS_NEEDED) {
        v->ring = m->min_interval_min ? frac(taken, (int64_t)m->min_interval_min * 60, now) : 0;
        snprintf(v->text, sizeof v->text, "as needed");
        return;
    }

    static int64_t prev_occ[OCC_MAX], next_occ[OCC_MAX];
    size_t np = hy_occurrences(m, now - 8 * DAY_S, now + 1, tz, prev_occ, OCC_MAX);
    size_t nn = hy_occurrences(m, now + 1, now + 8 * DAY_S, tz, next_occ, OCC_MAX);
    int64_t prev = np ? prev_occ[np - 1] : 0;
    int64_t next = nn ? next_occ[0] : 0;

    hy_occ_state_t st = HY_OCC_TAKEN;
    if (prev) {
        st = hy_occ_state(m, prev, next, now, tz, s_logs, n, NULL, 0, NULL);
        /* a dose taken early for the upcoming slot settles the display */
        if (next && (st == HY_OCC_DUE || st == HY_OCC_LATE || st == HY_OCC_MISSED)) {
            hy_occ_state_t sn = hy_occ_state(m, next, 0, now, tz, s_logs, n, NULL, 0, NULL);
            if (sn == HY_OCC_TAKEN || sn == HY_OCC_SKIPPED) {
                st = HY_OCC_TAKEN;
            }
        }
    }
    if (st == HY_OCC_DUE || st == HY_OCC_LATE) {
        v->ring = RING_MAX;
        v->due = true;
        v->style = 1;
        snprintf(v->text, sizeof v->text, st == HY_OCC_DUE ? "Due now" : "Late");
        return;
    }
    if (st == HY_OCC_MISSED) {
        v->ring = RING_MAX;
        v->style = 2;
        snprintf(v->text, sizeof v->text, "Missed");
        return;
    }
    int64_t from = taken ? taken : prev;
    v->ring = (from && next && next > from) ? frac(from, next - from, now) : 0;
    if (taken) {
        int64_t min = (now - taken) / 60;
        if (min < 1) {
            snprintf(v->text, sizeof v->text, "just now");
        } else {
            char d[24];
            fmt_dur(min, d, sizeof d);
            snprintf(v->text, sizeof v->text, "%s ago", d);
        }
    } else {
        snprintf(v->text, sizeof v->text, "no dose yet");
    }
}

/* ---- ring animation ---- */

static void ring_exec(void *obj, int32_t val)
{
    lv_arc_set_value((lv_obj_t *)obj, val);
}

static void set_ring(tile_t *t, int32_t target, bool animate)
{
    if (t->ring == target) {
        return;
    }
    lv_anim_delete(t->arc, ring_exec);
    if (animate) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, t->arc);
        lv_anim_set_exec_cb(&a, ring_exec);
        lv_anim_set_values(&a, t->ring, target);
        lv_anim_set_duration(&a, RING_ANIM_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    } else {
        lv_arc_set_value(t->arc, target);
    }
    t->ring = target;
}

/* ---- tiles ---- */

static void tile_click_cb(lv_event_t *e)
{
    uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "tile tap: med %u", (unsigned)id);
    home_tap_med(id);
}

static void make_tile(const hy_med_t *m, tile_t *t)
{
    memset(t, 0, sizeof *t);
    t->med_id = m->id;
    snprintf(t->code, sizeof t->code, "%s", m->code);
    t->color = m->color;
    lv_color_t col = hy_med_color(m->color);

    lv_obj_t *tile = lv_obj_create(s_grid);
    t->tile = tile;
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, HY_TILE_W, HY_TILE_H);
    lv_obj_set_style_radius(tile, 24, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(HY_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, TILE_BORDER, 0);
    lv_obj_set_style_border_color(tile, col, 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_clickable(tile, true);
    lv_obj_set_scroll_chain(tile, true);
    lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)m->id);

    /* children are positioned inside the tile's border box, so subtract the border */
    const int ring_x = (HY_TILE_W - HY_RING) / 2 - TILE_BORDER, ring_y = 16 - TILE_BORDER;

    lv_obj_t *disc = lv_obj_create(tile);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, HY_RING - 2 * HY_RING_W, HY_RING - 2 * HY_RING_W);
    lv_obj_set_pos(disc, ring_x + HY_RING_W, ring_y + HY_RING_W);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(HY_C_INNER), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_clickable(disc, false);
    lv_obj_set_scrollable(disc, false);

    lv_obj_t *arc = lv_arc_create(tile);
    t->arc = arc;
    lv_obj_set_size(arc, HY_RING, HY_RING);
    lv_obj_set_pos(arc, ring_x, ring_y);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, RING_MAX);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_style_pad_all(arc, 0, 0);
    lv_obj_set_style_arc_width(arc, HY_RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(HY_C_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, HY_RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, col, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);

    lv_obj_t *code = lv_label_create(tile);
    t->code_lbl = code;
    lv_obj_set_style_text_font(code, hy_type.code, 0);
    lv_obj_set_style_text_color(code, col, 0);
    lv_label_set_text(code, m->code);
    lv_obj_align(code, LV_ALIGN_TOP_MID, 0, ring_y + HY_RING / 2 - lv_font_get_line_height(hy_type.code) / 2 + 3); /* +3: matches the mockup glyph centre */

    lv_obj_t *st = lv_label_create(tile);
    t->status = st;
    lv_obj_set_style_text_font(st, hy_type.status, 0);
    lv_obj_set_style_text_color(st, lv_color_hex(HY_C_TEXT2), 0);
    lv_label_set_text(st, "");
}

static void update_time(int64_t now, bool valid, const char *tz)
{
    char b[8];
    if (valid) {
        fmt_hhmm(now, tz, b, sizeof b);
    } else {
        snprintf(b, sizeof b, "--:--");
    }
    lv_label_set_text(s_time, b);
}

static void set_status(tile_t *t, const char *text, int style)
{
    const lv_font_t *f = style ? hy_type.status_bold : hy_type.status;
    lv_label_set_text(t->status, text);
    lv_obj_set_style_text_font(t->status, f, 0);
    lv_obj_set_style_text_color(t->status,
                                style == 1 ? hy_med_color(t->color)
                                : style == 2 ? lv_color_hex(HY_C_RED)
                                             : lv_color_hex(HY_C_TEXT2), 0);
    /* status line centre about y 153 in the tile, border-box coordinates */
    lv_obj_align(t->status, LV_ALIGN_TOP_MID, 0, 153 - TILE_BORDER - lv_font_get_line_height(f) / 2);
}

void home_refresh(bool animate)
{
    int64_t t0 = esp_timer_get_time();
    const hy_clock_port_t *ck = hy_clock_port();
    bool valid = ck->valid(NULL);
    int64_t now = valid ? ck->now(NULL) : 0;
    const char *tz = ck->tz(NULL);
    update_time(now, valid, tz);
    s_last_minute = valid ? now / 60 : -1;

    for (size_t i = 0; i < s_ntiles; i++) {
        tile_t *t = &s_tiles[i];
        const hy_med_t *m = find_med(t->med_id);
        if (!m) {
            continue;
        }
        if (!valid) {
            set_ring(t, 0, animate);
            set_status(t, "clock not set", 0);
            lv_obj_set_style_border_opa(t->tile, LV_OPA_TRANSP, 0);
            continue;
        }
        view_t v;
        compute(m, now, tz, &v);
        set_ring(t, v.ring, animate);
        set_status(t, v.text, v.style);
        lv_obj_set_style_border_opa(t->tile, v.due ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    int64_t us = esp_timer_get_time() - t0;
    static bool first = true;
    ESP_LOG_LEVEL(first ? ESP_LOG_INFO : ESP_LOG_DEBUG, TAG, "home_refresh: %u tiles in %u ms",
                  (unsigned)s_ntiles, (unsigned)((us + 500) / 1000));
    first = false;
}

void home_rebuild(void)
{
    lv_obj_clean(s_grid);
    s_ntiles = 0;
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    for (size_t i = 0; i < n && s_ntiles < HY_MAX_MEDS; i++) {
        if (ms[i].archived) {
            continue;
        }
        make_tile(&ms[i], &s_tiles[s_ntiles++]);
    }
    lv_obj_update_layout(s_grid);
    ESP_LOGI(TAG, "grid rebuilt: %u tiles, viewport %d px, scrollable %d px",
             (unsigned)s_ntiles, (int)lv_obj_get_height(s_grid), (int)lv_obj_get_scroll_bottom(s_grid));
    home_refresh(false);
}

void home_create(lv_obj_t *parent)
{
    if (!s_logs) {
        s_logs = heap_caps_malloc(LOG_MAX * sizeof(hy_log_t), MALLOC_CAP_SPIRAM);
        if (!s_logs) {
            s_logs = malloc(LOG_MAX * sizeof(hy_log_t));
        }
    }

    s_time = lv_label_create(parent);
    lv_obj_set_style_text_font(s_time, hy_type.time, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(HY_C_TEXT), 0);
    lv_label_set_text(s_time, "--:--");
    lv_obj_align(s_time, LV_ALIGN_TOP_LEFT, HY_MARGIN + 2, (HY_BAR_H - lv_font_get_line_height(hy_type.time)) / 2);

    s_batt = lv_label_create(parent);
    lv_obj_set_style_text_font(s_batt, hy_type.body, 0);
    lv_obj_set_style_text_color(s_batt, lv_color_hex(HY_C_TEXT2), 0);
    lv_label_set_text(s_batt, "\xE2\x80\x94%"); /* battery placeholder until plan 3 */
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -HY_MARGIN, (HY_BAR_H - lv_font_get_line_height(hy_type.body)) / 2);

    s_grid = lv_obj_create(parent);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, HY_SCREEN, HY_SCREEN - HY_BAR_H);
    lv_obj_set_pos(s_grid, 0, HY_BAR_H);
    lv_obj_set_style_pad_left(s_grid, HY_MARGIN, 0);
    lv_obj_set_style_pad_right(s_grid, HY_MARGIN, 0);
    lv_obj_set_style_pad_top(s_grid, 8, 0);
    lv_obj_set_style_pad_bottom(s_grid, HY_MARGIN, 0);
    lv_obj_set_style_pad_column(s_grid, HY_GAP, 0);
    lv_obj_set_style_pad_row(s_grid, HY_GAP, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_elastic(s_grid, false);
    lv_obj_set_scroll_momentum(s_grid, false);
    home_rebuild();
}

/* ---- tapping ---- */

void home_tap_med(uint32_t med_id)
{
    if (toast_logged_visible()) {
        /* A finger lands on the scrim and does nothing; the serial command behaves the same. */
        ESP_LOGI(TAG, "tap ignored: logged toast is showing");
        return;
    }
    const hy_med_t *m = find_med(med_id);
    if (!m || m->archived) {
        ESP_LOGW(TAG, "tap: medicine %u not found", (unsigned)med_id);
        return;
    }
    const hy_clock_port_t *ck = hy_clock_port();
    bool valid = ck->valid(NULL);
    int64_t now = valid ? ck->now(NULL) : 0;
    const char *tz = ck->tz(NULL);
    bool second = s_pending && s_pending_id == med_id && lv_tick_elaps(s_pending_tick) <= HY_TOO_SOON_WINDOW_MS;

    if (valid && m->type == HY_SCHED_AS_NEEDED && !second) {
        size_t n = load_logs(m, now);
        int32_t since = -1;
        if (hy_min_interval_check(m, now, s_logs, n, &since) == HY_W_TOO_SOON) {
            char a[24], b[24], msg[96];
            fmt_dur(since, a, sizeof a);
            fmt_dur(m->min_interval_min, b, sizeof b);
            snprintf(msg, sizeof msg, "Last dose %s ago, minimum %s. Tap again to log", a, b);
            s_pending = true;
            s_pending_id = med_id;
            s_pending_tick = lv_tick_get();
            ESP_LOGI(TAG, "too soon: %s", msg);
            toast_show_info(msg);
            return;
        }
    }
    s_pending = false;

    char code[3];
    snprintf(code, sizeof code, "%s", m->code);   /* m is invalid after an action */
    uint32_t log_id = 0;
    hy_err_t err = hy_act_log_taken(med_id, 0, NULL, HY_SRC_SCREEN, &log_id);
    ESP_LOGI(TAG, "log_taken med %s: err %d, log id %u", code, (int)err, (unsigned)log_id);
    if (err == HY_OK) {
        char hhmm[8];
        fmt_hhmm(now, tz, hhmm, sizeof hhmm);
        toast_show_logged(code, hhmm, log_id);
        ui_log_hwm("log");
    } else if (err == HY_E_CLOCK) {
        toast_show_info("Set the time first");
    } else {
        char msg[48];
        snprintf(msg, sizeof msg, "Could not log (error %d)", (int)err);
        toast_show_info(msg);
    }
}

/* ---- minute refresh ---- */

void home_minute_tick(void)
{
    const hy_clock_port_t *ck = hy_clock_port();
    int64_t minute = ck->valid(NULL) ? ck->now(NULL) / 60 : -1;
    if (minute != s_last_minute) {
        home_refresh(true);
    }
}
