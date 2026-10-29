/* Idle clock: after HY_IDLE_MS without a touch and with no due card, a black overlay on the top layer (above the
 * top bar) shows the time, the next dose and the status line, and the brightness drops to min(setting, 20%).
 * The group moves a few pixels each minute against burn-in (hy_idle_drift). Any touch only wakes: the nav
 * state machine swallows the gesture as WAKE (rule 1) and the input layer marks the stroke as waking, so no
 * widget acts on it. A newly showing due entry wakes the screen too. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "hy/occ.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "ui_internal.h"

static const char *TAG = "idle";

#define IDLE_MS      30000
#define IDLE_DIM_MAX 20
#define FADE_MS      200

static lv_obj_t *s_ov, *s_grp, *s_clock, *s_next, *s_status;
static bool s_active;
static int64_t s_minute = -1;
static int32_t s_bri;   /* the brightness the idle animation drives */

static void bri_exec(void *v, int32_t b)
{
    (void)v;
    s_bri = b;
    hy_board_set_brightness((uint8_t)b);
}

static void bri_to(int32_t from, int32_t to)
{
    lv_anim_delete(&s_bri, bri_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_bri);
    lv_anim_set_exec_cb(&a, bri_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* The due flash and the sliders set the brightness themselves: they stop a running fade first */
void idle_brightness_stop(void)
{
    lv_anim_delete(&s_bri, bri_exec);
}

bool idle_active(void)
{
    return s_active;
}

/* a slot an early taken or skipped dose already satisfies */
static bool slot_done(const hy_snapshot_t *sn, const hy_med_t *m, int64_t slot)
{
    for (size_t i = sn->in.n_logs; i-- > 0;) {
        const hy_log_t *l = &sn->in.logs[i];
        if (l->ts < sn->in.now - 86400) {
            break;
        }
        /* the log carries the slot it satisfied: no re-matching (the log set contains the log itself) */
        if (l->med_id == m->id && l->slot == slot && (l->status == HY_TAKEN || l->status == HY_SKIPPED)) {
            return true;
        }
    }
    return false;
}

/* earliest upcoming slot of the active fixed medicines within 8 days that no logged dose satisfies; 0 when none */
static int64_t next_slot(const hy_snapshot_t *sn, const char **code)
{
    int64_t best = 0;
    for (size_t i = 0; i < sn->in.n_meds; i++) {
        int64_t occ[8];
        const hy_med_t *m = &sn->in.meds[i];
        if (m->type != HY_SCHED_FIXED) {
            continue;
        }
        size_t n = hy_occurrences(m, sn->in.now + 1, sn->in.now + 8 * 86400, sn->in.tz, occ, 8);
        for (size_t k = 0; k < n; k++) {
            if (best && occ[k] >= best) {
                break;
            }
            if (!slot_done(sn, m, occ[k])) {
                best = occ[k];
                *code = m->code;
                break;
            }
        }
    }
    return best;
}

static void render(void)
{
    const hy_clock_port_t *ck = hy_clock_port();
    bool valid = ck->valid(NULL);
    int64_t now = valid ? ck->now(NULL) : 0;
    char tz[HY_TZ_LEN], hhmm[8], b[64];
    hy_clock_tz_copy(tz, sizeof tz);
    if (valid) {
        ui_fmt_hhmm(now, tz, hhmm, sizeof hhmm);
        lv_label_set_text(s_clock, hhmm);
    } else {
        lv_label_set_text(s_clock, HY_S_TIME_UNSET);
    }
    const hy_snapshot_t *sn = hy_sched_snapshot();
    const char *code = "";
    int64_t nx = valid && sn->tracking && hy_sched_generation() ? next_slot(sn, &code) : 0;
    if (!valid) {
        snprintf(b, sizeof b, "%s", HY_S_IDLE_SET_TIME);
    } else if (nx) {
        ui_fmt_hhmm(nx, tz, hhmm, sizeof hhmm);
        snprintf(b, sizeof b, HY_S_IDLE_NEXT, code, hhmm);
    } else {
        snprintf(b, sizeof b, "%s", HY_S_IDLE_NONE);
    }
    lv_label_set_text(s_next, b);
    int64_t minute = (valid ? now : (int64_t)(lv_tick_get() / 1000)) / 60;
    int8_t dx, dy;
    hy_idle_drift(minute, &dx, &dy);
    lv_obj_set_pos(s_grp, dx, dy);
    s_minute = minute;
}

void idle_enter(void)
{
    if (s_active || !s_ov || due_visible()) {
        return;
    }
    s_active = true;
    ui_nav_set_idle(true);
    render();
    lv_obj_set_hidden(s_ov, false);
    lv_obj_move_foreground(s_ov);
    int32_t set = hy_settings()->brightness;
    int32_t dim = set < IDLE_DIM_MAX ? set : IDLE_DIM_MAX;
    ESP_LOGI(TAG, "idle: enter (brightness %d -> %d)", (int)set, (int)dim);
    bri_to(set, dim);
}

static void wake(const char *why)
{
    if (!s_active) {
        ui_nav_set_idle(false);
        return;
    }
    s_active = false;
    ui_nav_set_idle(false);
    lv_obj_set_hidden(s_ov, true);
    lv_display_trigger_activity(NULL);
    bri_to(s_bri, hy_settings()->brightness);
    ESP_LOGI(TAG, "idle: wake%s", why);
}

void idle_wake(void)
{
    wake("");
}

void idle_wake_due(void)
{
    wake(" (due)");
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_active) {
        const hy_clock_port_t *ck = hy_clock_port();
        int64_t minute = (ck->valid(NULL) ? ck->now(NULL) : (int64_t)(lv_tick_get() / 1000)) / 60;
        if (minute != s_minute) {
            render();
        }
        return;
    }
    if (due_visible() || ui_nav_idle()) {
        return;
    }
    if (lv_display_get_inactive_time(NULL) >= IDLE_MS) {
        idle_enter();
    }
}

void idle_create(void)
{
    s_ov = lv_obj_create(lv_layer_top());   /* after topbar_create: above the top bar */
    lv_obj_remove_style_all(s_ov);
    lv_obj_set_size(s_ov, HY_SCREEN, HY_SCREEN);
    lv_obj_set_pos(s_ov, 0, 0);
    lv_obj_set_style_bg_color(s_ov, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_ov, false);
    lv_obj_set_clickable(s_ov, true);   /* absorbs the touch: nothing underneath receives the waking stroke */
    lv_obj_set_hidden(s_ov, true);

    s_grp = ui_plain(s_ov);
    lv_obj_set_size(s_grp, HY_SCREEN, HY_SCREEN);
    s_clock = ui_label(s_grp, hy_type.clock, HY_C_TEXT2, HY_S_TIME_UNSET);
    lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, -33);
    s_next = ui_label(s_grp, hy_type.body, HY_C_TEXT2, "");
    lv_obj_align(s_next, LV_ALIGN_CENTER, 0, 65);
    s_status = ui_label(s_grp, hy_type.status, HY_C_TEXT2, HY_S_IDLE_STATUS);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 110);
    lv_timer_create(tick_cb, 500, NULL);
}
