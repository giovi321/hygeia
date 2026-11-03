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
    /* the chime is gated by sound, do-not-disturb and quiet hours; the light is not */
    if (chime) {
        hy_sched_chime_once("cabinet");
    }
    bool woke = false;
    if (light && screen_state() != HY_SCR_ON) {   /* ON: the chime alone; DIM and OFF wake to ON */
        screen_wake("cabinet");
        woke = true;
    }
    ESP_LOGI(TAG, "cabinet: react chime %d light %d", chime ? 1 : 0, light ? 1 : 0);
    /* No dose open (a snoozed one counts as open): the due card is not what the wake shows, so Home shows the next
       dose for 5 s */
    if (woke && hy_sched_snapshot()->out.n_open == 0 && !ui_nav_form_dirty() && !poweroff_visible() &&
        !pair_visible()) {
        char name[33], hhmm[8], b[80];
        if (idle_next_dose(name, sizeof name, hhmm, sizeof hhmm)) {
            ui_nav_home();
            snprintf(b, sizeof b, HY_S_CAB_NEXT, name, hhmm);
            toast_show_info(b);
        }
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
        ESP_LOGW(TAG, "cabinet: sensor fault, the feature is off until a settings change");
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
    ESP_LOGI(TAG, "usage: cab react");
}
#endif
