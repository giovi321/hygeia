/* Navigation host: owns the hy_nav_t, keeps one view per hy_view_t inside ui_content() and shows the top of
 * the stack. The screen state machine and the idle clock are idle.c; it keeps the nav idle flag equal to
 * "state is DIM or OFF". */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "hy/clock.h"
#include "hy/ui.h"
#include "ui_internal.h"

static const char *TAG = "ui_nav";

#define N_VIEWS (HY_V_MED_ARCHIVE + 1)

static hy_nav_t s_nav;
static lv_obj_t *s_pager;
static lv_obj_t *s_views[N_VIEWS];   /* NULL for the pager pages, which share s_pager */
static hy_view_t s_shown;
static bool s_started;
static volatile bool s_storage_down;

static const char *const view_names[N_VIEWS] = {
    "HOME", "OVERVIEW", "HISTORY", "DETAIL", "NOTE", "EARLIER", "QS", "SETTINGS", "SET_ZONE", "SET_TIME",
    "SET_SOUND", "SET_DISPLAY", "ABOUT", "CLOCK_INVALID", "STORAGE_DOWN",
    "MEDFORM", "MED_ARCHIVE",
};

static bool is_pager(hy_view_t v)
{
    return v == HY_V_HOME || v == HY_V_OVERVIEW || v == HY_V_HISTORY;
}

const char *ui_gesture_name(hy_gesture_t g)
{
    static const char *const n[] = {"none", "tap", "long", "left", "right", "up", "down", "edgedown",
                                    "edgeup", "edgeright"};
    return (unsigned)g < sizeof n / sizeof n[0] ? n[g] : "?";
}

/* ---- view containers ---- */

lv_obj_t *ui_view_box(void)
{
    lv_obj_t *v = lv_obj_create(ui_content());
    lv_obj_remove_style_all(v);
    lv_obj_set_size(v, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_pos(v, 0, 0);
    lv_obj_set_style_bg_color(v, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(v, false);
    lv_obj_set_clickable(v, false);
    lv_obj_set_hidden(v, true);
    return v;
}

void ui_register_view(hy_view_t v, lv_obj_t *obj)
{
    if ((unsigned)v < N_VIEWS) {
        s_views[v] = obj;
    }
}

/* fx handlers */
static void fx_wake(void) { idle_wake(); }
static void fx_snooze(void) { due_snooze(); }
static void fx_due_next(void) { due_next(); }
static void fx_due_prev(void) { due_prev(); }

/* ---- showing ---- */

static bool in_detail_group(hy_view_t v)
{
    return v == HY_V_DETAIL || v == HY_V_NOTE || v == HY_V_EARLIER;
}

static void refresh_view(hy_view_t v)
{
    switch (v) {
    case HY_V_HOME:
        home_refresh_if_changed();
        break;
    case HY_V_OVERVIEW:
        overview_refresh_if_changed();
        break;
    case HY_V_HISTORY:
        history_show();
        break;
    case HY_V_DETAIL:
        detail_show(s_nav.med_id);
        break;
    case HY_V_NOTE:
        note_show(s_nav.med_id);
        break;
    case HY_V_EARLIER:
        earlier_show(s_nav.med_id);
        break;
    case HY_V_MEDFORM:
        medform_show();
        break;
    case HY_V_MED_ARCHIVE:
        medarchive_show(s_nav.med_id);
        break;
    case HY_V_QS:
        qs_show();
        break;
    case HY_V_SETTINGS:
    case HY_V_SET_ZONE:
    case HY_V_SET_SOUND:
    case HY_V_SET_DISPLAY:
    case HY_V_ABOUT:
        settings_show(v);
        break;
    case HY_V_SET_TIME:
        timeset_show(false);
        break;
    case HY_V_CLOCK_INVALID:
        timeset_show(true);
        break;
    default:
        break;
    }
}

/* The view under quick settings stays visible while the 200 ms slide runs, then the slide hides it */
void ui_qs_slide_done(void)
{
    if (s_shown != HY_V_QS) {
        return;
    }
    for (int i = 0; i < N_VIEWS; i++) {
        if (s_views[i] && s_views[i] != s_views[HY_V_QS]) {
            lv_obj_set_hidden(s_views[i], true);
        }
    }
    lv_obj_set_hidden(s_pager, true);
}

static void ensure_view(hy_view_t v);

void ui_show_view(hy_view_t v)
{
    if ((unsigned)v >= N_VIEWS) {
        return;
    }
    ensure_view(v);
    bool pager = is_pager(v);
    bool under = v == HY_V_QS && s_started && s_shown != HY_V_QS;   /* keep the previous view for the slide */
    for (int i = 0; i < N_VIEWS; i++) {
        if (s_views[i]) {
            bool prev = under && !is_pager(s_shown) && s_views[i] == s_views[s_shown];
            lv_obj_set_hidden(s_views[i], s_views[i] != s_views[v] && !prev);
        }
    }
    lv_obj_set_hidden(s_pager, !pager && !(under && is_pager(s_shown)));
    if (pager) {
        /* animate only a page change within the pager */
        pager_show((int)(v - HY_V_HOME), s_started && is_pager(s_shown) && s_shown != v);
    }
    topbar_set_grab(v == HY_V_HOME && !s_nav.due);
    if (s_shown == HY_V_HISTORY && v != HY_V_HISTORY) {
        history_hide();
    }
    if (in_detail_group(s_shown) && !in_detail_group(v)) {
        detail_hide();
    }
    bool from_qs = s_shown == HY_V_QS;
    s_shown = v;   /* before the refresh: the page refreshes only while it is the shown view */
    if (from_qs && v == HY_V_SET_ZONE) {
        return;   /* quick settings was only pulled over the zone picker: keep what the user turned. The time
                   * screens always refresh: a zone change shifts the wall time their rollers show (P50) */
    }
    refresh_view(v);
}

void ui_nav_log(void)
{
    char b[128];
    size_t o = (size_t)snprintf(b, sizeof b, "nav: ");
    for (uint8_t i = 0; i < s_nav.depth && o < sizeof b; i++) {
        o += (size_t)snprintf(b + o, sizeof b - o, "%s%s", i ? " > " : "", view_names[s_nav.stack[i]]);
    }
    if (o < sizeof b && (s_nav.idle || s_nav.due || s_nav.poweroff)) {
        snprintf(b + o, sizeof b - o, "%s%s%s", s_nav.idle ? " [idle]" : "", s_nav.due ? " [due]" : "",
                 s_nav.poweroff ? " [poweroff]" : "");
    }
    ESP_LOGI(TAG, "%s", b);
}

/* The form draft lives in medform.c; the nav rules only need to know whether it has unsaved changes. The flag is
 * cleared when the form leaves the stack, whatever took it away (back, a discard, a root change). */
static void form_sync(void)
{
    for (uint8_t i = 0; i < s_nav.depth; i++) {
        if (s_nav.stack[i] == HY_V_MEDFORM) {
            return;
        }
    }
    s_nav.form_dirty = false;
}

void ui_nav_set_form_dirty(bool dirty)
{
    s_nav.form_dirty = dirty;
}

static void show_top(void)
{
    form_sync();
    ui_show_view(hy_nav_view(&s_nav));
}

hy_nav_fx_t ui_nav_gesture(hy_gesture_t g)
{
    hy_nav_fx_t fx = hy_nav_gesture(&s_nav, g);
    switch (fx) {
    case HY_FX_WAKE:
        fx_wake();
        break;
    case HY_FX_SNOOZE:
        fx_snooze();
        break;
    case HY_FX_DUE_NEXT:
        fx_due_next();
        break;
    case HY_FX_DUE_PREV:
        fx_due_prev();
        break;
    case HY_FX_FORM_BACK:
        medform_back();
        break;
    case HY_FX_NONE:
        break;
    }
    if (hy_nav_view(&s_nav) != s_shown) {
        show_top();
    }
    ui_nav_log();
    return fx;
}

hy_view_t ui_current_view(void)
{
    return s_shown;
}

bool ui_nav_idle(void)
{
    return s_nav.idle;
}

void ui_nav_set_idle(bool idle)
{
    s_nav.idle = idle;
}

void ui_nav_set_due(bool due)
{
    s_nav.due = due;
    topbar_set_grab(!due && s_shown == HY_V_HOME);   /* pull-down is ignored while the card is up */
}

void ui_nav_set_poweroff(bool on)
{
    s_nav.poweroff = on;
}

void ui_nav_push(hy_view_t v)
{
    hy_nav_push(&s_nav, v);
    show_top();
    ui_nav_log();
}

void ui_nav_pop(void)
{
    if (s_nav.depth > 1) {
        s_nav.depth--;
    }
    show_top();
    ui_nav_log();
}

void ui_nav_home(void)
{
    s_nav.depth = 1;
    if (s_nav.stack[0] == HY_V_OVERVIEW || s_nav.stack[0] == HY_V_HISTORY) {
        s_nav.stack[0] = HY_V_HOME;
    }
    show_top();
    ui_nav_log();
}

#if CONFIG_HY_DEBUG_CONSOLE
void ui_nav_reset(void)
{
    ui_nav_home();
}
#endif

void ui_open_detail(uint32_t med_id)
{
    hy_nav_open_detail(&s_nav, med_id);
    ESP_LOGI(TAG, "detail: med %u", (unsigned)med_id);
    show_top();
    ui_nav_log();
}

/* ---- root flags ---- */

void nav_host_tick(void)
{
    bool invalid = !hy_clock_valid();
    bool down = s_storage_down;
    if (invalid == s_nav.clock_invalid && down == s_nav.storage_down) {
        return;
    }
    hy_nav_set_root_flags(&s_nav, invalid, down);
    ESP_LOGI(TAG, "root: clock %s, storage %s", invalid ? "invalid" : "valid", down ? "down" : "ok");
    show_top();
    ui_nav_log();
}

void ui_set_storage_down(bool down)
{
    s_storage_down = down;
    nav_host_tick();
}

void hy_ui_set_storage_ok(bool ok)
{
    s_storage_down = !ok;   /* read by nav_host_tick in the LVGL task */
}

/* Views that are not on screen at boot are created after the splash (one every WARM_MS from nav_host_warm_start)
 * or at their first show, whichever comes first. The splash needs steady frames; each create below is one
 * blocking LVGL step. With CONFIG_HY_DEBUG_CONSOLE every create logs its cost. */
typedef enum { LZ_DETAIL, LZ_NOTE, LZ_EARLIER, LZ_MEDFORM, LZ_ARCHIVE, LZ_QS, LZ_SETTINGS, LZ_TIME, LZ_INVALID, LZ_N } lazy_t;

#define WARM_MS 250

static bool s_lazy_done[LZ_N];
static lv_timer_t *s_warm_t;

static lazy_t lazy_of(hy_view_t v, bool *is_lazy)
{
    *is_lazy = true;
    switch (v) {
    case HY_V_DETAIL:
        return LZ_DETAIL;
    case HY_V_NOTE:
        return LZ_NOTE;
    case HY_V_EARLIER:
        return LZ_EARLIER;
    case HY_V_MEDFORM:
        return LZ_MEDFORM;
    case HY_V_MED_ARCHIVE:
        return LZ_ARCHIVE;
    case HY_V_QS:
        return LZ_QS;
    case HY_V_SETTINGS:
    case HY_V_SET_ZONE:
    case HY_V_SET_SOUND:
    case HY_V_SET_DISPLAY:
    case HY_V_ABOUT:
        return LZ_SETTINGS;
    case HY_V_SET_TIME:
        return LZ_TIME;
    case HY_V_CLOCK_INVALID:
        return LZ_INVALID;
    default:
        *is_lazy = false;
        return LZ_N;
    }
}

static void create_lazy(lazy_t k)
{
    if (s_lazy_done[k]) {
        return;
    }
    s_lazy_done[k] = true;
#if CONFIG_HY_DEBUG_CONSOLE
    int64_t t0 = esp_timer_get_time();
#endif
    switch (k) {
    case LZ_DETAIL:
        detail_create();
        break;
    case LZ_NOTE:
        note_create();
        break;
    case LZ_EARLIER:
        earlier_create();
        break;
    case LZ_MEDFORM:
        medform_create();
        break;
    case LZ_ARCHIVE:
        medarchive_create();
        break;
    case LZ_QS:
        qs_create();
        break;
    case LZ_SETTINGS:
        settings_create();
        break;
    case LZ_TIME:
        s_views[HY_V_SET_TIME] = ui_view_box();
        timeset_create(s_views[HY_V_SET_TIME], false);
        break;
    case LZ_INVALID:
        s_views[HY_V_CLOCK_INVALID] = ui_view_box();
        timeset_create(s_views[HY_V_CLOCK_INVALID], true);
        break;
    default:
        break;
    }
    due_raise();   /* the card stays above every view, as when it was created last */
#if CONFIG_HY_DEBUG_CONSOLE
    static const char *const names[LZ_N] = {"detail", "note", "earlier", "medform", "archive", "qs", "settings", "time", "clock-invalid"};
    ESP_LOGI(TAG, "create %s: %lld us", names[k], (long long)(esp_timer_get_time() - t0));
#endif
}

static void ensure_view(hy_view_t v)
{
    bool is_lazy;
    lazy_t k = lazy_of(v, &is_lazy);
    if (is_lazy) {
        create_lazy(k);
    }
}

static void warm_cb(lv_timer_t *t)
{
    for (int k = 0; k < LZ_N; k++) {
        if (!s_lazy_done[k]) {
            create_lazy((lazy_t)k);
            return;
        }
    }
    lv_timer_delete(t);
    s_warm_t = NULL;
}

/* LVGL lock held, after the splash: creates the hidden views one per WARM_MS in the LVGL task */
void nav_host_warm_start(void)
{
    if (!s_warm_t) {
        s_warm_t = lv_timer_create(warm_cb, WARM_MS, NULL);
    }
}

/* Creates every hidden view now (the debug console pokes widgets of views that were never shown) */
void nav_host_warm_all(void)
{
    for (int k = 0; k < LZ_N; k++) {
        create_lazy((lazy_t)k);
    }
}

#if CONFIG_HY_DEBUG_CONSOLE
#define TIMED(name, call)                                                                \
    do {                                                                                 \
        int64_t t0_ = esp_timer_get_time();                                              \
        call;                                                                            \
        ESP_LOGI(TAG, "create %s: %lld us", name, (long long)(esp_timer_get_time() - t0_)); \
    } while (0)
#else
#define TIMED(name, call) call
#endif

void nav_host_init(void)
{
    hy_nav_init(&s_nav);
    TIMED("pager", s_pager = pager_create(ui_content()));
    splash_yield();
    TIMED("home", home_create(pager_page(0)));
    splash_yield();
    TIMED("overview", overview_create(pager_page(1)));
    splash_yield();
    TIMED("history", history_create(pager_page(2)));
    splash_yield();

    s_views[HY_V_STORAGE_DOWN] = storage_down_create(ui_content());
    lv_obj_set_hidden(s_views[HY_V_STORAGE_DOWN], true);
    splash_yield();
    due_create();   /* the other views are created later and move the card back to the top */

    s_shown = HY_V_HOME;
    hy_nav_set_root_flags(&s_nav, !hy_clock_valid(), s_storage_down);
    show_top();
    s_started = true;
    ui_nav_log();
}
