/* Navigation host: owns the hy_nav_t, keeps one view per hy_view_t inside ui_content() and shows the top of
 * the stack. Views that later tasks provide (quick settings, settings, set the time, idle)
 * are stubs here. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hy/clock.h"
#include "hy/ui.h"
#include "ui_internal.h"

static const char *TAG = "ui_nav";

#define N_VIEWS (HY_V_STORAGE_DOWN + 1)

static hy_nav_t s_nav;
static lv_obj_t *s_pager;
static lv_obj_t *s_views[N_VIEWS];   /* NULL for the pager pages, which share s_pager */
static hy_view_t s_shown;
static bool s_started;
static volatile bool s_storage_down;

static const char *const view_names[N_VIEWS] = {
    "HOME", "OVERVIEW", "HISTORY", "DETAIL", "NOTE", "EARLIER", "QS", "SETTINGS", "SET_ZONE", "SET_TIME",
    "SET_SOUND", "SET_DISPLAY", "ABOUT", "CLOCK_INVALID", "STORAGE_DOWN",
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

/* ---- stub views until their tasks land ---- */

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

static lv_obj_t *stub_titled(const char *title, const char *body)
{
    lv_obj_t *v = ui_view_box();
    lv_obj_t *t = ui_label(v, hy_type.title, HY_C_TEXT, title);
    lv_obj_set_pos(t, 24, 2);
    if (body) {
        lv_obj_t *b = ui_label(v, hy_type.status, HY_C_TEXT2, body);
        lv_obj_set_width(b, HY_SCREEN - 48);
        lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_pos(b, 24, 48);
    }
    return v;
}

/* fx handlers: the idle module (Task 17) replaces the wake stub */
static void fx_wake(void) { ESP_LOGI(TAG, "fx WAKE (idle module not present)"); }
static void fx_snooze(void) { due_snooze(); }
static void fx_due_next(void) { due_next(); }

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
    default:
        break;
    }
}

void ui_show_view(hy_view_t v)
{
    if ((unsigned)v >= N_VIEWS) {
        return;
    }
    bool pager = is_pager(v);
    for (int i = 0; i < N_VIEWS; i++) {
        if (s_views[i]) {
            lv_obj_set_hidden(s_views[i], s_views[i] != s_views[v]);
        }
    }
    lv_obj_set_hidden(s_pager, !pager);
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
    s_shown = v;   /* before the refresh: the page refreshes only while it is the shown view */
    refresh_view(v);
}

void ui_nav_log(void)
{
    char b[128];
    size_t o = (size_t)snprintf(b, sizeof b, "nav: ");
    for (uint8_t i = 0; i < s_nav.depth && o < sizeof b; i++) {
        o += (size_t)snprintf(b + o, sizeof b - o, "%s%s", i ? " > " : "", view_names[s_nav.stack[i]]);
    }
    if (o < sizeof b && (s_nav.idle || s_nav.due)) {
        snprintf(b + o, sizeof b - o, "%s%s", s_nav.idle ? " [idle]" : "", s_nav.due ? " [due]" : "");
    }
    ESP_LOGI(TAG, "%s", b);
}

static void show_top(void)
{
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
    ESP_LOGI(TAG, "idle flag %s (stub until the idle module)", idle ? "set" : "cleared");
}

void ui_nav_set_due(bool due)
{
    s_nav.due = due;
    topbar_set_grab(!due && s_shown == HY_V_HOME);   /* pull-down is ignored while the card is up */
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

#if CONFIG_HY_DEBUG_CONSOLE
void ui_nav_reset(void)
{
    s_nav.depth = 1;
    if (s_nav.stack[0] == HY_V_OVERVIEW || s_nav.stack[0] == HY_V_HISTORY) {
        s_nav.stack[0] = HY_V_HOME;
    }
    show_top();
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

void nav_host_init(void)
{
    hy_nav_init(&s_nav);
    s_pager = pager_create(ui_content());
    home_create(pager_page(0));
    overview_create(pager_page(1));
    history_create(pager_page(2));

    s_views[HY_V_STORAGE_DOWN] = storage_down_create(ui_content());
    lv_obj_set_hidden(s_views[HY_V_STORAGE_DOWN], true);
    s_views[HY_V_CLOCK_INVALID] = stub_titled(HY_S_TIME_TITLE, HY_S_TIME_BODY);
    s_views[HY_V_QS] = stub_titled(HY_S_QS_TITLE, NULL);
    detail_create();
    note_create();
    earlier_create();
    lv_obj_t *other = ui_view_box();   /* settings and its sub-views: empty until their tasks */
    for (int v = HY_V_NOTE; v < N_VIEWS; v++) {
        if (!s_views[v] && !is_pager((hy_view_t)v)) {
            s_views[v] = other;
        }
    }

    due_create();   /* last: above every view, below the toast */

    s_shown = HY_V_HOME;
    hy_nav_set_root_flags(&s_nav, !hy_clock_valid(), s_storage_down);
    show_top();
    s_started = true;
    ui_nav_log();
}
