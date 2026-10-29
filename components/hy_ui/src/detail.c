/* Medicine detail (opened by a tile long press and a History row tap): ring, name, schedule line, pills left,
 * the three newest log entries, the two log buttons, the 30-day grid and the adherence. One vertical scroll.
 *
 * Everything here runs in the LVGL task. The scheduler snapshot supplies the medicine, the ring and the
 * tracking start. The snapshot's log window is 8 days, so this view queries the store for this medicine's
 * last 32 days (marks, adherence, newest entries) and runs the summaries on a copy of the snapshot input with
 * those logs. That query and the 30 day marks (about 4 ms a day) are cached under a key (today, zone, last zone
 * change, tracked-since, the medicine, its log count and newest id) and run again only when the key changes or an
 * event invalidated them; a minute tick recomputes today's cell only. */

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hy/occ.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "hy/summary.h"
#include "ui_internal.h"

static const char *TAG = "ui_detail";

#define DAYS        30
#define COLS        15
#define CELL        22
#define CELL_GAP    6
#define RING_D      96
#define RING_THICK  8
#define HEAD_H      97
#define TEXT_X      138
#define CARD_M      18
#define ROW_H       38
#define BTN_H       66
#define BTN_W       216
#define BUF_CAP     512             /* logs of one medicine over the 32 day window */
#define WINDOW_S    (32 * 86400)
#define OLD_S       (366 * 86400)   /* the look back for the newest entries of a quiet medicine */
#define OLD_CAP     64
#define N_RECENT    3

static lv_obj_t *s_root, *s_scroll, *s_arc, *s_code, *s_name, *s_sched, *s_pills, *s_adh, *s_grid;
static lv_obj_t *s_rowbox[N_RECENT], *s_when[N_RECENT], *s_what[N_RECENT];
static lv_obj_t *s_cell[DAYS], *s_dot[DAYS];

static uint32_t s_med_id;
static bool s_open;                  /* the detail group is on screen: a return from note keeps the scroll */
static bool s_inval = true;
static uint32_t s_gen_seen;
static uint32_t s_static_sig, s_rows_sig, s_cells_sig, s_key;
static int32_t s_ring = -1;
static char s_when_txt[N_RECENT][40], s_what_txt[N_RECENT][24];
static char s_adh_txt[24];

static hy_log_t *s_buf;              /* BUF_CAP entries, PSRAM, ascending ts; the tail is scratch for the old look back */
static size_t s_nbuf;
static hy_log_t s_recent[N_RECENT];  /* newest first */
static size_t s_nrecent;
static hy_mark_t s_marks[DAYS];
static bool s_prog;
static hy_adherence_t s_ad;

/* ---- data ---- */

/* The medicine's last 32 days, and its newest entries even when they are older than that */
static void load_logs(const hy_med_t *m, int64_t now)
{
    s_nbuf = s_buf ? hy_store_port()->query_log(NULL, now - WINDOW_S, now + 1, m->id, s_buf, BUF_CAP) : 0;
    if (s_nbuf >= BUF_CAP) {
        ESP_LOGW(TAG, "log window of med %u filled the %u entry buffer", (unsigned)m->id, BUF_CAP);
    }
    s_nrecent = 0;
    size_t got = s_nbuf < N_RECENT ? s_nbuf : N_RECENT;
    for (size_t i = 0; i < got; i++) {
        s_recent[s_nrecent++] = s_buf[s_nbuf - 1 - i];
    }
    if (got < N_RECENT && s_buf) {
        size_t room = BUF_CAP - s_nbuf < OLD_CAP ? BUF_CAP - s_nbuf : OLD_CAP;
        size_t n = room ? hy_store_port()->query_log(NULL, now - OLD_S, now - WINDOW_S, m->id, s_buf + s_nbuf, room) : 0;
        for (size_t i = 0; i < n && s_nrecent < N_RECENT; i++) {
            s_recent[s_nrecent++] = s_buf[s_nbuf + n - 1 - i];
        }
    }
}

/* ---- drawing ---- */

static void set_text(lv_obj_t *l, char *cache, size_t cap, const char *text)
{
    if (strncmp(cache, text, cap) != 0) {
        lv_label_set_text(l, text);
        snprintf(cache, cap, "%s", text);
    }
}

static void style_cell(int i, hy_mark_t mk, lv_color_t col, bool dim)
{
    lv_obj_t *c = s_cell[i];
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_opa(c, dim ? LV_OPA_50 : LV_OPA_COVER, 0);
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
    lv_obj_set_hidden(s_dot[i], mk != HY_MARK_LATE);
}

/* "06:00, 14:00, 22:00", with the dose in front when there is one */
static void schedule_text(const hy_med_t *m, char *out, size_t len)
{
    char body[96];
    if (m->type == HY_SCHED_AS_NEEDED) {
        if (m->min_interval_min) {
            char d[24];
            ui_fmt_dur(m->min_interval_min, d, sizeof d);
            snprintf(body, sizeof body, HY_S_DET_AS_NEEDED, d);
        } else {
            snprintf(body, sizeof body, HY_S_TILE_AS_NEEDED);
        }
    } else {
        size_t o = 0;
        body[0] = 0;
        for (uint8_t i = 0; i < m->n_times && i < HY_MAX_TIMES && o + 8 < sizeof body; i++) {
            o += (size_t)snprintf(body + o, sizeof body - o, "%s%02u:%02u", i ? HY_S_DET_SEP : "",
                                  (unsigned)(m->times[i] / 60), (unsigned)(m->times[i] % 60));
        }
    }
    if (m->dose[0]) {
        snprintf(out, len, "%s" HY_S_DOT "%s", m->dose, body);
    } else {
        snprintf(out, len, "%s", body);
    }
}

static void draw_static(const hy_med_t *m)
{
    uint32_t sig = 2166136261u;
    sig = ui_fnv(sig, &m->id, sizeof m->id);
    sig = ui_fnv(sig, m->code, sizeof m->code);
    sig = ui_fnv(sig, m->name, sizeof m->name);
    sig = ui_fnv(sig, m->dose, sizeof m->dose);
    sig = ui_fnv(sig, &m->color, sizeof m->color);
    sig = ui_fnv(sig, &m->type, sizeof m->type);
    sig = ui_fnv(sig, m->times, sizeof m->times);
    sig = ui_fnv(sig, &m->n_times, sizeof m->n_times);
    sig = ui_fnv(sig, &m->min_interval_min, sizeof m->min_interval_min);
    sig = ui_fnv(sig, &m->pills_enabled, sizeof m->pills_enabled);
    sig = ui_fnv(sig, &m->pills_left, sizeof m->pills_left);
    sig = ui_fnv(sig, &m->pills_alert, sizeof m->pills_alert);
    if (sig == s_static_sig) {
        return;
    }
    s_static_sig = sig;
    lv_color_t col = hy_med_color(m->color);
    lv_obj_set_style_arc_color(s_arc, col, LV_PART_INDICATOR);
    lv_label_set_text(s_code, m->code);
    lv_obj_set_style_text_color(s_code, col, 0);
    lv_obj_center(s_code);
    lv_label_set_text(s_name, m->name);
    char t[128];
    schedule_text(m, t, sizeof t);
    lv_label_set_text(s_sched, t);
    if (m->pills_enabled) {
        if (m->pills_left == 1) {
            snprintf(t, sizeof t, HY_S_DET_PILL_1);
        } else {
            snprintf(t, sizeof t, HY_S_DET_PILLS, (unsigned)(m->pills_left < 0 ? 0 : m->pills_left));
        }
        lv_label_set_text(s_pills, t);
        lv_obj_set_style_text_color(s_pills, lv_color_hex(m->pills_left <= m->pills_alert ? HY_C_AMBER : HY_C_TEXT2), 0);
        lv_obj_set_hidden(s_pills, false);
    } else {
        lv_obj_set_hidden(s_pills, true);
    }
}

/* "Today 06:28", "Yesterday 22:04", "Mon 26 Oct 14:41", all in the entry's own zone */
static void when_text(const hy_log_t *e, int64_t now, char *out, size_t len)
{
    int32_t today = hy_local_date(now, e->tz), d = hy_local_date(e->ts, e->tz);
    char hhmm[8];
    ui_fmt_hhmm(e->ts, e->tz, hhmm, sizeof hhmm);
    if (d == today) {
        snprintf(out, len, HY_S_TODAY_AT, hhmm);
    } else if (d == hy_date_add(today, -1)) {
        snprintf(out, len, HY_S_YESTERDAY_AT, hhmm);
    } else {
        hy_format_local(e->ts, e->tz, out, len);
    }
}

static void what_text(const hy_log_t *e, char *out, size_t len)
{
    out[0] = 0;
    if (e->status == HY_SKIPPED) {
        snprintf(out, len, HY_S_DET_SKIPPED);
    } else if (e->status == HY_MISSED) {
        snprintf(out, len, HY_S_DET_MISSED);
    } else if (e->slot) {
        hy_delta_text(e->delta_min, out, len);
    }
}

static void draw_rows(int64_t now, int32_t today)
{
    uint32_t sig = 2166136261u;
    sig = ui_fnv(sig, &today, sizeof today);
    sig = ui_fnv(sig, &s_nrecent, sizeof s_nrecent);
    for (size_t i = 0; i < s_nrecent; i++) {
        sig = ui_fnv(sig, &s_recent[i].id, sizeof s_recent[i].id);
        sig = ui_fnv(sig, &s_recent[i].ts, sizeof s_recent[i].ts);
        sig = ui_fnv(sig, &s_recent[i].status, sizeof s_recent[i].status);
        sig = ui_fnv(sig, &s_recent[i].slot, sizeof s_recent[i].slot);
        sig = ui_fnv(sig, &s_recent[i].delta_min, sizeof s_recent[i].delta_min);
        sig = ui_fnv(sig, s_recent[i].tz, strnlen(s_recent[i].tz, HY_TZ_LEN));
    }
    if (sig == s_rows_sig) {
        return;
    }
    s_rows_sig = sig;
    for (size_t i = 0; i < N_RECENT; i++) {
        char w[40] = "", x[24] = "";
        bool show = i < s_nrecent || (i == 0 && s_nrecent == 0);
        if (i < s_nrecent) {
            when_text(&s_recent[i], now, w, sizeof w);
            what_text(&s_recent[i], x, sizeof x);
            lv_obj_set_style_text_color(s_when[i], lv_color_hex(HY_C_TEXT), 0);
        } else if (show) {
            snprintf(w, sizeof w, HY_S_DET_NO_LOGS);
            lv_obj_set_style_text_color(s_when[i], lv_color_hex(HY_C_TEXT2), 0);
        }
        set_text(s_when[i], s_when_txt[i], sizeof s_when_txt[i], w);
        set_text(s_what[i], s_what_txt[i], sizeof s_what_txt[i], x);
        lv_obj_set_hidden(s_rowbox[i], !show);
    }
}

static void draw_cells(const hy_med_t *m, bool tracking)
{
    uint32_t sig = 2166136261u;
    sig = ui_fnv(sig, s_marks, sizeof s_marks);
    sig = ui_fnv(sig, &s_prog, sizeof s_prog);
    sig = ui_fnv(sig, &m->color, sizeof m->color);
    sig = ui_fnv(sig, &s_ad, sizeof s_ad);
    sig = ui_fnv(sig, &tracking, sizeof tracking);
    if (sig == s_cells_sig) {
        return;
    }
    s_cells_sig = sig;
    lv_color_t col = hy_med_color(m->color);
    for (int i = 0; i < DAYS; i++) {
        style_cell(i, s_marks[i], col, i == DAYS - 1 && s_prog);
    }
    char b[24];
    if (!tracking || s_ad.resolved == 0) {
        snprintf(b, sizeof b, HY_S_ADHERENCE_NONE);
    } else {
        snprintf(b, sizeof b, HY_S_ADHERENCE, ((unsigned)s_ad.taken * 200u + s_ad.resolved) / (2u * s_ad.resolved));
    }
    set_text(s_adh, s_adh_txt, sizeof s_adh_txt, b);
}

static void render(void)
{
    if (hy_sched_generation() == 0) {
        return;
    }
    const hy_snapshot_t *sn = hy_sched_snapshot();
    s_gen_seen = hy_sched_generation();
    const hy_med_t *m = NULL;
    for (size_t i = 0; i < sn->in.n_meds; i++) {
        if (sn->in.meds[i].id == s_med_id) {
            m = &sn->in.meds[i];
            break;
        }
    }
    if (!m) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    draw_static(m);

    hy_tile_view_t tv = {0};
    if (sn->tracking) {
        hy_tile_view(m, &sn->in, sn->sched, &tv);
    }
    if (tv.ring != s_ring) {
        lv_arc_set_value(s_arc, tv.ring);
        s_ring = tv.ring;
    }

    int32_t today = 0;
    bool full = false;
    if (sn->tracking) {
        today = hy_local_date(sn->in.now, sn->in.tz);
        int32_t first = hy_date_add(today, -(DAYS - 1));
        int64_t since = hy_sched_tracked_since(sn->sched, m->id);
        uint32_t key = ui_fnv(2166136261u, &today, sizeof today);
        key = ui_fnv(key, sn->in.tz, strnlen(sn->in.tz, HY_TZ_LEN));
        if (sn->in.last_change) {
            key = ui_fnv(key, &sn->in.last_change->at, sizeof sn->in.last_change->at);
            key = ui_fnv(key, sn->in.last_change->new_tz, strnlen(sn->in.last_change->new_tz, HY_TZ_LEN));
        }
        key = ui_fnv(key, &since, sizeof since);
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
        for (size_t i = 0; sn->in.logs && i < sn->in.n_logs; i++) {   /* this medicine's logs in the 8 day window */
            const hy_log_t *l = &sn->in.logs[i];
            if (l->med_id == m->id) {
                key = ui_fnv(key, &l->id, sizeof l->id);
                key = ui_fnv(key, &l->ts, sizeof l->ts);
                key = ui_fnv(key, &l->status, sizeof l->status);
                key = ui_fnv(key, &l->slot, sizeof l->slot);
            }
        }
        full = s_inval || key != s_key;
        if (full) {
            load_logs(m, sn->in.now);
        }
        hy_sched_in_t in = sn->in;
        in.logs = s_buf;
        in.n_logs = s_nbuf;
        for (int d = full ? 0 : DAYS - 1; d < DAYS; d++) {
            bool prog = false;
            s_marks[d] = hy_day_mark(m, hy_date_add(first, d), &in, sn->sched, d == DAYS - 1 ? &prog : NULL);
            if (d == DAYS - 1) {
                s_prog = prog;
            }
        }
        if (full) {
            hy_adherence(m->id, first, DAYS, &in, sn->sched, &s_ad);
            s_key = key;
            s_inval = false;
        }
    } else {
        memset(s_marks, 0, sizeof s_marks);
        memset(&s_ad, 0, sizeof s_ad);
        s_nrecent = 0;
        s_prog = false;
    }
    int64_t t1 = esp_timer_get_time();
    draw_rows(sn->in.now, today);
    draw_cells(m, sn->tracking);
    ESP_LOGI(TAG, "detail med %u: %s, data %u us, lvgl %u us", (unsigned)m->id, full ? "full" : "cached",
             (unsigned)(t1 - t0), (unsigned)(esp_timer_get_time() - t1));
}

/* ---- public ---- */

void detail_invalidate(void)
{
    s_inval = true;
}

void detail_hide(void)
{
    s_open = false;
}

void detail_show(uint32_t med_id)
{
    if (med_id != s_med_id) {
        s_med_id = med_id;
        s_static_sig = s_rows_sig = s_cells_sig = 0;
        s_inval = true;
    }
    if (!s_open) {
        lv_obj_scroll_to_y(s_scroll, 0, LV_ANIM_OFF);
        s_open = true;
    }
    hy_sched_run_tick();   /* an action just before this show left the snapshot dirty */
    render();
}

#if CONFIG_HY_DEBUG_CONSOLE
void detail_debug_scroll(void)
{
    lv_obj_update_layout(s_scroll);
    lv_obj_scroll_to_y(s_scroll, lv_obj_get_scroll_bottom(s_scroll) + lv_obj_get_scroll_y(s_scroll), LV_ANIM_OFF);
}
#endif

void detail_refresh_if_changed(void)
{
    if (ui_current_view() == HY_V_DETAIL && hy_sched_generation() != s_gen_seen) {
        render();
    }
}

/* ---- widgets ---- */

static void scroll_cb(lv_event_t *e)
{
    (void)e;
    ui_input_consume();   /* a stroke that scrolls the detail must not also navigate */
}

static void button_cb(lv_event_t *e)
{
    if (!ui_input_last_was_tap()) {
        return;   /* a waking stroke or a swipe that ended on the button */
    }
    hy_view_t v = (hy_view_t)(uintptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "button: %s", v == HY_V_NOTE ? "log with note" : "log earlier");
    ui_nav_push(v);
}

static void make_button(lv_obj_t *parent, int x, const char *text, hy_view_t target)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BTN_W, BTN_H);
    lv_obj_set_pos(b, x, 0);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(HY_C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(HY_C_GRAB), LV_STATE_PRESSED);
    lv_obj_set_scrollable(b, false);
    lv_obj_set_clickable(b, true);
    lv_obj_set_scroll_chain(b, true);
    lv_obj_add_event_cb(b, button_cb, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)target);
    lv_obj_center(ui_label(b, hy_type.body_bold, HY_C_TEXT, text));
}

static lv_obj_t *one_line(lv_obj_t *parent, const lv_font_t *f, uint32_t color, int w)
{
    lv_obj_t *l = ui_label(parent, f, color, "");
    lv_obj_set_width(l, w);
    lv_obj_set_height(l, lv_font_get_line_height(f));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return l;
}

void detail_create(void)
{
    s_buf = heap_caps_malloc(BUF_CAP * sizeof(hy_log_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "no PSRAM for the log window: marks and entries stay empty");
    }
    s_root = ui_view_box();
    ui_register_view(HY_V_DETAIL, s_root);

    s_scroll = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_scroll);   /* stays clickable: children that are not clickable pass the stroke here */
    lv_obj_set_size(s_scroll, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_pos(s_scroll, 0, 0);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_scroll, 12, 0);
    lv_obj_set_style_pad_bottom(s_scroll, HY_MARGIN, 0);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_elastic(s_scroll, false);
    lv_obj_set_scroll_momentum(s_scroll, false);
    lv_obj_add_event_cb(s_scroll, scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);

    /* header: ring (outer 96, inner 80) with the code, then name, schedule and pills */
    lv_obj_t *hdr = ui_plain(s_scroll);
    lv_obj_set_size(hdr, HY_SCREEN, HEAD_H);
    s_arc = lv_arc_create(hdr);
    lv_obj_set_size(s_arc, RING_D, RING_D);
    lv_obj_set_pos(s_arc, 24, 1);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, HY_RING_MAX);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_arc, false);
    lv_obj_set_style_pad_all(s_arc, 0, 0);
    lv_obj_set_style_arc_width(s_arc, RING_THICK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(HY_C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, RING_THICK, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, false, LV_PART_INDICATOR);
    s_code = ui_label(s_arc, hy_type.code_m, HY_C_TEXT, "");
    lv_obj_center(s_code);

    int cw = HY_SCREEN - TEXT_X - CARD_M;
    lv_obj_t *col = ui_plain(hdr);
    lv_obj_set_size(col, cw, HEAD_H);
    lv_obj_set_pos(col, TEXT_X, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 1, 0);
    s_name = one_line(col, hy_type.title, HY_C_TEXT, cw);
    s_sched = one_line(col, hy_type.status, HY_C_TEXT2, cw);
    s_pills = one_line(col, hy_type.status, HY_C_TEXT2, cw);

    /* the three newest entries */
    lv_obj_t *card = ui_plain(s_scroll);
    lv_obj_set_width(card, HY_SCREEN - 2 * CARD_M);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_left(card, CARD_M, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(HY_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_pad_ver(card, 9, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < N_RECENT; i++) {
        s_rowbox[i] = ui_plain(card);
        lv_obj_set_size(s_rowbox[i], HY_SCREEN - 2 * CARD_M, ROW_H);
        s_when[i] = ui_label(s_rowbox[i], hy_type.body, HY_C_TEXT, "");
        lv_obj_align(s_when[i], LV_ALIGN_LEFT_MID, CARD_M, 0);
        s_what[i] = ui_label(s_rowbox[i], hy_type.status, HY_C_TEXT2, "");
        lv_obj_align(s_what[i], LV_ALIGN_RIGHT_MID, -CARD_M, 0);
        lv_obj_set_hidden(s_rowbox[i], true);
    }

    /* buttons */
    lv_obj_t *brow = ui_plain(s_scroll);
    lv_obj_set_size(brow, HY_SCREEN - 2 * CARD_M, BTN_H);
    lv_obj_set_style_margin_left(brow, CARD_M, 0);
    make_button(brow, 0, HY_S_DET_NOTE, HY_V_NOTE);
    make_button(brow, HY_SCREEN - 2 * CARD_M - BTN_W, HY_S_DET_EARLIER, HY_V_EARLIER);

    /* 30 days */
    lv_obj_t *title = ui_label(s_scroll, hy_type.body_bold, HY_C_TEXT, HY_S_DET_30_DAYS);
    lv_obj_set_style_margin_left(title, 24, 0);
    lv_obj_set_style_margin_top(title, 6, 0);
    s_grid = ui_plain(s_scroll);
    lv_obj_set_size(s_grid, COLS * CELL + (COLS - 1) * CELL_GAP, 2 * CELL + CELL_GAP);
    lv_obj_set_style_margin_left(s_grid, 24, 0);
    for (int i = 0; i < DAYS; i++) {
        lv_obj_t *c = ui_plain(s_grid);
        s_cell[i] = c;
        lv_obj_set_size(c, CELL, CELL);
        lv_obj_set_pos(c, (i % COLS) * (CELL + CELL_GAP), (i / COLS) * (CELL + CELL_GAP));
        lv_obj_set_style_radius(c, 6, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(HY_C_NA), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_t *d = ui_plain(c);
        s_dot[i] = d;
        lv_obj_set_size(d, 4, 4);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(HY_C_AMBER), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_center(d);
        lv_obj_set_hidden(d, true);
    }
    s_adh = ui_label(s_scroll, hy_type.status, HY_C_TEXT2, "");
    lv_obj_set_style_margin_left(s_adh, 24, 0);
}
