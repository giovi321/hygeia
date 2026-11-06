#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hy/cabinet_run.h"
#include "hy/clock.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "hy/web.h"
#include "ui_internal.h"

static const char *TAG = "ui_cab";

/* Why the reaction is not allowed now; NULL when it is. Re-checked here because a log may have landed between the
   runner's check in the motion task and this callback. */
static const char *react_blocked(void)
{
    const hy_clock_port_t *ck = hy_clock_port();
    if (!ck->valid(NULL)) {
        return "clock not valid";
    }
    int64_t at = hy_sched_run_attention_from();
    if (at == 0) {
        return "nothing to remind";
    }
    if (at > ck->now(NULL)) {
        return "too early";
    }
    return NULL;
}

/* A cabinet chime that did not light the screen (Light off, screen DIM or OFF) leaves its reason here until the next
   wake shows it. LVGL task only. */
static struct {
    bool set;
    int64_t slot;   /* the dose time the note names; the text is rebuilt at the wake */
} s_pending;

/* Home with the "Next: A at 14:00" toast, unless a dose is open (the due card explains the wake), a form has unsaved
   edits or an overlay is up. */
static void show_next(const char *name, const char *hhmm)
{
    if (hy_sched_snapshot()->out.n_open != 0 || ui_nav_form_dirty() || poweroff_visible() || pair_visible()) {
        return;
    }
    char b[80];
    ui_nav_home();
    snprintf(b, sizeof b, HY_S_CAB_NEXT, name, hhmm);
    toast_show_info(b);
}

/* idle.c, when the screen comes to ON from DIM or OFF by any cause. Shows the remembered reason once. It is dropped
   instead when a dose is open (the due card takes over), when its dose time has come or when that dose no longer is
   the next one (logged early). */
void cabinet_ui_screen_woke(void)
{
    if (!s_pending.set) {
        return;
    }
    s_pending.set = false;
    const hy_clock_port_t *ck = hy_clock_port();
    char name[33], hhmm[8];
    int64_t slot = 0;
    if (!ck->valid(NULL) || ck->now(NULL) >= s_pending.slot || !idle_next_dose(name, sizeof name, hhmm, sizeof hhmm, &slot) ||
        slot != s_pending.slot) {
        return;
    }
    ESP_LOGI(TAG, "cabinet: reason shown at the wake");
    show_next(name, hhmm);
}

static void react(void)
{
    const char *why = react_blocked();
    if (why) {
        ESP_LOGI(TAG, "cabinet: no reaction (%s)", why);
        return;
    }
    const hy_settings_t *st = hy_settings();
    bool chime = st->cab_chime, light = st->cab_light;
    if (!chime && !light) {
        ESP_LOGI(TAG, "cabinet: no reaction (both switches off)");
        return;
    }
    /* The screen state is read here, in the LVGL task that owns it. The runner only knows "on or dimmed", and a
       check there could race with a wake, so it reacts, starts its pause and counts, and this gives all of it back
       when the screen is on: you are already looking at it. DIM and OFF react as before. */
    if (screen_state() == HY_SCR_ON) {
        hy_cab_run_react_undo();
        ESP_LOGI(TAG, "cabinet: no reaction (screen on)");
        return;
    }
    s_pending.set = false;   /* this reaction replaces an older note */
    /* the chime is gated by sound, do-not-disturb and quiet hours; the light is not */
    bool sounded = false;
    if (chime) {
        sounded = hy_sched_chime_once("cabinet");
    }
    bool woke = false;
    if (light) {   /* the screen is DIM or OFF here: wake to ON */
        screen_wake("cabinet");
        woke = true;
    }
    ESP_LOGI(TAG, "cabinet: react chime %d light %d", chime ? 1 : 0, light ? 1 : 0);
    char name[33], hhmm[8];
    int64_t slot = 0;
    bool have = idle_next_dose(name, sizeof name, hhmm, sizeof hhmm, &slot);
    if (woke) {
        /* No dose open (a snoozed one counts as open): the due card is not what the wake shows, so Home shows the next
           dose for 5 s */
        if (have) {
            show_next(name, hhmm);
        }
    } else if (sounded && have && hy_sched_snapshot()->out.n_open == 0) {
        /* The chime sounded with a dark or dim screen: keep the reason for the next wake. A silenced chime leaves
           nothing to explain. */
        s_pending.set = true;
        s_pending.slot = slot;
    }
}

/* The single dispatch point for the runner's events (LVGL task). The teach and test events go to the cabinet views,
   which ignore them unless a teach or a test is running. */
void cabinet_ui_event(const hy_cabev_t *e)
{
    switch (e->kind) {
    case HY_CABEV_REACT:
        react();
        break;
    case HY_CABEV_FAULT:
        ESP_LOGW(TAG, "cabinet: sensor fault, paused; tried again at the next screen wake or a settings change");
        cab_event(e);
        break;
    case HY_CABEV_MATCH:
    case HY_CABEV_OTHER:
        hy_web_cabinet_event(e->dist, e->thr, e->kind == HY_CABEV_MATCH);
        cab_event(e);
        break;
    default:
        cab_event(e);
        break;
    }
}

void cabinet_ui_init(void)
{
    static const hy_cab_ui_t ui = {.event = cabinet_ui_event};
    esp_err_t err = hy_cab_run_init(&ui);
    ESP_LOGI(TAG, "hy_cab_run_init: %s", esp_err_to_name(err));
    hy_cab_run_config(hy_settings());
}

#if CONFIG_HY_DEBUG_CONSOLE
void cabinet_debug(const char *arg)
{
    if (arg && strcmp(arg, "react") == 0) {
        static const hy_cabev_t e = {.kind = HY_CABEV_REACT};
        cabinet_ui_event(&e);
        return;
    }
    if (arg && strncmp(arg, "ui", 2) == 0) {
        const char *p = arg + 2;
        while (*p == ' ') {
            p++;
        }
        cab_debug_ui(p);
        return;
    }
    if (arg && strncmp(arg, "press", 5) == 0) {
        const char *p = arg + 5;
        while (*p == ' ') {
            p++;
        }
        cab_debug_press(p);
        return;
    }
    ESP_LOGI(TAG, "usage: cab react | cab ui <phase> | cab press <button>");
}
#endif
