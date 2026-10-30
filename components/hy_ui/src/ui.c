#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "hy/battery.h"
#include "hy/events.h"
#include "hy/occ.h"
#include "hy/power_run.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "hy/ui.h"
#include "ui_internal.h"

static const char *TAG = "ui";

static lv_obj_t *s_screen, *s_content;
static bool s_refresh_queued, s_rebuild_queued;

static void async_refresh(void *arg)
{
    (void)arg;
    bool rebuild = s_rebuild_queued;
    s_refresh_queued = s_rebuild_queued = false;
    hy_sched_run_tick();   /* the event marked the runner dirty: evaluate before redrawing */
    if (rebuild) {
        home_rebuild();
    } else {
        home_refresh_if_changed();
    }
    overview_refresh_if_changed();
    history_refresh_if_changed();
    detail_refresh_if_changed();
    due_refresh();
    medarchive_refresh_if_shown();
    screen_sched_evaluated();
    topbar_refresh();   /* a zone change moves the clock */
}

/* Events arrive in the LVGL task (actions run there); defer the redraw out of the action's call stack. */
static void event_cb(const hy_event_t *ev, void *ctx)
{
    (void)ctx;
    if (ev->type == HY_EV_BATTERY_LOW || ev->type == HY_EV_BATTERY_CRITICAL) {
        return;
    }
    history_invalidate();
    if (ev->type == HY_EV_LOG_EDITED ||
        ((ev->type == HY_EV_TAKEN || ev->type == HY_EV_SKIPPED || ev->type == HY_EV_UNDONE) &&
         ev->ts && ev->ts < hy_clock_port()->now(NULL) - 8 * 86400)) {
        detail_invalidate();   /* an entry older than the scheduler's 8 day window changed */
    }
    if (ev->type == HY_EV_MEDS_CHANGED) {
        s_rebuild_queued = true;
    }
    if (!s_refresh_queued) {
        s_refresh_queued = true;
        lv_async_call(async_refresh, NULL);
    }
}

/* Power task callback (LVGL task, via lv_async_call): refresh the battery widgets; a low or critical crossing
 * publishes its event. Low also shows the info toast, on a dark screen too (it does not wake it and is gone
 * after its 5 s). Critical publishes its event on the first reading and shows no toast; the countdown overlay opens
 * on HY_BATT_CRIT_CONFIRMED (hy_batt_crit_confirm in the power task), not on the first reading. */
static void battery_cb(uint32_t flags)
{
    topbar_battery_refresh();
    if (flags & HY_BATT_CRIT_CONFIRMED) {
        poweroff_countdown_show();   /* two critical readings 10 s apart with a low voltage (R18) */
    }
    if (!(flags & (HY_BATT_EV_LOW | HY_BATT_EV_CRIT))) {
        return;
    }
    hy_power_state_t st;
    hy_power_state(&st);
    int pct = st.pmu_ok ? st.batt.pct : -1;   /* the same reading the widgets show */
    const hy_clock_port_t *ck = hy_clock_port();
    hy_event_t ev = {.ts = ck->valid(NULL) ? ck->now(NULL) : 0, .value = pct};
    if (flags & HY_BATT_EV_LOW) {
        ev.type = HY_EV_BATTERY_LOW;
        ESP_LOGI(TAG, "HY_EV_BATTERY_LOW value=%d", (int)ev.value);
        hy_ev_publish(&ev);
        ui_battery_toast(pct);
    }
    if (flags & HY_BATT_EV_CRIT) {
        ev.type = HY_EV_BATTERY_CRITICAL;
        ESP_LOGI(TAG, "HY_EV_BATTERY_CRITICAL value=%d", (int)ev.value);
        hy_ev_publish(&ev);
    }
}

/* Everything that follows the clock: the scheduler, the root flags, the top bar and every shown page */
void ui_refresh_all(void)
{
    hy_sched_run_tick();
    nav_host_tick();
    topbar_refresh();
    topbar_battery_refresh();
    home_refresh_if_changed();
    overview_refresh_if_changed();
    history_refresh_if_changed();
    detail_refresh_if_changed();
    due_refresh();
    medarchive_refresh_if_shown();
    screen_sched_evaluated();
}

/* A time or zone set publishes no hy event: recompute the views that depend on the day and the zone */
void ui_time_changed(void)
{
    hy_sched_run_invalidate();   /* a zone set bumps no sync seq: make the next tick evaluate at once */
    history_invalidate();
    detail_invalidate();
    ui_refresh_all();
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    ui_refresh_all();
    screen_tick();
}

/* hy_power_ui_t.usb: the countdown (only USB cancels it) and the screen */
static void power_usb(bool in)
{
    poweroff_usb(in);
    screen_usb(in);
}

/* hy_power_ui_t.wake: the power task left the sleep mode, or the user key */
static void power_wake(hy_power_wake_t cause)
{
    switch (cause) {
    case HY_PWAKE_TOUCH:
        screen_touch_wake();
        break;
    case HY_PWAKE_KEY:
        screen_event(HY_SE_USER_KEY);
        break;
    case HY_PWAKE_TICK:
        /* next_wake or the RTC alarm: the scheduler decides now (a due card or a chime on USB turns the screen ON;
         * otherwise it stays OFF and screen_tick asks for the sleep mode again) */
        ui_refresh_all();
        screen_tick();
        break;
    }
}

/* The first snapshot, as soon as the LVGL task runs; the tiles stay blank until then */
static void first_tick(void *arg)
{
    (void)arg;
    hy_sched_run_tick();
    home_refresh_if_changed();
    overview_refresh_if_changed();
    history_refresh_if_changed();
    due_refresh();
    medarchive_refresh_if_shown();
    screen_sched_evaluated();
}

lv_obj_t *ui_plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

uint32_t ui_fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

/* hy_store query_log keeps the earliest entries when it fills the buffer. The newest ones are wanted, so a full
 * buffer narrows the window from its old end until the answer fits. */
size_t ui_log_window(uint32_t med_id, int64_t from, int64_t to, hy_log_t *out, size_t cap)
{
    const hy_store_port_t *st = hy_store_port();
    for (;;) {
        size_t n = st->query_log(NULL, from, to, med_id, out, cap);
        if (n < cap || to - from <= 3600) {
            return n;
        }
        /* exactly cap entries: truly full only when something follows the last one (the last itself matches) */
        hy_log_t probe[2];
        if (st->query_log(NULL, out[n - 1].ts, to, med_id, probe, 2) < 2) {
            return n;
        }
        ESP_LOGW(TAG, "log window of med %u filled %u entries: keeping the newest half", (unsigned)med_id,
                 (unsigned)cap);
        from += (to - from) / 2;
    }
}

const hy_med_t *ui_find_med(uint32_t id)
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

/* ---- widget helpers shared by quick settings, settings and the set-the-time screen ---- */

typedef struct {
    ui_action_cb cb;
    void *ud;
} tap_ctx_t;

static void tap_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    tap_ctx_t *c = lv_event_get_user_data(e);
    if (code == LV_EVENT_DELETE) {
        lv_free(c);
        return;
    }
    if (!ui_input_last_was_tap()) {
        return;   /* a waking stroke or a swipe that ended here */
    }
    c->cb(c->ud);
}

void ui_on_tap(lv_obj_t *o, ui_action_cb cb, void *ud)
{
    tap_ctx_t *c = lv_malloc(sizeof *c);
    if (!c) {
        return;
    }
    c->cb = cb;
    c->ud = ud;
    lv_obj_set_clickable(o, true);
    lv_obj_add_event_cb(o, tap_event_cb, LV_EVENT_SHORT_CLICKED, c);
    lv_obj_add_event_cb(o, tap_event_cb, LV_EVENT_DELETE, c);
}

lv_obj_t *ui_button(lv_obj_t *parent, int32_t w, int32_t h, const lv_font_t *f, const char *text, ui_action_cb cb,
                    void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(HY_C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(HY_C_GRAB), LV_STATE_PRESSED);
    lv_obj_set_scrollable(b, false);
    lv_obj_set_scroll_chain(b, true);
    ui_on_tap(b, cb, ud);
    lv_obj_center(ui_label(b, f, HY_C_TEXT, text));
    return b;
}

lv_obj_t *ui_view_title(lv_obj_t *view, const char *text)
{
    lv_obj_t *t = ui_label(view, hy_type.title, HY_C_TEXT, text);
    lv_obj_set_pos(t, 24, 2);
    return t;
}

typedef struct {
    void (*live)(int32_t);
    void (*commit)(int32_t);
    int32_t start;
    bool ok;      /* the stroke began on an awake screen */
} slider_ctx_t;

static void slider_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    slider_ctx_t *c = lv_event_get_user_data(e);
    lv_obj_t *s = lv_event_get_target_obj(e);
    switch (code) {
    case LV_EVENT_DELETE:
        lv_free(c);
        break;
    case LV_EVENT_PRESSED:
        ui_input_consume_force();   /* the slider owns the stroke, also one that starts in an edge zone */
        c->ok = ui_input_still();   /* false when the stroke only wakes the dimmed screen */
        c->start = lv_slider_get_value(s);
        break;
    case LV_EVENT_PRESSING:
        ui_input_consume();
        break;
    case LV_EVENT_VALUE_CHANGED:
        if (c->ok) {
            c->live(lv_slider_get_value(s));
        } else {
            lv_slider_set_value(s, c->start, LV_ANIM_OFF);
        }
        break;
    case LV_EVENT_PRESS_LOST:
    case LV_EVENT_RELEASED:
        if (c->ok) {
            c->commit(lv_slider_get_value(s));
        } else {
            lv_slider_set_value(s, c->start, LV_ANIM_OFF);
        }
        c->ok = false;
        break;
    default:
        break;
    }
}

lv_obj_t *ui_slider(lv_obj_t *parent, int32_t w, int32_t min, int32_t max, void (*live)(int32_t),
                    void (*commit)(int32_t))
{
    lv_obj_t *s = lv_slider_create(parent);
    slider_ctx_t *c = lv_malloc(sizeof *c);
    if (!c) {
        return s;
    }
    c->live = live;
    c->commit = commit;
    c->ok = false;
    c->start = min;
    lv_slider_set_range(s, min, max);
    lv_obj_set_size(s, w, 30);
    lv_obj_set_style_radius(s, 15, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(HY_C_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 15, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(HY_C_FILL), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 0, LV_PART_KNOB);
    lv_obj_set_style_border_width(s, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
    lv_obj_set_style_outline_width(s, 0, 0);
    lv_obj_set_ext_click_area(s, 12);
    lv_obj_add_event_cb(s, slider_event_cb, LV_EVENT_ALL, c);
    return s;
}

typedef struct {
    lv_event_cb_t changed;
    uint32_t start;
    bool ok;      /* the stroke began on an awake screen; kept until the next press (the snap ends later) */
} roller_ctx_t;

static void roller_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    roller_ctx_t *c = lv_event_get_user_data(e);
    lv_obj_t *r = lv_event_get_target_obj(e);
    switch (code) {
    case LV_EVENT_DELETE:
        lv_free(c);
        break;
    case LV_EVENT_PRESSED:
        ui_input_consume();   /* turning a roller is the roller stroke */
        c->ok = !ui_input_waking();
        c->start = lv_roller_get_selected(r);
        break;
    case LV_EVENT_PRESSING:
    case LV_EVENT_SCROLL_BEGIN:
        ui_input_consume();
        break;
    case LV_EVENT_VALUE_CHANGED:
        if (!c->ok) {
            lv_roller_set_selected(r, c->start, LV_ANIM_OFF);   /* a waking stroke turns nothing */
        } else if (c->changed) {
            c->changed(e);
        }
        break;
    default:
        break;
    }
}

lv_obj_t *ui_roller(lv_obj_t *parent, const char *opts, lv_roller_mode_t mode, int32_t w, const lv_font_t *sel_font,
                    lv_event_cb_t changed)
{
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, mode);
    lv_roller_set_visible_row_count(r, 5);
    lv_obj_set_width(r, w);
    lv_obj_set_style_text_font(r, hy_type.body, 0);
    lv_obj_set_style_text_color(r, lv_color_hex(HY_C_TEXT2), 0);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(r, 12, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(HY_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 22, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_outline_width(r, 0, 0);
    lv_obj_set_style_text_font(r, sel_font, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(HY_C_TEXT), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r, lv_color_hex(HY_C_SURFACE2), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
    roller_ctx_t *c = lv_malloc(sizeof *c);
    if (c) {
        c->changed = changed;
        c->start = 0;
        c->ok = true;
        lv_obj_add_event_cb(r, roller_event_cb, LV_EVENT_ALL, c);
    }
    return r;
}

lv_obj_t *ui_content(void)
{
    return s_content;
}

void ui_fmt_hhmm(int64_t ts, const char *tz, char *out, size_t len)
{
    char full[32];
    hy_format_local(ts, tz, full, sizeof full);   /* "Tue 27 Oct 22:04" */
    size_t n = strlen(full);
    snprintf(out, len, "%s", n >= 5 ? full + n - 5 : full);
}

void ui_log_hwm(const char *what)
{
    static const char *seen[4];
    for (size_t i = 0; i < 4; i++) {
        if (seen[i] && strcmp(seen[i], what) == 0) {
            return;
        }
    }
    for (size_t i = 0; i < 4; i++) {
        if (!seen[i]) {
            seen[i] = what;
            break;
        }
    }
    ESP_LOGI(TAG, "stack margin after first %s: task \"%s\" never below %u bytes free",
             what, pcTaskGetName(NULL), (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

esp_err_t hy_ui_start(void)
{
    if (!hy_board_display()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!hy_board_lock(2000)) {
        return ESP_ERR_TIMEOUT;
    }
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, HY_SCREEN, HY_SCREEN);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_screen, false);   /* stays clickable: every touch reaches the input layer */

    s_content = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_pos(s_content, 0, HY_BAR_H);
    lv_obj_set_scrollable(s_content, false);
    lv_obj_set_clickable(s_content, false);

    topbar_create();
    splash_yield();
    nav_host_init();
    toast_create(s_screen);
    splash_yield();
    if (splash_remaining_ms() > 0) {
        hy_board_unlock();
        splash_wait();   /* without the lock: the splash animations run in the LVGL task */
        if (!hy_board_lock(2000)) {
            return ESP_ERR_TIMEOUT;
        }
    }
    splash_finish(s_screen);
    /* input, the 1 s tick, the first evaluation (a due card and its chime) and the screen state start with the content */
    nav_host_warm_start();   /* the hidden views, one every 250 ms in the LVGL task */
    idle_create();
    poweroff_create();
    screen_init();
    input_init(hy_board_indev());
    lv_timer_create(tick_cb, 1000, NULL);
    lv_async_call(first_tick, NULL);   /* the first evaluation runs in the LVGL task, not here */
    int sub = hy_ev_subscribe(event_cb, NULL);
    static const hy_power_ui_t power_ui = {.battery = battery_cb,
                                           .pwr_short = poweroff_pwr_short,
                                           .pwr_long = poweroff_pwr_long,
                                           .usb = power_usb,
                                           .wake = power_wake};
    hy_power_set_ui(&power_ui);
    hy_board_unlock();

    if (sub < 0) {
        ESP_LOGE(TAG, "event subscribe failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "screen shell up");
    return ESP_OK;
}
