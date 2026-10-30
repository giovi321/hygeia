/* The medicine form: a step-by-step view that adds or edits a medicine on the device. hy_core (hy/medform.h) owns
 * the draft, the field rules and the step order; this file only draws the pages and drives those functions.
 *
 * One container per page, only the current one visible. The title, the step counter, the error line and the Back and
 * Next buttons are shared and re-laid out per page. Name and Dose share one text area and one keyboard. Nothing is
 * saved before the Save button of the Review page; the draft is static RAM, so it survives dimming, a due card and
 * the power-off overlay. Every button goes through ui_on_tap (still, awake taps only) and the text area ignores an
 * OK key that only woke the screen. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "hy/actions.h"
#include "hy/clock.h"
#include "hy/medform.h"
#include "ui_internal.h"

LV_FONT_DECLARE(lv_font_montserrat_14);

static const char *TAG = "ui_medform";

typedef enum {
    PG_NAME, PG_DOSE, PG_CODE, PG_PICK, PG_COLOR, PG_TYPE, PG_TIMES, PG_ADD_TIME, PG_INTERVAL, PG_REVIEW,
    PG_DISCARD, PG_N
} page_t;

#define PAD_X      18                       /* left edge of full-width controls */
#define FULL_W     (HY_SCREEN - 2 * PAD_X)  /* 444 */
#define BTN_Y      336                      /* bottom button row */
#define BACK_W     144
#define NEXT_W     288
#define NEXT_X     174
#define TITLE_MAX_W 432
#define N_CODE_KEYS 36                      /* A to Z, 0 to 9 */
#define N_COLORS   10
#define N_OPTS     12                       /* interval roller options: 10 fixed plus one extra, with room */

static hy_medform_t s_f;
static page_t s_page, s_discard_from;
static bool s_fresh;                        /* just opened: the first medform_show draws the page; later shows keep the sub-page state */
static bool s_loading;                      /* the text area is being set by code: its change handler stays quiet */
static bool s_via_code_row;                 /* opened from the Code and colour row: Next goes on to Colour */
static char s_pick[3];                      /* the code typed in the picker */
static bool s_pick_used[N_CODE_KEYS];       /* single characters another active medicine uses */
static uint16_t s_iopts[N_OPTS];
static size_t s_n_iopts;

static lv_obj_t *s_root, *s_title, *s_step, *s_err, *s_back, *s_next, *s_back_l, *s_next_l;
static lv_obj_t *s_c[PG_N];                 /* page containers; Name and Dose share s_c[PG_NAME] */
static lv_obj_t *s_ta, *s_kb;
static lv_obj_t *s_ring, *s_code_lbl;
static lv_obj_t *s_pick_prev, *s_pick_grid;
static lv_obj_t *s_dot[N_COLORS], *s_dot_lbl[N_COLORS], *s_color_prev;
static lv_obj_t *s_opt[2];
static lv_obj_t *s_days, *s_list, *s_add_btn;
static struct { lv_obj_t *row, *time, *rm; } s_rows[HY_MAX_TIMES];
static lv_obj_t *s_hour, *s_min;
static lv_obj_t *s_iroller;
static struct { lv_obj_t *val; } s_rv[4];
static lv_font_t s_pick_font;               /* Inter 26 bold with the Montserrat fallback for the backspace symbol */

static const char *const s_step_names[] = {"NAME", "DOSE", "CODE", "COLOR", "TYPE", "TIMES", "INTERVAL", "REVIEW"};

static const char *step_name(hy_mf_step_t s)
{
    return (unsigned)s < sizeof s_step_names / sizeof s_step_names[0] ? s_step_names[s] : "?";
}

static void log_err(hy_err_t e, hy_mf_step_t at)
{
    ESP_LOGW(TAG, "medform: %s at %s", hy_err_str(e), step_name(at));
}

/* ---- small helpers ---- */

static bool form_open(void)
{
    return s_root && ui_current_view() == HY_V_MEDFORM;
}

static void set_err(const char *text)
{
    lv_label_set_text(s_err, text ? text : "");
}

static void fmt_hhmm(uint16_t min, char *out, size_t len)
{
    snprintf(out, len, "%02u:%02u", (unsigned)(min / 60), (unsigned)(min % 60));
}

/* "No minimum", "30 min", "1 h", "1 h 30 min" */
static void fmt_interval(uint16_t min, char *out, size_t len)
{
    if (min == 0) {
        snprintf(out, len, HY_S_MF_NO_MIN);
    } else if (min < 60) {
        snprintf(out, len, HY_S_MF_MIN, (int)min);
    } else if (min % 60 == 0) {
        snprintf(out, len, HY_S_MF_HOUR, (int)(min / 60));
    } else {
        snprintf(out, len, HY_S_MF_HOUR_MIN, (int)(min / 60), (int)(min % 60));
    }
}

static hy_mf_step_t sched_step(void)
{
    return s_f.draft.type == HY_SCHED_FIXED ? HY_MF_TIMES : HY_MF_INTERVAL;
}

static page_t page_of(hy_mf_step_t s)
{
    switch (s) {
    case HY_MF_NAME:     return PG_NAME;
    case HY_MF_DOSE:     return PG_DOSE;
    case HY_MF_CODE:     return PG_CODE;
    case HY_MF_COLOR:    return PG_COLOR;
    case HY_MF_TYPE:     return PG_TYPE;
    case HY_MF_TIMES:    return PG_TIMES;
    case HY_MF_INTERVAL: return PG_INTERVAL;
    case HY_MF_REVIEW:   return PG_REVIEW;
    }
    return PG_NAME;
}

static hy_mf_step_t first_step(void)
{
    return s_f.edit_id ? HY_MF_REVIEW : HY_MF_NAME;
}

/* Text for an error the form can fix on a page. dup_id names the other medicine for HY_E_CODE_DUP. */
static const char *err_text(hy_err_t e, const char *code, uint32_t dup_id, char *buf, size_t len)
{
    switch (e) {
    case HY_E_NAME:     return HY_S_MF_ERR_NAME;
    case HY_E_DOSE:     return HY_S_MF_ERR_DOSE;
    case HY_E_CODE:     return HY_S_MF_ERR_CODE;
    case HY_E_CODE_DUP: {
        const hy_med_t *o = ui_find_med(dup_id);
        snprintf(buf, len, HY_S_MF_ERR_DUP, code, o ? o->name : "");
        return buf;
    }
    case HY_E_INVALID:  return HY_S_MF_ERR_COLOR;
    case HY_E_TIMES:    return HY_S_MF_ERR_TIMES;
    case HY_E_INTERVAL: return HY_S_MF_ERR_INTERVAL;
    default:            return HY_S_MF_ERR_WEB;
    }
}

/* ---- widget factories ---- */

static lv_obj_t *new_page(void)
{
    lv_obj_t *p = ui_plain(s_root);
    lv_obj_set_size(p, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_hidden(p, true);
    return p;
}

static void style_surface(lv_obj_t *o, int32_t radius)
{
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(HY_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

static void consume_cb(lv_event_t *e)
{
    (void)e;
    ui_input_consume();   /* a key press, a roller turn or a list scroll is the widget's stroke */
}

/* ---- navigation inside the form ---- */

static void show_page(page_t pg);
static void set_title(const char *t, bool step);
static void do_next(void);
static void do_back(void);

static void go_step(hy_mf_step_t step)
{
    s_f.step = step;
    if (step == HY_MF_REVIEW) {
        s_f.from_review = false;
        s_via_code_row = false;
    }
    if (step == HY_MF_CODE && !s_f.draft.code[0]) {
        const hy_med_t *ms;
        size_t n = hy_meds(&ms);
        char c[3];
        hy_mf_suggest_code(s_f.draft.name, ms, n, s_f.edit_id, c);
        if (c[0]) {
            hy_mf_set_code(&s_f, c, ms, n, NULL);
        }
    }
    show_page(page_of(step));
}

static void close_form(void)
{
    ESP_LOGI(TAG, "medform: closed");
    ui_nav_pop();   /* nav_host clears the dirty flag when the form leaves the stack */
}

/* Back on the first page, Cancel on Review (edit) and the edge swipe: leave at once, or ask first */
static void leave(void)
{
    if (s_f.dirty) {
        s_discard_from = s_page;
        show_page(PG_DISCARD);
    } else {
        close_form();
    }
}

static void open_from_review(hy_mf_step_t step)
{
    s_f.from_review = true;
    s_via_code_row = step == HY_MF_CODE;
    go_step(step);
}

/* ---- the text pages: Name and Dose ---- */

static bool on_text_page(void)
{
    return s_page == PG_NAME || s_page == PG_DOSE;
}

static void text_load(void)
{
    bool name = s_page == PG_NAME;
    s_loading = true;
    lv_textarea_set_max_length(s_ta, name ? HY_MF_NAME_MAX : HY_MF_DOSE_MAX);
    lv_textarea_set_placeholder_text(s_ta, name ? HY_S_MF_NAME_HINT : HY_S_MF_DOSE_HINT);
    lv_textarea_set_text(s_ta, name ? s_f.draft.name : s_f.draft.dose);
    s_loading = false;
    if (lv_keyboard_get_mode(s_kb) != LV_KEYBOARD_MODE_TEXT_LOWER) {
        lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);   /* shift or symbols do not carry over */
    }
}

/* Typing updates the draft at once (a value the rules reject leaves the draft as it was), so a half-typed name counts
 * as a change and leaving asks first */
static void ta_changed_cb(lv_event_t *e)
{
    (void)e;
    if (s_loading || !on_text_page() || ui_input_waking()) {   /* a waking keystroke is restored by the keyboard */
        return;
    }
    const char *t = lv_textarea_get_text(s_ta);
    if (s_page == PG_NAME) {
        hy_mf_set_name(&s_f, t);
    } else {
        hy_mf_set_dose(&s_f, t);
    }
    ui_nav_set_form_dirty(s_f.dirty);
}

/* The keyboard's OK key is Next, but never for a stroke that only woke the dimmed screen */
static void ta_ready_cb(lv_event_t *e)
{
    (void)e;
    if (ui_input_waking()) {
        ESP_LOGI(TAG, "OK ignored: the stroke only woke the screen");
        return;
    }
    if (form_open() && on_text_page()) {
        do_next();
    }
}

static void render_text(void)
{
    text_load();
    lv_obj_set_size(s_back, BACK_W, 56);
    lv_obj_set_pos(s_back, PAD_X, 116);
    lv_obj_set_size(s_next, NEXT_W, 56);
    lv_obj_set_pos(s_next, NEXT_X, 116);
    lv_obj_set_pos(s_err, PAD_X, 178);
}

/* ---- Code page and picker ---- */

static void render_code(void)
{
    bool two = s_f.draft.code[1] != 0;
    lv_obj_set_style_border_color(s_ring, hy_med_color(s_f.draft.color), 0);
    lv_obj_set_style_text_font(s_code_lbl, two ? hy_type.code : hy_type.code_l, 0);
    lv_label_set_text(s_code_lbl, s_f.draft.code);
    lv_obj_center(s_code_lbl);
}

static void pick_refresh(void)
{
    lv_label_set_text(s_pick_prev, s_pick);
    lv_obj_set_style_text_color(s_pick_prev, hy_med_color(s_f.draft.color), 0);
    lv_obj_invalidate(s_pick_grid);
}

static void render_pick(void)
{
    snprintf(s_pick, sizeof s_pick, "%s", s_f.draft.code);
    memset(s_pick_used, 0, sizeof s_pick_used);
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    for (size_t i = 0; i < n; i++) {
        const char *c = ms[i].code;
        if (ms[i].archived || ms[i].id == s_f.edit_id || !c[0] || c[1]) {
            continue;
        }
        int k = c[0] >= 'A' && c[0] <= 'Z' ? c[0] - 'A' : (c[0] >= '0' && c[0] <= '9' ? 26 + c[0] - '0' : -1);
        if (k >= 0) {
            s_pick_used[k] = true;
        }
    }
    pick_refresh();
}

/* Done: the typed code into the draft, then the Code page; a rejected code shows its error there */
static void pick_commit(void)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    uint32_t dup = 0;
    char code[3];
    snprintf(code, sizeof code, "%s", s_pick);
    hy_err_t e = hy_mf_set_code(&s_f, code, ms, n, &dup);
    s_f.step = HY_MF_CODE;
    show_page(PG_CODE);
    if (e != HY_OK) {
        char b[96];
        log_err(e, HY_MF_CODE);
        set_err(err_text(e, code, dup, b, sizeof b));
    }
}

static void pick_done_cb(void *ud)
{
    (void)ud;
    if (form_open() && s_page == PG_PICK) {
        pick_commit();
    }
}

static void pick_key_cb(lv_event_t *e)
{
    if (!ui_input_last_was_tap() || !form_open() || s_page != PG_PICK) {
        return;   /* a waking stroke or a swipe that ended on a key */
    }
    lv_obj_t *g = lv_event_get_target_obj(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(g);
    size_t len = strlen(s_pick);
    if (id < N_CODE_KEYS) {
        if (len < 2) {   /* a third character is ignored */
            s_pick[len] = (char)(id < 26 ? 'A' + id : '0' + (id - 26));
            s_pick[len + 1] = 0;
        }
    } else if (id == N_CODE_KEYS && len > 0) {
        s_pick[len - 1] = 0;
    }
    pick_refresh();
}

static void pick_draw_cb(lv_event_t *e)
{
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(t);
    if (!base || base->part != LV_PART_ITEMS || base->id1 >= N_CODE_KEYS || !s_pick_used[base->id1]) {
        return;
    }
    if (lv_draw_task_get_type(t) == LV_DRAW_TASK_TYPE_LABEL) {
        lv_draw_task_get_label_dsc(t)->color = lv_color_hex(HY_C_TEXT2);   /* used by another medicine, still tappable */
    }
}

static void change_code_cb(void *ud)
{
    (void)ud;
    if (form_open() && s_page == PG_CODE) {
        show_page(PG_PICK);
    }
}

/* ---- Colour page ---- */

static const hy_med_t *color_user(uint8_t idx)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    for (size_t i = 0; i < n; i++) {
        if (!ms[i].archived && ms[i].id != s_f.edit_id && ms[i].color == idx) {
            return &ms[i];
        }
    }
    return NULL;
}

static void render_color(void)
{
    for (int i = 0; i < N_COLORS; i++) {
        bool sel = s_f.draft.color == i;
        lv_obj_set_style_outline_width(s_dot[i], sel ? 4 : 0, 0);
        const hy_med_t *u = color_user((uint8_t)i);
        lv_label_set_text(s_dot_lbl[i], u ? u->code : "");
        lv_obj_center(s_dot_lbl[i]);
    }
    lv_label_set_text(s_color_prev, s_f.draft.code);
    lv_obj_set_style_text_color(s_color_prev, hy_med_color(s_f.draft.color), 0);
}

static void color_cb(void *ud)
{
    if (!form_open() || s_page != PG_COLOR) {
        return;
    }
    hy_mf_set_color(&s_f, (uint8_t)(uintptr_t)ud);
    show_page(PG_COLOR);
}

/* ---- Type page ---- */

static void render_type(void)
{
    for (int i = 0; i < 2; i++) {
        bool cur = (int)s_f.draft.type == i;
        lv_obj_set_style_outline_width(s_opt[i], cur ? 3 : 0, 0);
    }
}

static void type_cb(void *ud)
{
    if (!form_open() || s_page != PG_TYPE) {
        return;
    }
    hy_mf_set_type(&s_f, (hy_sched_type_t)(uintptr_t)ud);
    go_step(sched_step());   /* from Review too: the schedule page of the chosen type */
}

/* ---- Times page and Add time ---- */

static void render_times(void)
{
    char b[96];
    if (s_f.draft.weekdays == 0x7F) {
        snprintf(b, sizeof b, HY_S_MF_EVERY_DAY);
    } else {
        static const char *const days[7] = HY_S_MF_DAY_NAMES;
        size_t o = 0;
        b[0] = 0;
        for (int d = 0; d < 7; d++) {
            if (s_f.draft.weekdays & (1u << d)) {
                o += (size_t)snprintf(b + o, sizeof b - o, "%s%s", o ? HY_S_DET_SEP : "", days[d]);
            }
        }
        snprintf(b + o, sizeof b - o, HY_S_MF_WEB_SET);
    }
    lv_label_set_text(s_days, b);
    for (int i = 0; i < HY_MAX_TIMES; i++) {
        bool on = i < s_f.draft.n_times;
        lv_obj_set_hidden(s_rows[i].row, !on);
        if (on) {
            fmt_hhmm(s_f.draft.times[i], b, sizeof b);
            lv_label_set_text(s_rows[i].time, b);
            lv_obj_set_hidden(s_rows[i].rm, s_f.draft.n_times <= 1);
        }
    }
    lv_obj_set_hidden(s_add_btn, s_f.draft.n_times >= HY_MAX_TIMES);
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
}

static void remove_time_cb(void *ud)
{
    if (!form_open() || s_page != PG_TIMES) {
        return;
    }
    hy_err_t e = hy_mf_remove_time(&s_f, (size_t)(uintptr_t)ud);
    show_page(PG_TIMES);
    if (e != HY_OK) {
        log_err(e, HY_MF_TIMES);
        set_err(HY_S_MF_ERR_TIMES);
    }
}

static void add_time_open_cb(void *ud)
{
    (void)ud;
    if (form_open() && s_page == PG_TIMES) {
        show_page(PG_ADD_TIME);
    }
}

static uint16_t add_sel(void)
{
    return (uint16_t)(lv_roller_get_selected(s_hour) * 60 + lv_roller_get_selected(s_min) * 5);
}

static void add_label_update(void)
{
    char t[8], b[24];
    fmt_hhmm(add_sel(), t, sizeof t);
    snprintf(b, sizeof b, HY_S_MF_ADD_AT, t);
    lv_label_set_text(s_next_l, b);   /* the Add time page labels the Next button "Add 08:00" */
}

static void roller_changed_cb(lv_event_t *e)
{
    (void)e;
    add_label_update();
}

static void render_add_time(void)
{
    uint16_t d = hy_mf_next_time_default(&s_f);
    lv_roller_set_selected(s_hour, d / 60, LV_ANIM_OFF);
    lv_roller_set_selected(s_min, (d % 60) / 5, LV_ANIM_OFF);
    add_label_update();
}

static void add_time_commit(void)
{
    uint16_t t = add_sel();
    hy_err_t e = hy_mf_add_time(&s_f, t);
    if (e != HY_OK) {
        char hhmm[8], b[48];
        fmt_hhmm(t, hhmm, sizeof hhmm);
        log_err(e, HY_MF_TIMES);
        snprintf(b, sizeof b, HY_S_MF_ERR_TIME_DUP, hhmm);
        set_err(b);
        return;
    }
    show_page(PG_TIMES);
}

/* ---- Interval page ---- */

static void interval_apply(void)
{
    uint32_t sel = lv_roller_get_selected(s_iroller);
    if (sel < s_n_iopts) {
        hy_mf_set_interval(&s_f, s_iopts[sel]);
        ui_nav_set_form_dirty(s_f.dirty);
    }
}

static void interval_changed_cb(lv_event_t *e)
{
    (void)e;
    if (form_open() && s_page == PG_INTERVAL) {
        interval_apply();
    }
}

static void render_interval(void)
{
    char opts[N_OPTS * 16], lbl[24];
    s_n_iopts = hy_mf_interval_options(s_f.draft.min_interval_min, s_iopts, N_OPTS);
    size_t o = 0, cur = 0;
    opts[0] = 0;
    for (size_t i = 0; i < s_n_iopts; i++) {
        fmt_interval(s_iopts[i], lbl, sizeof lbl);
        o += (size_t)snprintf(opts + o, sizeof opts - o, "%s%s", i ? "\n" : "", lbl);
        if (s_iopts[i] == s_f.draft.min_interval_min) {
            cur = i;
        }
    }
    lv_roller_set_options(s_iroller, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(s_iroller, (uint32_t)cur, LV_ANIM_OFF);
}

/* ---- Review page ---- */

static void schedule_value(char *out, size_t len)
{
    if (s_f.draft.type == HY_SCHED_AS_NEEDED) {
        if (s_f.draft.min_interval_min) {
            char d[24];
            fmt_interval(s_f.draft.min_interval_min, d, sizeof d);
            snprintf(out, len, HY_S_MF_ASNEEDED HY_S_MF_AT_LEAST, d);
        } else {
            snprintf(out, len, HY_S_MF_ASNEEDED);
        }
        return;
    }
    size_t o = 0;
    out[0] = 0;
    for (uint8_t i = 0; i < s_f.draft.n_times && i < HY_MAX_TIMES && o + 8 < len; i++) {
        char t[8];
        fmt_hhmm(s_f.draft.times[i], t, sizeof t);
        o += (size_t)snprintf(out + o, len - o, "%s%s", i ? HY_S_DET_SEP : "", t);
    }
}

/* "08:00 today will be due right away" when saving would make a dose due now */
static void due_note(void)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    hy_med_t built;
    hy_mf_step_t bad;
    const hy_clock_port_t *ck = hy_clock_port();
    if (hy_mf_build(&s_f, ms, n, &built, &bad) != HY_OK || !ck->valid(NULL)) {
        return;
    }
    char tz[HY_TZ_LEN];
    hy_clock_tz_copy(tz, sizeof tz);
    int64_t slots[HY_MAX_TIMES];
    size_t c = hy_mf_due_on_save(&built, s_f.edit_id ? ui_find_med(s_f.edit_id) : NULL, ck->now(NULL), tz, slots,
                                 HY_MAX_TIMES);
    if (c > 0) {
        char hhmm[8], b[64];
        ui_fmt_hhmm(slots[0], tz, hhmm, sizeof hhmm);
        snprintf(b, sizeof b, HY_S_MF_DUE_NOW, hhmm);
        set_err(b);
    }
}

static void render_review(void)
{
    char b[96];
    bool edit = s_f.edit_id != 0;
    snprintf(b, sizeof b, HY_S_MF_REVIEW_EDIT, s_f.draft.name);
    set_title(edit ? b : HY_S_MF_REVIEW_NEW, false);

    lv_label_set_text(s_rv[0].val, s_f.draft.name);
    lv_obj_set_style_text_color(s_rv[0].val, lv_color_hex(HY_C_TEXT), 0);
    bool nodose = !s_f.draft.dose[0];
    lv_label_set_text(s_rv[1].val, nodose ? HY_S_MF_DOSE_NONE : s_f.draft.dose);
    lv_obj_set_style_text_color(s_rv[1].val, lv_color_hex(nodose ? HY_C_TEXT2 : HY_C_TEXT), 0);
    lv_label_set_text(s_rv[2].val, s_f.draft.code);
    lv_obj_set_style_text_color(s_rv[2].val, hy_med_color(s_f.draft.color), 0);
    schedule_value(b, sizeof b);
    lv_label_set_text(s_rv[3].val, b);
    lv_obj_set_style_text_color(s_rv[3].val, lv_color_hex(HY_C_TEXT), 0);

    lv_label_set_text(s_back_l, edit ? HY_S_CANCEL : HY_S_BACK);
    lv_label_set_text(s_next_l, HY_S_MF_SAVE);
    due_note();
}

static void review_row_cb(void *ud)
{
    if (!form_open() || s_page != PG_REVIEW) {
        return;
    }
    hy_mf_step_t step = (hy_mf_step_t)(uintptr_t)ud;
    open_from_review(step);
}

/* The Review rows open their page; Schedule opens the type page, whose choice goes on to the times or the interval */
static const hy_mf_step_t k_row_step[4] = {HY_MF_NAME, HY_MF_DOSE, HY_MF_CODE, HY_MF_TYPE};

/* ---- saving ---- */

static void save_failed(hy_err_t e, hy_mf_step_t bad)
{
    log_err(e, bad);
    if (e == HY_E_NOT_FOUND) {
        toast_show_info(HY_S_MF_ERR_GONE);
        close_form();
        return;
    }
    char b[96];
    const char *txt;
    if (e == HY_E_FULL) {
        txt = HY_S_MF_ERR_FULL;
    } else if (e == HY_E_IO) {
        txt = HY_S_MF_ERR_IO;
    } else if (bad == HY_MF_REVIEW) {
        txt = HY_S_MF_ERR_WEB;   /* the error comes from a field the device cannot change */
    } else {
        const hy_med_t *ms;
        size_t n = hy_meds(&ms);
        uint32_t dup = 0;
        if (e == HY_E_CODE_DUP) {
            hy_mf_check_code(s_f.draft.code, ms, n, s_f.edit_id, &dup);
        }
        txt = err_text(e, s_f.draft.code, dup, b, sizeof b);
    }
    if (bad == HY_MF_REVIEW || e == HY_E_FULL || e == HY_E_IO) {
        show_page(PG_REVIEW);
    } else {
        s_f.from_review = true;   /* fix it, then Next returns to Review */
        s_via_code_row = false;
        go_step(bad);
    }
    set_err(txt);
}

static void do_save(void)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    hy_med_t m;
    hy_mf_step_t bad = HY_MF_REVIEW;
    hy_err_t e = hy_mf_build(&s_f, ms, n, &m, &bad);
    if (e != HY_OK) {
        save_failed(e, bad);
        return;
    }
    bool is_new = s_f.edit_id == 0;
    e = hy_act_med_upsert(&m);
    if (e != HY_OK) {
        save_failed(e, HY_MF_REVIEW);
        return;
    }
    char code[3], msg[48];
    snprintf(code, sizeof code, "%s", m.code);
    ESP_LOGI(TAG, "medform: saved id %u (%s)", (unsigned)m.id, code);
    ui_log_hwm("medform save");
    if (is_new) {
        ui_nav_home();
        home_show_med(m.id);
        snprintf(msg, sizeof msg, HY_S_MF_ADDED, code);
        toast_show_info(msg);
    } else {
        ui_nav_pop();   /* back to the detail */
        toast_show_info(HY_S_MF_SAVED);
    }
}

/* ---- Next and Back ---- */

static void do_next(void)
{
    if (!form_open()) {
        return;
    }
    hy_err_t e = HY_OK;
    uint32_t dup = 0;
    char b[96];
    switch (s_page) {
    case PG_NAME:
        e = hy_mf_set_name(&s_f, lv_textarea_get_text(s_ta));
        break;
    case PG_DOSE:
        e = hy_mf_set_dose(&s_f, lv_textarea_get_text(s_ta));
        break;
    case PG_CODE: {
        const hy_med_t *ms;
        size_t n = hy_meds(&ms);
        e = hy_mf_check_code(s_f.draft.code, ms, n, s_f.edit_id, &dup);
        break;
    }
    case PG_INTERVAL:
        interval_apply();
        break;
    case PG_ADD_TIME:
        add_time_commit();
        return;
    case PG_REVIEW:
        do_save();
        return;
    default:
        break;
    }
    if (e != HY_OK) {
        log_err(e, s_f.step);
        ui_nav_set_form_dirty(s_f.dirty);
        set_err(err_text(e, s_f.draft.code, dup, b, sizeof b));
        return;
    }
    if (s_page == PG_CODE && s_f.from_review && s_via_code_row) {
        go_step(HY_MF_COLOR);   /* the Code and colour row: on to the colour, then Review */
        return;
    }
    go_step(hy_mf_next(&s_f));
}

static void do_back(void)
{
    if (!form_open()) {
        return;
    }
    switch (s_page) {
    case PG_PICK:
        show_page(PG_CODE);   /* the typed code is dropped */
        return;
    case PG_ADD_TIME:
        show_page(PG_TIMES);
        return;
    case PG_DISCARD:
        show_page(s_discard_from);
        return;
    default:
        break;
    }
    if (on_text_page()) {
        text_load();   /* the draft holds the last accepted value: the field shows it again, so Review never differs from it */
    }
    if (s_f.from_review) {
        go_step(s_f.step == HY_MF_COLOR && s_via_code_row ? HY_MF_CODE : HY_MF_REVIEW);
        return;
    }
    if (s_f.step == first_step()) {
        leave();
        return;
    }
    go_step(hy_mf_prev(&s_f));
}

static void next_cb(void *ud)
{
    (void)ud;
    do_next();
}

static void back_cb(void *ud)
{
    (void)ud;
    do_back();
}

static void keep_cb(void *ud)
{
    (void)ud;
    if (form_open() && s_page == PG_DISCARD) {
        show_page(s_discard_from);
    }
}

static void discard_cb(void *ud)
{
    (void)ud;
    if (form_open() && s_page == PG_DISCARD) {
        ESP_LOGI(TAG, "medform: discarded");
        close_form();
    }
}

/* ---- showing a page ---- */

static void set_title(const char *t, bool step)
{
    lv_obj_set_hidden(s_title, t == NULL);
    if (!t) {
        lv_obj_set_hidden(s_step, true);
        return;
    }
    lv_obj_set_size(s_title, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_label_set_text(s_title, t);
    lv_obj_update_layout(s_title);
    int32_t tw = lv_obj_get_width(s_title);
    if (tw > TITLE_MAX_W) {
        lv_obj_set_size(s_title, TITLE_MAX_W, lv_font_get_line_height(hy_type.title));
        tw = TITLE_MAX_W;
    }
    lv_obj_set_hidden(s_step, !step);
    if (step) {
        char b[24];
        snprintf(b, sizeof b, HY_S_MF_STEP, hy_mf_step_number(&s_f), HY_MF_STEPS);
        lv_label_set_text(s_step, b);
        lv_obj_update_layout(s_step);
        int32_t sw = lv_obj_get_width(s_step);
        /* a long title (the interval page) would run into the counter: it drops below the title line */
        lv_obj_set_pos(s_step, HY_SCREEN - 24 - sw, 24 + tw + 12 > HY_SCREEN - 24 - sw ? 33 : 8);
    }
}

static void bottom_row(const char *back, const char *next, bool show_back, bool show_next)
{
    lv_obj_set_size(s_back, BACK_W, 66);
    lv_obj_set_pos(s_back, PAD_X, BTN_Y);
    lv_obj_set_size(s_next, NEXT_W, 66);
    lv_obj_set_pos(s_next, NEXT_X, BTN_Y);
    lv_label_set_text(s_back_l, back);
    lv_label_set_text(s_next_l, next);
    lv_obj_set_hidden(s_back, !show_back);
    lv_obj_set_hidden(s_next, !show_next);
}

static void show_page(page_t pg)
{
    s_page = pg;
    for (int i = 0; i < PG_N; i++) {
        if (s_c[i]) {
            lv_obj_set_hidden(s_c[i], s_c[i] != s_c[pg]);
        }
    }
    set_err(NULL);
    lv_obj_set_pos(s_err, PAD_X, 300);

    switch (pg) {
    case PG_NAME:
    case PG_DOSE:
        set_title(pg == PG_NAME ? HY_S_MF_NAME : HY_S_MF_DOSE, true);
        bottom_row(HY_S_BACK, HY_S_NEXT, true, true);
        render_text();
        break;
    case PG_CODE:
        set_title(HY_S_MF_CODE, true);
        bottom_row(HY_S_BACK, HY_S_NEXT, true, true);
        render_code();
        break;
    case PG_PICK:
        set_title(NULL, false);
        bottom_row(HY_S_BACK, HY_S_NEXT, false, false);
        render_pick();
        break;
    case PG_COLOR:
        set_title(HY_S_MF_COLOR, true);
        bottom_row(HY_S_BACK, HY_S_NEXT, true, true);
        render_color();
        break;
    case PG_TYPE:
        set_title(HY_S_MF_TYPE, true);
        bottom_row(HY_S_BACK, HY_S_NEXT, true, false);
        render_type();
        break;
    case PG_TIMES:
        set_title(HY_S_MF_TIMES, true);
        bottom_row(HY_S_BACK, HY_S_NEXT, true, true);
        render_times();
        break;
    case PG_ADD_TIME:
        set_title(HY_S_MF_ADD_TIME, false);
        bottom_row(HY_S_CANCEL, "", true, true);
        render_add_time();
        break;
    case PG_INTERVAL:
        set_title(HY_S_MF_INTERVAL, true);
        bottom_row(HY_S_BACK, HY_S_NEXT, true, true);
        render_interval();
        break;
    case PG_REVIEW:
        bottom_row(HY_S_BACK, HY_S_MF_SAVE, true, true);
        lv_obj_set_pos(s_err, PAD_X, 314);
        render_review();
        break;
    case PG_DISCARD:
        set_title(HY_S_MF_DISCARD_TITLE, false);
        bottom_row(HY_S_BACK, HY_S_NEXT, false, false);
        break;
    default:
        break;
    }
    ui_nav_set_form_dirty(s_f.dirty);
}

/* ---- public ---- */

void medform_show(void)
{
    if (!s_root) {
        return;
    }
    /* After the quick settings closed, the picker, Add time and Times pages keep what the person had entered */
    if (!s_fresh && (s_page == PG_PICK || s_page == PG_ADD_TIME || s_page == PG_TIMES)) {
        ui_nav_set_form_dirty(s_f.dirty);
        return;
    }
    s_fresh = false;
    show_page(s_page);
}

void medform_back(void)
{
    do_back();
}

void medform_open_new(void)
{
    if (hy_meds_active_count() >= HY_MAX_MEDS) {
        toast_show_info(HY_S_MF_ERR_FULL);
        return;
    }
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    hy_mf_new(&s_f, ms, n);
    s_page = PG_NAME;
    s_via_code_row = false;
    s_fresh = true;
    ui_nav_set_form_dirty(false);
    ESP_LOGI(TAG, "medform: new");
    ui_nav_push(HY_V_MEDFORM);
}

void medform_open_edit(uint32_t med_id)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    hy_err_t e = hy_mf_edit(&s_f, ms, n, med_id);
    if (e != HY_OK) {
        log_err(e, HY_MF_REVIEW);
        toast_show_info(ui_err_text(e));
        return;
    }
    s_page = PG_REVIEW;
    s_via_code_row = false;
    s_fresh = true;
    ui_nav_set_form_dirty(false);
    ESP_LOGI(TAG, "medform: edit id %u", (unsigned)med_id);
    ui_nav_push(HY_V_MEDFORM);
}

/* ---- debug ---- */

#if CONFIG_HY_DEBUG_CONSOLE
static void dbg_state(void)
{
    char t[96], iv[24];
    size_t o = 0;
    t[0] = 0;
    for (uint8_t i = 0; i < s_f.draft.n_times && i < HY_MAX_TIMES && o + 8 < sizeof t; i++) {
        o += (size_t)snprintf(t + o, sizeof t - o, "%s%02u:%02u", i ? "," : "", (unsigned)(s_f.draft.times[i] / 60),
                              (unsigned)(s_f.draft.times[i] % 60));
    }
    fmt_interval(s_f.draft.min_interval_min, iv, sizeof iv);
    ESP_LOGI(TAG,
             "medform: state step %s page %d dirty %d from_review %d edit %u name \"%s\" dose \"%s\" code \"%s\" "
             "color %u type %s times [%s] interval %s",
             step_name(s_f.step), (int)s_page, s_f.dirty, s_f.from_review, (unsigned)s_f.edit_id, s_f.draft.name,
             s_f.draft.dose, s_f.draft.code, (unsigned)s_f.draft.color,
             s_f.draft.type == HY_SCHED_FIXED ? "fixed" : "asneeded", t, iv);
}

/* A setter driven from the console, as the widget would: the result is logged, the page redrawn */
static void dbg_done(hy_err_t e, hy_mf_step_t at, uint32_t dup)
{
    char b[96];
    show_page(s_page);
    if (e != HY_OK) {
        log_err(e, at);
        set_err(err_text(e, s_f.draft.code, dup, b, sizeof b));
    }
}

void medform_debug(const char *arg)
{
    char cmd[12] = "";
    int used = 0;
    sscanf(arg, "%11s%n", cmd, &used);
    const char *rest = arg + used;
    while (*rest == ' ') {
        rest++;
    }
    if (strcmp(cmd, "new") == 0) {
        ui_nav_reset();
        medform_open_new();
        return;
    }
    if (strcmp(cmd, "edit") == 0) {
        uint32_t id;
        char code[4] = "";
        sscanf(rest, "%3s", code);
        if (!home_med_id_by_code(code, &id)) {
            ESP_LOGW(TAG, "mf edit: no active medicine with code \"%s\"", code);
            return;
        }
        ui_nav_reset();
        medform_open_edit(id);
        return;
    }
    if (strcmp(cmd, "state") == 0) {
        dbg_state();
        return;
    }
    if (!form_open()) {
        ESP_LOGW(TAG, "mf %s: the medicine form is not showing", cmd);
        return;
    }
    if (strcmp(cmd, "next") == 0) {
        do_next();
    } else if (strcmp(cmd, "back") == 0) {
        do_back();
    } else if (strcmp(cmd, "save") == 0) {
        do_save();
    } else if (strcmp(cmd, "name") == 0) {
        dbg_done(hy_mf_set_name(&s_f, rest), HY_MF_NAME, 0);
    } else if (strcmp(cmd, "dose") == 0) {
        dbg_done(hy_mf_set_dose(&s_f, rest), HY_MF_DOSE, 0);
    } else if (strcmp(cmd, "code") == 0) {
        snprintf(s_pick, sizeof s_pick, "%s", rest);   /* the picker's Done with this code */
        for (char *c = s_pick; *c; c++) {
            if (*c >= 'a' && *c <= 'z') {
                *c = (char)(*c - 'a' + 'A');
            }
        }
        pick_commit();
    } else if (strcmp(cmd, "color") == 0) {
        dbg_done(hy_mf_set_color(&s_f, (uint8_t)atoi(rest)), HY_MF_COLOR, 0);
    } else if (strcmp(cmd, "type") == 0) {
        hy_mf_set_type(&s_f, strcmp(rest, "asneeded") == 0 ? HY_SCHED_AS_NEEDED : HY_SCHED_FIXED);
        dbg_done(HY_OK, HY_MF_TYPE, 0);
    } else if (strcmp(cmd, "time") == 0) {
        int h, m, idx;
        if (strncmp(rest, "add ", 4) == 0 && sscanf(rest + 4, "%d:%d", &h, &m) == 2) {
            dbg_done(hy_mf_add_time(&s_f, (uint16_t)(h * 60 + m)), HY_MF_TIMES, 0);
        } else if (strncmp(rest, "del ", 4) == 0 && sscanf(rest + 4, "%d", &idx) == 1) {
            dbg_done(hy_mf_remove_time(&s_f, (size_t)idx), HY_MF_TIMES, 0);
        } else {
            ESP_LOGE(TAG, "usage: mf time add HH:MM | mf time del <idx>");
        }
    } else if (strcmp(cmd, "interval") == 0) {
        dbg_done(hy_mf_set_interval(&s_f, (uint16_t)atoi(rest)), HY_MF_INTERVAL, 0);
    } else {
        ESP_LOGE(TAG, "usage: mf new|edit <code>|name <text>|dose [text]|code <xy>|color <0-9>|type fixed|asneeded|"
                      "time add HH:MM|time del <idx>|interval <min>|next|back|save|state");
    }
}
#endif

/* ---- creation ---- */

static void style_ta(lv_obj_t *ta)
{
    lv_obj_set_style_text_font(ta, hy_type.body, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(HY_C_TEXT), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(HY_C_TEXT2), LV_PART_TEXTAREA_PLACEHOLDER);
    style_surface(ta, 22);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_shadow_width(ta, 0, 0);
    lv_obj_set_style_outline_width(ta, 0, 0);
    lv_obj_set_style_pad_hor(ta, 18, 0);
    lv_obj_set_style_pad_ver(ta, 14, 0);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_border_color(ta, lv_color_hex(HY_C_TEXT), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
}

static void create_text(void)
{
    lv_obj_t *c = s_c[PG_NAME] = s_c[PG_DOSE] = new_page();
    s_ta = lv_textarea_create(c);
    lv_obj_set_size(s_ta, FULL_W, 56);
    lv_obj_set_pos(s_ta, PAD_X, 48);
    lv_textarea_set_one_line(s_ta, true);
    style_ta(s_ta);
    lv_obj_add_event_cb(s_ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_ta, consume_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ta, consume_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_ta, ta_ready_cb, LV_EVENT_READY, NULL);
    s_kb = ui_kbd_create(c, s_ta);
}

static void create_code(void)
{
    lv_obj_t *c = s_c[PG_CODE] = new_page();
    s_ring = ui_plain(c);
    lv_obj_set_size(s_ring, 150, 150);
    lv_obj_set_pos(s_ring, 165, 44);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ring, lv_color_hex(HY_C_INNER), 0);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ring, 11, 0);
    s_code_lbl = ui_label(s_ring, hy_type.code_l, HY_C_TEXT, "");
    lv_obj_t *hint = ui_label(c, hy_type.status, HY_C_TEXT2, HY_S_MF_CODE_HINT);
    lv_obj_set_width(hint, HY_SCREEN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 0, 204);
    lv_obj_t *chg = ui_button(c, FULL_W, 66, hy_type.body_bold, HY_S_MF_CODE_CHANGE, change_code_cb, NULL);
    lv_obj_set_pos(chg, PAD_X, 232);
}

static const char *const k_pick_map[] = {
    "A", "B", "C", "D", "E", "F", "G", "\n",
    "H", "I", "J", "K", "L", "M", "N", "\n",
    "O", "P", "Q", "R", "S", "T", "U", "\n",
    "V", "W", "X", "Y", "Z", "0", "1", "\n",
    "2", "3", "4", "5", "6", "7", "8", "\n",
    "9", LV_SYMBOL_BACKSPACE, " ", " ", " ", " ", ""};
#define PK_KEY (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT | 1)
#define PK_GAP (LV_BUTTONMATRIX_CTRL_HIDDEN | LV_BUTTONMATRIX_CTRL_DISABLED | 1)
#define PK_R   PK_KEY, PK_KEY, PK_KEY, PK_KEY, PK_KEY, PK_KEY, PK_KEY
static const lv_buttonmatrix_ctrl_t k_pick_ctrl[] = {
    PK_R, PK_R, PK_R, PK_R, PK_R,
    PK_KEY, (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT | 2), PK_GAP, PK_GAP, PK_GAP, PK_GAP};

static void create_pick(void)
{
    lv_obj_t *c = s_c[PG_PICK] = new_page();
    s_pick_prev = ui_label(c, hy_type.code_m, HY_C_TEXT, "");
    lv_obj_set_pos(s_pick_prev, 24, 6);
    lv_obj_t *done = ui_button(c, 140, 56, hy_type.body_bold, HY_S_DONE, pick_done_cb, NULL);
    lv_obj_set_pos(done, 322, 4);

    s_pick_font = *hy_type.title;
    s_pick_font.fallback = &lv_font_montserrat_14;
    s_pick_grid = lv_buttonmatrix_create(c);
    lv_buttonmatrix_set_map(s_pick_grid, k_pick_map);
    lv_buttonmatrix_set_ctrl_map(s_pick_grid, k_pick_ctrl);
    lv_obj_set_size(s_pick_grid, 456, 342);
    lv_obj_set_pos(s_pick_grid, 12, 72);
    lv_obj_set_style_bg_opa(s_pick_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pick_grid, 0, 0);
    lv_obj_set_style_pad_all(s_pick_grid, 0, 0);
    lv_obj_set_style_pad_gap(s_pick_grid, 6, 0);
    lv_obj_set_style_text_font(s_pick_grid, &s_pick_font, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_pick_grid, lv_color_hex(HY_C_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_pick_grid, lv_color_hex(HY_C_SURFACE2), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_pick_grid, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_pick_grid, 12, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_pick_grid, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_pick_grid, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_pick_grid, lv_color_hex(HY_C_GRAB), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_send_draw_task_events(s_pick_grid, true);
    lv_obj_add_event_cb(s_pick_grid, pick_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_event_cb(s_pick_grid, pick_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void create_color(void)
{
    lv_obj_t *c = s_c[PG_COLOR] = new_page();
    for (int i = 0; i < N_COLORS; i++) {
        lv_obj_t *d = ui_plain(c);
        lv_obj_set_size(d, 72, 72);
        lv_obj_set_pos(d, 24 + (i % 5) * 90, i < 5 ? 60 : 160);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, hy_med_color((uint8_t)i), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_color(d, lv_color_hex(HY_C_TEXT), 0);
        lv_obj_set_style_outline_pad(d, 4, 0);
        lv_obj_set_style_outline_width(d, 0, 0);
        ui_on_tap(d, color_cb, (void *)(uintptr_t)i);
        s_dot[i] = d;
        s_dot_lbl[i] = ui_label(d, hy_type.status_bold, 0x000000, "");
    }
    s_color_prev = ui_label(c, hy_type.code, HY_C_TEXT, "");
    lv_obj_set_width(s_color_prev, HY_SCREEN);
    lv_obj_set_style_text_align(s_color_prev, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_color_prev, 0, 254);
}

static lv_obj_t *option_button(lv_obj_t *c, int y, const char *title, const char *sub, hy_sched_type_t t)
{
    lv_obj_t *b = ui_plain(c);
    lv_obj_set_size(b, FULL_W, 112);
    lv_obj_set_pos(b, PAD_X, y);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(HY_C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(HY_C_GRAB), LV_STATE_PRESSED);
    lv_obj_set_style_outline_color(b, lv_color_hex(HY_C_TEXT), 0);
    lv_obj_set_style_outline_width(b, 0, 0);
    ui_on_tap(b, type_cb, (void *)(uintptr_t)t);
    lv_obj_set_pos(ui_label(b, hy_type.body_bold, HY_C_TEXT, title), 22, 24);
    lv_obj_t *s = ui_label(b, hy_type.status, HY_C_TEXT2, sub);
    lv_obj_set_width(s, FULL_W - 44);
    lv_obj_set_pos(s, 22, 62);
    return b;
}

static void create_type(void)
{
    lv_obj_t *c = s_c[PG_TYPE] = new_page();
    s_opt[HY_SCHED_FIXED] = option_button(c, 52, HY_S_MF_FIXED, HY_S_MF_FIXED_SUB, HY_SCHED_FIXED);
    s_opt[HY_SCHED_AS_NEEDED] = option_button(c, 180, HY_S_MF_ASNEEDED, HY_S_MF_ASNEEDED_SUB, HY_SCHED_AS_NEEDED);
}

static void create_times(void)
{
    lv_obj_t *c = s_c[PG_TIMES] = new_page();
    s_days = ui_label(c, hy_type.status, HY_C_TEXT2, "");
    lv_obj_set_width(s_days, HY_SCREEN - 48);
    lv_label_set_long_mode(s_days, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_days, lv_font_get_line_height(hy_type.status));
    lv_obj_set_pos(s_days, 24, 38);

    s_list = lv_obj_create(c);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, FULL_W, 222);
    lv_obj_set_pos(s_list, PAD_X, 68);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_elastic(s_list, false);
    lv_obj_set_scroll_momentum(s_list, false);
    lv_obj_add_event_cb(s_list, consume_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_list, consume_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    for (int i = 0; i < HY_MAX_TIMES; i++) {
        lv_obj_t *r = ui_plain(s_list);
        lv_obj_set_size(r, FULL_W, 60);
        style_surface(r, 18);
        s_rows[i].row = r;
        s_rows[i].time = ui_label(r, hy_type.title, HY_C_TEXT, "");
        lv_obj_align(s_rows[i].time, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_t *rm = ui_button(r, 128, 48, hy_type.body, HY_S_MF_REMOVE, remove_time_cb, (void *)(uintptr_t)i);
        lv_obj_align(rm, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_ext_click_area(rm, 6);   /* 48 px drawn, 60 px to hit */
        s_rows[i].rm = rm;
    }
    s_add_btn = ui_button(s_list, FULL_W, 60, hy_type.body_bold, HY_S_MF_ADD_TIME, add_time_open_cb, NULL);
}

static void create_add_time(void)
{
    lv_obj_t *c = s_c[PG_ADD_TIME] = new_page();
    char hours[24 * 3 + 1], mins[12 * 3 + 1];
    size_t o = 0;
    for (int h = 0; h < 24; h++) {
        o += (size_t)snprintf(hours + o, sizeof hours - o, "%s%02d", h ? "\n" : "", h);
    }
    o = 0;
    for (int m = 0; m < 60; m += 5) {
        o += (size_t)snprintf(mins + o, sizeof mins - o, "%s%02d", m ? "\n" : "", m);
    }
    s_hour = ui_roller(c, hours, LV_ROLLER_MODE_INFINITE, 120, hy_type.time, roller_changed_cb);
    lv_obj_set_pos(s_hour, 108, 92);
    s_min = ui_roller(c, mins, LV_ROLLER_MODE_INFINITE, 120, hy_type.time, roller_changed_cb);
    lv_obj_set_pos(s_min, 252, 92);
    lv_obj_update_layout(s_hour);
    lv_obj_t *colon = ui_label(c, hy_type.time, HY_C_TEXT, HY_S_COLON);
    lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, 92 + lv_obj_get_height(s_hour) / 2 - lv_font_get_line_height(hy_type.time) / 2);
}

static void create_interval(void)
{
    lv_obj_t *c = s_c[PG_INTERVAL] = new_page();
    s_iroller = ui_roller(c, "0", LV_ROLLER_MODE_NORMAL, 300, hy_type.time, interval_changed_cb);
    lv_obj_set_pos(s_iroller, 90, 52);
    lv_obj_t *sub = ui_label(c, hy_type.status, HY_C_TEXT2, HY_S_MF_INTERVAL_SUB);
    lv_obj_set_width(sub, FULL_W);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sub, PAD_X, 290);
}

static void create_review(void)
{
    static const char *const labels[4] = {HY_S_MF_ROW_NAME, HY_S_MF_ROW_DOSE, HY_S_MF_ROW_CODE, HY_S_MF_ROW_SCHED};
    lv_obj_t *c = s_c[PG_REVIEW] = new_page();
    for (int i = 0; i < 4; i++) {
        lv_obj_t *r = ui_plain(c);
        lv_obj_set_size(r, FULL_W, 56);
        lv_obj_set_pos(r, PAD_X, 44 + i * 62);
        style_surface(r, 18);
        lv_obj_set_style_bg_color(r, lv_color_hex(HY_C_SURFACE2), LV_STATE_PRESSED);
        ui_on_tap(r, review_row_cb, (void *)(uintptr_t)k_row_step[i]);
        lv_obj_align(ui_label(r, hy_type.status, HY_C_TEXT2, labels[i]), LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_t *v = ui_label(r, hy_type.body, HY_C_TEXT, "");
        lv_obj_set_width(v, 300);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -20, 0);
        s_rv[i].val = v;
    }
    lv_obj_t *web = ui_label(c, hy_type.status, HY_C_TEXT2, HY_S_MF_WEB_ONLY);
    lv_obj_set_pos(web, 24, 292);
}

static void create_discard(void)
{
    lv_obj_t *c = s_c[PG_DISCARD] = new_page();
    lv_obj_t *keep = ui_button(c, FULL_W, 66, hy_type.body_bold, HY_S_MF_KEEP, keep_cb, NULL);
    lv_obj_set_pos(keep, PAD_X, 120);
    lv_obj_t *drop = ui_button(c, FULL_W, 66, hy_type.body_bold, HY_S_MF_DISCARD, discard_cb, NULL);
    lv_obj_set_pos(drop, PAD_X, 210);
    lv_obj_set_style_text_color(lv_obj_get_child(drop, 0), lv_color_hex(HY_C_MISS), 0);
}

void medform_create(void)
{
    s_root = ui_view_box();
    ui_register_view(HY_V_MEDFORM, s_root);

    create_text();
    create_code();
    create_pick();
    create_color();
    create_type();
    create_times();
    create_add_time();
    create_interval();
    create_review();
    create_discard();

    /* shared chrome, above every page */
    s_title = ui_view_title(s_root, "");
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    s_step = ui_label(s_root, hy_type.status, HY_C_TEXT2, "");
    s_err = ui_label(s_root, hy_type.status, HY_C_AMBER, "");
    lv_obj_set_width(s_err, FULL_W);
    s_back = ui_button(s_root, BACK_W, 66, hy_type.body_bold, HY_S_BACK, back_cb, NULL);
    s_back_l = lv_obj_get_child(s_back, 0);
    s_next = ui_button(s_root, NEXT_W, 66, hy_type.body_bold, HY_S_NEXT, next_cb, NULL);
    s_next_l = lv_obj_get_child(s_next, 0);
    lv_obj_set_pos(s_back, PAD_X, BTN_Y);
    lv_obj_set_pos(s_next, NEXT_X, BTN_Y);
}
