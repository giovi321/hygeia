/* Due card: the full-content overlay shown while a dose is due or late.
 *
 * One entry at a time out of the scheduler snapshot's open and unsnoozed entries. The knob logs the dose,
 * a swipe left snoozes, Skip skips, a swipe up steps to the next entry. Every log shows the logged or skipped
 * toast with Undo. The card sits inside ui_content() so the top bar stays and the toast (created later on the
 * screen) is drawn above it.
 *
 * Everything here runs in the LVGL task. The snapshot is read only from due_refresh(), which the redraw paths
 * call when the evaluation generation changed; the action handlers never read it. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hy/actions.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "slide.h"
#include "ui_internal.h"

static const char *TAG = "ui_due";

#define RING_D        200
#define RING_Y        6
#define RING_W        12
#define DISC_D        (RING_D - 2 * RING_W)
#define NAME_Y        215
#define DUE_Y         255
#define TRACK_X       26
#define TRACK_Y       291
#define TRACK_W       SLIDE_TRACK_W
#define TRACK_H       SLIDE_TRACK_H
#define ROW_CY        404          /* centre line of the bottom row */
#define SIDE_X        40
#define SKIP_W        120
#define SKIP_H        48
#define FADE_MS       150
#define FLASH_STEP_MS 200
#define PD_MAX        6            /* entry dots at the left edge; the last one stands for "this and more" */
#define PD            8
#define PD_ACTIVE_H   22
#define PD_GAP        8
#define PD_X          12
#define FLASH_PHASES  6            /* three 200 ms flashes, each followed by 200 ms back */

typedef struct {
    uint32_t med_id;
    int64_t slot, base;
    hy_occ_state_t state;
    uint8_t color;
    char code[3], name[33], dose[17];
} entry_t;

static HY_EXT_BSS entry_t s_list[HY_SCHED_MAX_OPEN];
static HY_EXT_BSS entry_t s_new[HY_SCHED_MAX_OPEN];
static size_t s_n, s_idx;
static char s_tz[HY_TZ_LEN];
static uint32_t s_gen_seen;
static bool s_visible;

static lv_obj_t *s_dots_box, *s_dots[PD_MAX];
static lv_obj_t *s_card, *s_ring, *s_code, *s_name, *s_due, *s_track, *s_knob, *s_check, *s_slide_lbl,
    *s_snooze_lbl, *s_skip_lbl, *s_skip_hit;

/* knob drag */
static slide_t s_slide;
static entry_t s_cap;          /* the entry the knob was grabbed on: (med, slot, code) */
static bool s_deferred;        /* a refresh arrived during the drag; applied after the release */

/* flash */
static lv_timer_t *s_flash_t;
static bool s_flash_on;
static uint8_t s_flash_phase;

/* what the last "due: show" line said, so a refresh with nothing new stays quiet */
static uint32_t s_logged_med;
static int64_t s_logged_slot;
static size_t s_logged_idx, s_logged_n;

static const lv_point_precise_t check_pts[] = {{21, 38}, {30, 47}, {50, 26}};

/* ---- helpers ---- */

static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t) != 0) {
        lv_label_set_text(l, t);
    }
}

static entry_t *cur(void)
{
    return s_idx < s_n ? &s_list[s_idx] : NULL;
}

static void now_hhmm(char *out, size_t len)
{
    char tz[HY_TZ_LEN];
    hy_clock_tz_copy(tz, sizeof tz);
    ui_fmt_hhmm(hy_clock_port()->now(NULL), tz, out, len);
}

static void opa_exec(void *o, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
}

static void knob_reset(void)
{
    slide_reset(&s_slide);
}

/* ---- speaker failure flash ---- */

static bool drag_begin(void);

static void flash_apply(bool bright)
{
    ESP_LOGI(TAG, "due: flash %s", bright ? "bright" : "back");
    idle_brightness_stop();
    hy_board_set_brightness(bright ? 100 : hy_settings()->brightness);
    lv_obj_set_style_arc_opa(s_ring, bright ? LV_OPA_COVER : LV_OPA_40, LV_PART_INDICATOR);
}

static void flash_stop(void)
{
    if (s_flash_t) {
        lv_timer_pause(s_flash_t);
    }
    if (s_flash_on) {
        s_flash_on = false;
        idle_brightness_stop();
        hy_board_set_brightness(hy_settings()->brightness);
        lv_obj_set_style_arc_opa(s_ring, LV_OPA_COVER, LV_PART_INDICATOR);
    }
}

static void flash_cb(lv_timer_t *t)
{
    (void)t;
    if (++s_flash_phase >= FLASH_PHASES) {
        flash_stop();
        return;
    }
    flash_apply(s_flash_phase % 2 == 0);
}

static void flash_start(void)
{
    ESP_LOGI(TAG, "due: flash (speaker unavailable)");
    s_flash_phase = 0;
    s_flash_on = true;
    flash_apply(true);
    if (!s_flash_t) {
        s_flash_t = lv_timer_create(flash_cb, FLASH_STEP_MS, NULL);
    } else {
        lv_timer_reset(s_flash_t);
        lv_timer_resume(s_flash_t);
    }
}

/* ---- drawing ---- */

/* One dot per showing entry, at most PD_MAX. With more entries the last dot is smaller and covers the rest.
 * The current entry's dot is taller and in the medicine colour. Hidden when one entry shows. */
static void dots_render(lv_color_t col)
{
    size_t shown = s_n > PD_MAX ? PD_MAX : s_n;
    if (s_n < 2) {
        lv_obj_set_hidden(s_dots_box, true);
        return;
    }
    lv_obj_set_hidden(s_dots_box, false);
    size_t active = s_idx < PD_MAX ? s_idx : PD_MAX - 1;
    for (size_t i = 0; i < PD_MAX; i++) {
        if (i >= shown) {
            lv_obj_set_hidden(s_dots[i], true);
            continue;
        }
        bool on = i == active;
        bool more = s_n > PD_MAX && i == PD_MAX - 1;
        lv_obj_set_hidden(s_dots[i], false);
        lv_obj_set_size(s_dots[i], PD, on ? PD_ACTIVE_H : more ? PD - 3 : PD);
        lv_obj_set_style_bg_color(s_dots[i], on ? col : lv_color_hex(HY_C_DOT), 0);
    }
}

static void render(void)
{
    entry_t *e = cur();
    if (!e) {
        return;
    }
    lv_color_t col = hy_med_color(e->color);
    lv_obj_set_style_arc_color(s_ring, col, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_code, col, 0);
    lv_obj_set_style_bg_color(s_knob, col, 0);
    set_text(s_code, e->code);
    dots_render(col);

    char buf[96];
    if (e->dose[0]) {
        snprintf(buf, sizeof buf, HY_S_DUE_NAME, e->name, e->dose);
    } else {
        snprintf(buf, sizeof buf, "%s", e->name);
    }
    set_text(s_name, buf);

    char hhmm[8];
    ui_fmt_hhmm(e->base, s_tz, hhmm, sizeof hhmm);
    size_t o = (size_t)snprintf(buf, sizeof buf, e->state == HY_OCC_LATE ? HY_S_DUE_LATE : HY_S_DUE_AT, hhmm);
    if (s_n > 1 && o < sizeof buf) {
        snprintf(buf + o, sizeof buf - o, HY_S_DUE_MORE, (unsigned)(s_n - 1));
    }
    set_text(s_due, buf);

    snprintf(buf, sizeof buf, HY_S_DUE_SNOOZE, (unsigned)hy_settings()->snooze_min);
    set_text(s_snooze_lbl, buf);

    if (!s_visible || s_logged_med != e->med_id || s_logged_slot != e->slot || s_logged_idx != s_idx ||
        s_logged_n != s_n) {
        char sl[8];
        ui_fmt_hhmm(e->slot, s_tz, sl, sizeof sl);
        ESP_LOGI(TAG, "due: show %s slot %s (%u of %u)", e->code, sl, (unsigned)(s_idx + 1), (unsigned)s_n);
        s_logged_med = e->med_id;
        s_logged_slot = e->slot;
        s_logged_idx = s_idx;
        s_logged_n = s_n;
    }
}

static void show_card(void)
{
    s_visible = true;
    knob_reset();
    ui_nav_set_due(true);
    idle_wake_due();   /* DUE: a due dose wakes a dimmed or dark screen, on USB and on battery */
    lv_obj_set_style_opa(s_card, LV_OPA_TRANSP, 0);
    lv_obj_set_hidden(s_card, false);
    lv_obj_move_foreground(s_card);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_card);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void hide_card(void)
{
    s_visible = false;
    flash_stop();
    knob_reset();
    lv_anim_delete(s_card, opa_exec);
    lv_obj_set_hidden(s_card, true);
    ui_nav_set_due(false);
    s_logged_med = 0;
    ESP_LOGI(TAG, "due: hide");
}

static const hy_med_t *snap_med(const hy_snapshot_t *sn, uint32_t id)
{
    for (size_t i = 0; i < sn->in.n_meds; i++) {
        if (sn->in.meds[i].id == id) {
            return &sn->in.meds[i];
        }
    }
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    for (size_t i = 0; i < n; i++) {
        if (ms[i].id == id) {
            return &ms[i];
        }
    }
    return NULL;
}

void due_refresh(void)
{
    if (!s_card) {
        return;
    }
    uint32_t gen = hy_sched_generation();
    if (gen == 0 || gen == s_gen_seen) {
        return;
    }
    s_gen_seen = gen;
    const hy_snapshot_t *sn = hy_sched_snapshot();

    size_t n = 0;
    if (sn->tracking) {
        for (size_t i = 0; i < sn->out.n_open && n < HY_SCHED_MAX_OPEN; i++) {
            const hy_open_t *op = &sn->out.open[i];
            const hy_med_t *m = op->snoozed_until == 0 ? snap_med(sn, op->med_id) : NULL;
            if (!m) {
                continue;
            }
            entry_t *e = &s_new[n++];
            e->med_id = op->med_id;
            e->slot = op->slot;
            e->base = op->base;
            e->state = op->state;
            e->color = m->color;
            snprintf(e->code, sizeof e->code, "%s", m->code);
            snprintf(e->name, sizeof e->name, "%s", m->name);
            snprintf(e->dose, sizeof e->dose, "%s", m->dose);
        }
    }

    bool same = n == s_n;
    for (size_t i = 0; same && i < n; i++) {
        same = s_new[i].med_id == s_list[i].med_id && s_new[i].slot == s_list[i].slot;
    }
    memcpy(s_list, s_new, n * sizeof s_list[0]);
    s_n = n;
    if (!same || s_idx >= s_n) {
        s_idx = 0;
    }

    if (s_slide.drag) {
        /* P38: the card does not change under the finger; the list is kept, drawing waits for the release */
        s_deferred = true;
        ESP_LOGI(TAG, "due: refresh deferred during the knob drag (%u showing)", (unsigned)n);
        return;
    }

    if (n == 0) {
        if (s_visible) {
            hide_card();
        }
        return;
    }
    if (sn->in.tz) {
        snprintf(s_tz, sizeof s_tz, "%s", sn->in.tz);
    }
    if (!s_visible) {
        show_card();
    } else if (sn->out.n_newly_due > 0) {
        idle_wake_due();   /* a dose joined the open card: wake a dimmed or dark screen too (R16) */
    }
    render();
    if (sn->flash && hy_sched_take_flash()) {
        flash_start();
    }
}

void due_raise(void)
{
    if (s_card) {
        lv_obj_move_foreground(s_card);
    }
}

bool due_visible(void)
{
    return s_visible;
}

#if CONFIG_HY_DEBUG_CONSOLE
/* "due drag": a knob grab on the shown entry with the knob at the end of the track; "due drop": the release.
 * Anything that changes the list in between (tap, remindtest, due snooze) tests the frozen entry. */
void due_drag_debug(bool drop)
{
    if (!drop) {
        if (!drag_begin()) {
            ESP_LOGW(TAG, "drag: nothing to grab");
            return;
        }
        slide_debug_grab(&s_slide);
        ESP_LOGI(TAG, "drag: grabbed %s slot %lld at %d px", s_cap.code, (long long)s_cap.slot, (int)s_slide.off);
    } else if (s_slide.drag) {
        slide_end(&s_slide, true);
    } else {
        ESP_LOGW(TAG, "drop: no drag");
    }
}

void due_flash_debug(void)
{
    if (s_visible) {
        flash_start();
    } else {
        ESP_LOGW(TAG, "flash: nothing is due");
    }
}
#endif

/* ---- actions ---- */

void due_next(void)
{
    if (!s_visible || s_n < 2) {
        return;
    }
    s_idx = (s_idx + 1) % s_n;
    render();
}

void due_prev(void)
{
    if (!s_visible || s_n < 2) {
        return;
    }
    s_idx = (s_idx + s_n - 1) % s_n;
    render();
}

void due_snooze(void)
{
    entry_t *e = cur();
    if (!s_visible || !e) {
        ESP_LOGW(TAG, "snooze: nothing is due");
        return;
    }
    char code[3];
    snprintf(code, sizeof code, "%s", e->code);
    int64_t until = 0;
    hy_err_t err = hy_act_snooze(e->med_id, hy_settings()->snooze_min, HY_SRC_SCREEN, &until);
    if (err == HY_OK) {
        char hhmm[8], msg[48];
        ui_fmt_hhmm(until, s_tz, hhmm, sizeof hhmm);
        ESP_LOGI(TAG, "snooze %s: %s until %s", code, hy_err_str(err), hhmm);
        snprintf(msg, sizeof msg, HY_S_SNOOZED_UNTIL, hhmm);
        toast_show_ok(msg);
    } else {
        ESP_LOGW(TAG, "snooze %s: %s (%d)", code, hy_err_str(err), (int)err);
        const char *t = ui_err_text(err);
        toast_show_info(strcmp(t, HY_S_ERR_LOG) == 0 ? HY_S_ERR_SNOOZE : t);
    }
}

/* Result handling shared by take and skip */
static void act_result(const char *what, const char *code, hy_err_t err, uint32_t log_id, bool skipped)
{
    ESP_LOGI(TAG, "%s %s: %s (%d), log id %u", what, code, hy_err_str(err), (int)err, (unsigned)log_id);
    if (err != HY_OK) {
        toast_show_info(ui_err_text(err));
        return;
    }
    char hhmm[8];
    now_hhmm(hhmm, sizeof hhmm);
    if (skipped) {
        toast_show_skipped(code, hhmm, log_id);
    } else {
        toast_show_logged(code, hhmm, log_id);
    }
    ui_log_hwm(what);
}

static void take_entry(const entry_t *e)
{
    uint32_t log_id = 0;
    hy_err_t err = hy_act_log_taken(e->med_id, 0, NULL, HY_SRC_SCREEN, &log_id);
    act_result("take", e->code, err, log_id, false);
}

void due_slide_complete(void)
{
    entry_t *e = cur();
    if (!s_visible || !e) {
        ESP_LOGW(TAG, "take: nothing is due");
        return;
    }
    entry_t copy = *e;   /* the action republishes events; do not point into the list */
    take_entry(&copy);
}

void due_skip(void)
{
    entry_t *e = cur();
    if (!s_visible || !e) {
        ESP_LOGW(TAG, "skip: nothing is due");
        return;
    }
    uint32_t log_id = 0;
    char code[3];
    snprintf(code, sizeof code, "%s", e->code);
    hy_err_t err = hy_act_skip(e->med_id, HY_SRC_SCREEN, &log_id);
    act_result("skip", code, err, log_id, true);
}

/* ---- touch ---- */

static void deferred_refresh(void *arg)
{
    (void)arg;
    s_gen_seen = 0;   /* the next due_refresh redraws from the latest snapshot */
    due_refresh();
}

/* The drag starts on the shown entry; that entry, and no other, is what a release can log (P38) */
static bool drag_begin(void)
{
    entry_t *e = cur();
    if (!s_visible || !e || ui_nav_idle() || toast_logged_visible()) {
        return false;
    }
    s_cap = *e;
    s_deferred = false;
    return true;
}

/* The slide ended (the knob is back at the start): a complete one logs the captured slot if it is still open */
static void drag_end(bool complete)
{
    if (complete) {
        const entry_t *open = NULL;   /* the captured slot, if it is still showing in the latest list */
        for (size_t i = 0; i < s_n; i++) {
            if (s_list[i].med_id == s_cap.med_id && s_list[i].slot == s_cap.slot) {
                open = &s_list[i];
            }
        }
        if (open) {
            entry_t copy = *open;
            take_entry(&copy);
        } else {
            ESP_LOGW(TAG, "take %s: slot no longer open, nothing logged", s_cap.code);
        }
    }
    if (s_deferred) {
        s_deferred = false;
        lv_async_call(deferred_refresh, NULL);
    }
}

static void skip_cb(lv_event_t *e)
{
    (void)e;
    if (!ui_input_last_was_tap()) {
        ESP_LOGI(TAG, "skip click ignored: stroke was not a tap");
        return;
    }
    due_skip();
}

/* ---- construction ---- */

static lv_obj_t *plain(lv_obj_t *parent, int32_t w, int32_t h, int32_t x, int32_t y)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *text_line(lv_obj_t *parent, const lv_font_t *f, uint32_t color, int32_t y, int32_t w, int32_t x)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, w, lv_font_get_line_height(f));
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

void due_create(void)
{
    s_card = plain(ui_content(), HY_SCREEN, HY_CONTENT_H, 0, 0);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_card, true);   /* swallows every touch so nothing underneath logs a dose */
    lv_obj_set_hidden(s_card, true);

    s_dots_box = plain(s_card, PD, LV_SIZE_CONTENT, PD_X, 0);
    lv_obj_set_height(s_dots_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_dots_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_dots_box, PD_GAP, 0);
    lv_obj_align(s_dots_box, LV_ALIGN_LEFT_MID, PD_X, 0);
    lv_obj_set_hidden(s_dots_box, true);
    for (size_t i = 0; i < PD_MAX; i++) {
        s_dots[i] = plain(s_dots_box, PD, PD, 0, 0);
        lv_obj_set_style_radius(s_dots[i], PD / 2, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(HY_C_DOT), 0);
    }

    const int32_t ring_x = (HY_SCREEN - RING_D) / 2;
    lv_obj_t *disc = plain(s_card, DISC_D, DISC_D, ring_x + RING_W, RING_Y + RING_W);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(HY_C_INNER), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);

    s_ring = lv_arc_create(s_card);
    lv_obj_set_size(s_ring, RING_D, RING_D);
    lv_obj_set_pos(s_ring, ring_x, RING_Y);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, 100);
    lv_arc_set_value(s_ring, 100);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_ring, false);
    lv_obj_set_style_pad_all(s_ring, 0, 0);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(HY_C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_ring, false, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_ring, false, LV_PART_INDICATOR);

    const lv_font_t *fc = hy_type.code_l;
    s_code = text_line(s_card, fc, HY_C_TEXT, RING_Y + RING_D / 2 - lv_font_get_line_height(fc) / 2 + 7, RING_D, ring_x);

    s_name = text_line(s_card, hy_type.title, HY_C_TEXT, NAME_Y, HY_SCREEN - 40, 20);
    s_due = text_line(s_card, hy_type.status, HY_C_TEXT2, DUE_Y, HY_SCREEN - 40, 20);

    s_track = plain(s_card, TRACK_W, TRACK_H, TRACK_X, TRACK_Y);
    lv_obj_set_style_radius(s_track, TRACK_H / 2, 0);
    lv_obj_set_style_bg_color(s_track, lv_color_hex(HY_C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(s_track, LV_OPA_COVER, 0);
    lv_obj_t *tl = lv_label_create(s_track);
    s_slide_lbl = tl;
    lv_obj_set_style_text_font(tl, hy_type.body, 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(HY_C_TEXT2), 0);
    lv_label_set_text(tl, HY_S_DUE_SLIDE);
    lv_obj_center(tl);

    s_knob = plain(s_track, SLIDE_KNOB_D, SLIDE_KNOB_D, SLIDE_KNOB_PAD, SLIDE_KNOB_PAD);
    lv_obj_set_style_radius(s_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_knob, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_knob, true);
    s_slide = (slide_t){.knob = s_knob, .tag = TAG, .begin = drag_begin, .end = drag_end};
    slide_attach(&s_slide);
    s_check = lv_line_create(s_knob);
    lv_line_set_points(s_check, check_pts, 3);
    lv_obj_set_style_line_width(s_check, 5, 0);
    lv_obj_set_style_line_color(s_check, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_line_rounded(s_check, true, 0);

    const lv_font_t *fs = hy_type.status;
    s_snooze_lbl = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_snooze_lbl, fs, 0);
    lv_obj_set_style_text_color(s_snooze_lbl, lv_color_hex(HY_C_TEXT2), 0);
    lv_label_set_text(s_snooze_lbl, "");
    lv_obj_set_pos(s_snooze_lbl, SIDE_X, ROW_CY - lv_font_get_line_height(fs) / 2);

    s_skip_lbl = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_skip_lbl, fs, 0);
    lv_obj_set_style_text_color(s_skip_lbl, lv_color_hex(HY_C_TEXT2), 0);
    lv_label_set_text(s_skip_lbl, HY_S_DUE_SKIP);
    lv_obj_align(s_skip_lbl, LV_ALIGN_TOP_RIGHT, -SIDE_X, ROW_CY - lv_font_get_line_height(fs) / 2);

    /* 120 x 48 hit area around the label, kept inside the card */
    s_skip_hit = plain(s_card, SKIP_W, SKIP_H, HY_SCREEN - SKIP_W, HY_CONTENT_H - SKIP_H);
    lv_obj_set_clickable(s_skip_hit, true);
    lv_obj_add_event_cb(s_skip_hit, skip_cb, LV_EVENT_SHORT_CLICKED, NULL);
}
