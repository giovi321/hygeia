#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hy/actions.h"
#include "hy/clock.h"
#include "hy/occ.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "hy/summary.h"
#include "ui_internal.h"

static const char *TAG = "ui_home";

#define DAY_S 86400
#define LOG_MAX 128
#define RING_MAX HY_RING_MAX
#define RING_ANIM_MS 200
#define TILE_BORDER 3

typedef struct {
    uint32_t med_id;
    char code[3];
    uint8_t color;
    lv_obj_t *tile, *arc, *code_lbl, *status;
    int32_t ring;
    /* what the status line and outline show now, so a refresh touches only tiles that changed */
    bool drawn, due;
    int style;
    char text[32];
} tile_t;

static tile_t s_tiles[HY_MAX_MEDS];
static size_t s_ntiles;
static lv_obj_t *s_grid;
static uint32_t s_gen_seen;
static uint32_t s_view_us, s_lvgl_us;   /* last home_refresh: tile view computation, LVGL updates */
static hy_log_t *s_logs;   /* LOG_MAX entries, PSRAM: the as-needed too-soon check only */

static uint32_t s_pending_id;
static uint32_t s_pending_tick;
static bool s_pending;

/* ---- helpers ---- */

/* Duration as "2 h 14 m"; strings.h joins each number and unit with a no-break space so a wrapped toast never
 * separates "4" from "h". Under a minute gives "<1 m". */
static void fmt_dur(int64_t min, char *buf, size_t len)
{
    if (min < 1) {
        snprintf(buf, len, HY_S_DUR_LT1M);
        return;
    }
    int d = (int)(min / 1440), h = (int)((min % 1440) / 60), m = (int)(min % 60);
    if (d > 0) {
        if (h > 0) {
            snprintf(buf, len, HY_S_DUR_DH, d, h);
        } else {
            snprintf(buf, len, HY_S_DUR_D, d);
        }
    } else if (h > 0) {
        if (m > 0) {
            snprintf(buf, len, HY_S_DUR_HM, h, m);
        } else {
            snprintf(buf, len, HY_S_DUR_H, h);
        }
    } else {
        snprintf(buf, len, HY_S_DUR_M, m);
    }
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

/* Logs from `from` to now. The caller picks the window so the 128-entry cap (the store keeps the
 * earliest entries) cannot hide the newest dose. */
static size_t load_logs(const hy_med_t *m, int64_t from, int64_t now)
{
    return hy_store_port()->query_log(NULL, from, now + 1, m->id, s_logs, LOG_MAX);
}

/* As-needed window: the last max(minimum interval, 1 day). */
static int64_t as_needed_from(const hy_med_t *m, int64_t now)
{
    int64_t span = DAY_S;
    int64_t iv = (int64_t)m->min_interval_min * 60;
    if (iv > span) {
        span = iv;
    }
    return now - span;
}

typedef struct {
    int32_t ring;      /* 0..RING_MAX */
    bool due;          /* outline */
    int style;         /* 0 secondary text, 1 medicine colour bold, 2 red bold */
    char text[32];
} view_t;

static void view_of(const hy_med_t *m, const hy_snapshot_t *sn, view_t *v)
{
    memset(v, 0, sizeof *v);
    hy_tile_view_t tv;
    hy_tile_view(m, &sn->in, sn->sched, &tv);
    v->ring = tv.ring;
    switch (tv.kind) {
    case HY_TILE_AS_NEEDED:
        snprintf(v->text, sizeof v->text, HY_S_TILE_AS_NEEDED);
        break;
    case HY_TILE_NO_DOSE:
        snprintf(v->text, sizeof v->text, HY_S_TILE_NO_DOSE);
        break;
    case HY_TILE_SINCE: {
        int64_t min = (sn->in.now - tv.last_taken) / 60;
        if (min < 1) {
            snprintf(v->text, sizeof v->text, HY_S_TILE_JUST_NOW);
        } else {
            char d[24];
            fmt_dur(min, d, sizeof d);
            snprintf(v->text, sizeof v->text, HY_S_TILE_AGO, d);
        }
        break;
    }
    case HY_TILE_DUE:
    case HY_TILE_LATE:
        v->due = true;
        v->style = 1;
        snprintf(v->text, sizeof v->text, "%s", tv.kind == HY_TILE_DUE ? HY_S_TILE_DUE : HY_S_TILE_LATE);
        break;
    case HY_TILE_SNOOZED: {
        char hhmm[8];
        ui_fmt_hhmm(tv.snoozed_until, sn->in.tz, hhmm, sizeof hhmm);
        v->style = 1;
        snprintf(v->text, sizeof v->text, HY_S_TILE_SNOOZED, hhmm);
        break;
    }
    case HY_TILE_MISSED:
        v->style = 2;
        snprintf(v->text, sizeof v->text, HY_S_TILE_MISSED);
        break;
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

/* SHORT_CLICKED also follows a swipe that started on the tile (the pager does not scroll): only a stroke the
 * input layer classified as TAP logs. */
void home_tile_click(uint32_t id)
{
    if (!ui_input_last_was_tap()) {
        ESP_LOGI(TAG, "tile click ignored: med %u, stroke was not a tap", (unsigned)id);
        return;
    }
    ESP_LOGI(TAG, "tile tap: med %u", (unsigned)id);
    home_tap_med(id);
}

static void tile_click_cb(lv_event_t *e)
{
    home_tile_click((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

/* A stroke that scrolls the grid is the grid's: it must not also page the pager */
static void grid_scroll_cb(lv_event_t *e)
{
    (void)e;
    ui_input_consume();
}

static void tile_long_cb(lv_event_t *e)
{
    uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (!ui_input_still()) {
        return;   /* a slow drag is not a long press */
    }
    ESP_LOGI(TAG, "tile long press: med %u", (unsigned)id);
    ui_open_detail(id);
}

static void plus_click_cb(lv_event_t *e)
{
    (void)e;
    if (!ui_input_last_was_tap()) {
        return;
    }
    ESP_LOGI(TAG, "plus tile tap");
    toast_show_info(HY_S_ADD_IN_WEB);
}

/* The "+" tile after the medicine tiles; the QR to the web UI comes with plan 4 */
static void make_plus(void)
{
    lv_obj_t *tile = lv_obj_create(s_grid);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, HY_TILE_W, HY_TILE_H);
    lv_obj_set_style_radius(tile, 24, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(HY_C_PLUS_BG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_clickable(tile, true);
    lv_obj_set_scroll_chain(tile, true);
    lv_obj_add_event_cb(tile, plus_click_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(tile);
    lv_obj_set_style_text_font(l, hy_type.code_m, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(HY_C_TEXT2), 0);
    lv_label_set_text(l, HY_S_PLUS);
    lv_obj_center(l);
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
    lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)m->id);
    lv_obj_add_event_cb(tile, tile_long_cb, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)m->id);

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

/* The medicine as the snapshot saw it; the live list for a tile the snapshot does not know yet */
static const hy_med_t *snap_med(const hy_snapshot_t *sn, uint32_t id)
{
    for (size_t i = 0; i < sn->in.n_meds; i++) {
        if (sn->in.meds[i].id == id) {
            return &sn->in.meds[i];
        }
    }
    return find_med(id);
}

/* Applies v to the tile, touching LVGL only for what changed since the last refresh */
static void apply_view(tile_t *t, const view_t *v, bool animate)
{
    set_ring(t, v->ring, animate);   /* no-op for an unchanged ring */
    if (!t->drawn || t->style != v->style || strcmp(t->text, v->text) != 0) {
        set_status(t, v->text, v->style);
        t->style = v->style;
        snprintf(t->text, sizeof t->text, "%s", v->text);
    }
    if (!t->drawn || t->due != v->due) {
        lv_obj_set_style_border_opa(t->tile, v->due ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        t->due = v->due;
    }
    t->drawn = true;
}

void home_refresh(bool animate)
{
    if (hy_sched_generation() == 0) {
        return;   /* no evaluation yet: the tiles stay blank until the first snapshot */
    }
    int64_t t0 = esp_timer_get_time();
    const hy_snapshot_t *sn = hy_sched_snapshot();
    s_gen_seen = hy_sched_generation();
    bool valid = sn->tracking;
    int64_t view_us = 0;

    for (size_t i = 0; i < s_ntiles; i++) {
        tile_t *t = &s_tiles[i];
        const hy_med_t *m = snap_med(sn, t->med_id);
        if (!m) {
            continue;
        }
        view_t v;
        if (!valid) {
            memset(&v, 0, sizeof v);
            snprintf(v.text, sizeof v.text, "%s", HY_S_TILE_CLOCK_UNSET);
        } else {
            int64_t tv0 = esp_timer_get_time();
            view_of(m, sn, &v);
            view_us += esp_timer_get_time() - tv0;
        }
        apply_view(t, &v, animate);
    }
    int64_t us = esp_timer_get_time() - t0;
    s_view_us = (uint32_t)view_us;
    s_lvgl_us = (uint32_t)(us - view_us);
    static bool first = true;
    ESP_LOG_LEVEL(first ? ESP_LOG_INFO : ESP_LOG_DEBUG, TAG, "home_refresh: %u tiles in %u ms",
                  (unsigned)s_ntiles, (unsigned)((us + 500) / 1000));
    first = false;
}

void home_refresh_timing(uint32_t *view_us, uint32_t *lvgl_us)
{
    *view_us = s_view_us;
    *lvgl_us = s_lvgl_us;
}

void home_refresh_if_changed(void)
{
    if (hy_sched_generation() != s_gen_seen) {
        home_refresh(true);
    }
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
    make_plus();
    lv_obj_update_layout(s_grid);
    ESP_LOGI(TAG, "grid rebuilt: %u tiles, viewport %d px, scrollable %d px",
             (unsigned)s_ntiles, (int)lv_obj_get_height(s_grid), (int)lv_obj_get_scroll_bottom(s_grid));
    home_refresh(false);
}

void home_create(lv_obj_t *page)
{
    if (!s_logs) {
        s_logs = heap_caps_malloc(LOG_MAX * sizeof(hy_log_t), MALLOC_CAP_SPIRAM);
        if (!s_logs) {
            s_logs = malloc(LOG_MAX * sizeof(hy_log_t));
        }
    }

    s_grid = lv_obj_create(page);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_pos(s_grid, 0, 0);
    lv_obj_set_style_pad_left(s_grid, HY_MARGIN, 0);
    lv_obj_set_style_pad_right(s_grid, HY_MARGIN, 0);
    lv_obj_set_style_pad_top(s_grid, 8, 0);
    lv_obj_set_style_pad_bottom(s_grid, HY_MARGIN, 0);
    lv_obj_set_style_pad_column(s_grid, HY_GAP, 0);
    lv_obj_set_style_pad_row(s_grid, HY_GAP, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_grid, grid_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_set_scroll_elastic(s_grid, false);
    lv_obj_set_scroll_momentum(s_grid, false);
    home_rebuild();
}

/* ---- tapping ---- */

const char *ui_err_text(hy_err_t err)
{
    switch (err) {
    case HY_E_CLOCK:     return HY_S_ERR_CLOCK;
    case HY_E_NOT_FOUND: return HY_S_ERR_NOT_FOUND;
    case HY_E_IO:        return HY_S_ERR_IO;
    case HY_E_FULL:      return HY_S_ERR_FULL;
    default:             return HY_S_ERR_LOG;
    }
}

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
    char tz[HY_TZ_LEN];
    hy_clock_tz_copy(tz, sizeof tz); /* the port pointer may be replaced by a zone change */
    bool second = s_pending && s_pending_id == med_id && lv_tick_elaps(s_pending_tick) <= HY_TOO_SOON_WINDOW_MS;

    if (valid && m->type == HY_SCHED_AS_NEEDED && !second) {
        size_t n = load_logs(m, as_needed_from(m, now), now);
        int32_t since = -1;
        if (hy_min_interval_check(m, now, s_logs, n, &since) == HY_W_TOO_SOON) {
            char a[24], b[24], msg[96];
            fmt_dur(since, a, sizeof a);
            fmt_dur(m->min_interval_min, b, sizeof b);
            snprintf(msg, sizeof msg, HY_S_TOO_SOON, a, b);
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
    ESP_LOGI(TAG, "log_taken med %s: %s (%d), log id %u", code, hy_err_str(err), (int)err, (unsigned)log_id);
    if (err == HY_OK) {
        char hhmm[8];
        ui_fmt_hhmm(now, tz, hhmm, sizeof hhmm);
        toast_show_logged(code, hhmm, log_id);
        ui_log_hwm("log");
    } else {
        toast_show_info(ui_err_text(err));
    }
}
