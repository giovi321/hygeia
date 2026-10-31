/* Screen state machine (hy_screen_*, hy_core) and its views. The state is ON, DIM or OFF:
 * - TICK every second (ui.c), touch strokes (TOUCH), a newly showing due card (DUE), each chime moment (CHIME: wakes
 *   on USB only, R2), USB in and out, PWR short (toggles ON <-> OFF) and the user key drive it, with the dim_s,
 *   off_bat_s and off_usb_s settings.
 * - Hold (no dim, no off; a dimmed screen comes back ON): the power-off confirmation or the critical countdown shows,
 *   or a due card shows on USB (R1). On battery a due card dims and goes off like any view.
 * - DIM is the idle clock: a black overlay on the top layer, above the top bar and below the power-off overlay, with
 *   the time, the next dose and the status line, brightness min(setting, 20%), moved a few pixels each minute
 *   against burn-in (hy_idle_drift).
 * - OFF blanks the panel and asks the power task for the sleep mode (LVGL paused, light sleep without VBUS), again
 *   after each tick while it stays OFF. Not while the critical countdown shows: it keeps counting.
 * The nav idle flag is "state is DIM or OFF": the first stroke on a dimmed or dark screen only wakes it (nav rule 1
 * and the input layer), whatever view or overlay shows. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "hy/occ.h"
#include "hy/power_run.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "ui_internal.h"

static const char *TAG = "screen";

#define IDLE_DIM_MAX 20
#define FADE_MS      200
#define GUARD_MS     1000   /* a touch that woke the sleeping CPU: a stroke starting this soon after only wakes */

static lv_obj_t *s_ov, *s_grp, *s_clock, *s_next, *s_status;
static bool s_active;   /* the idle clock overlay shows (DIM and OFF) */
static int64_t s_minute = -1;
static int32_t s_bri;   /* the brightness the idle animation drives */

static hy_screen_t s_scr;
static hy_scr_t s_shown = HY_SCR_ON;   /* the state the views show */
static bool s_inited;
static bool s_nousb;                   /* debug `scr nousb`: the state machine runs as on battery */
static uint32_t s_chime_gen;           /* scheduler generation whose chime was counted */
static bool s_guard;
static uint32_t s_guard_t0;

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
    return s_shown != HY_SCR_ON;
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

/* code of the first open, not snoozed dose (the order the due card lists them); "" when its medicine is unknown */
static const char *open_code(const hy_snapshot_t *sn)
{
    for (size_t i = 0; i < sn->out.n_open; i++) {
        const hy_open_t *op = &sn->out.open[i];
        if (op->snoozed_until != 0) {
            continue;
        }
        for (size_t k = 0; k < sn->in.n_meds; k++) {
            if (sn->in.meds[k].id == op->med_id) {
                return sn->in.meds[k].code;
            }
        }
        return "";
    }
    return "";
}

/* "Next: A at 14:00" for the power-off confirmation; false when the clock is invalid or nothing is scheduled */
bool idle_next_text(char *out, size_t len)
{
    const hy_clock_port_t *ck = hy_clock_port();
    if (!ck->valid(NULL)) {
        return false;
    }
    const hy_snapshot_t *sn = hy_sched_snapshot();
    const char *code = "";
    int64_t nx = sn->tracking && hy_sched_generation() ? next_slot(sn, &code) : 0;
    if (!nx) {
        return false;
    }
    char tz[HY_TZ_LEN], hhmm[8];
    hy_clock_tz_copy(tz, sizeof tz);
    ui_fmt_hhmm(nx, tz, hhmm, sizeof hhmm);
    snprintf(out, len, HY_S_IDLE_NEXT, code, hhmm);
    return true;
}

void idle_status_refresh(void)
{
    if (!s_status) {
        return;
    }
    char pct[16], est[16], b[48];
    bool red, bolt, has_est, known;
    ui_battery_text(pct, sizeof pct, &red, &bolt, est, sizeof est, &has_est, &known);
    snprintf(b, sizeof b, HY_S_IDLE_STATUS, pct);
    lv_label_set_text(s_status, b);
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
    } else if (sn->tracking && sn->out.n_showing == 1) {
        /* a dose is open (on battery its card does not hold the screen): name it, not a future slot */
        snprintf(b, sizeof b, HY_S_IDLE_DUE_ONE, open_code(sn));
    } else if (sn->tracking && sn->out.n_showing > 1) {
        snprintf(b, sizeof b, HY_S_IDLE_DUE_MORE, open_code(sn), (unsigned)(sn->out.n_showing - 1));
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

/* ---- views ---- */

static void overlay_show(void)
{
    if (s_active) {
        return;
    }
    s_active = true;
    render();
    lv_obj_set_hidden(s_ov, false);   /* created after the top bar, before the power-off overlay: between them */
}

static void overlay_hide(void)
{
    if (!s_active) {
        return;
    }
    s_active = false;
    lv_obj_set_hidden(s_ov, true);
}

static int64_t now_ms64(void)
{
    return esp_timer_get_time() / 1000;
}

static void apply(hy_scr_t prev, hy_scr_t st, const char *why)
{
    s_shown = st;
    hy_power_screen_state(st == HY_SCR_ON ? 0 : st == HY_SCR_DIM ? 1 : 2);
    ui_nav_set_idle(st != HY_SCR_ON);
    if (prev == HY_SCR_OFF) {
        hy_power_screen_off(false);   /* returns with LVGL running */
        hy_board_display_power(true);
        lv_obj_invalidate(lv_screen_active());   /* the whole display: every layer redraws */
        ESP_LOGI(TAG, "display: on");
    }
    if (st != HY_SCR_OFF) {
        rotate_screen_state(st);   /* a no-op while the motion task is already on */
    }
    int32_t set = hy_settings()->brightness;
    switch (st) {
    case HY_SCR_ON:
        overlay_hide();
        lv_display_trigger_activity(NULL);
        bri_to(s_bri, set);
        ESP_LOGI(TAG, "screen: wake (%s) at %lld ms", why, (long long)now_ms64());
        ui_battery_toast_flush();   /* a low-battery toast held while the screen was not on (R13) */
        break;
    case HY_SCR_DIM: {
        overlay_show();
        int32_t dim = set < IDLE_DIM_MAX ? set : IDLE_DIM_MAX;
        ESP_LOGI(TAG, "screen: dim (%s), idle clock, brightness %d -> %d", why, (int)set, (int)dim);
        bri_to(set, dim);
        break;
    }
    case HY_SCR_OFF:
        overlay_show();
        lv_anim_delete(&s_bri, bri_exec);
        ESP_LOGI(TAG, "screen: off (%s)", why);
        ESP_LOGI(TAG, "display: off");
        hy_board_display_power(false);
        rotate_screen_state(HY_SCR_OFF);   /* before the sleep request: the IMU is off by the time the CPU may sleep */
        if (!poweroff_countdown_visible()) {
            hy_power_screen_off(true);
        }
        break;
    }
}

static bool hold_now(void)
{
    return poweroff_visible() || poweroff_countdown_visible() || (due_visible() && s_scr.usb);
}

static void event(hy_scr_ev_t ev, const char *why)
{
    static const char *const names[] = {"touch", "key", "pwr", "due", "chime", "usb in", "usb out", "tick"};
    if (!s_inited) {
        return;
    }
    uint32_t now = (uint32_t)now_ms64();
    hy_screen_set_hold(&s_scr, hold_now(), now);
    const hy_settings_t *st = hy_settings();
    hy_scr_cfg_t cfg = {.dim_s = st->dim_s, .off_bat_s = st->off_bat_s, .off_usb_s = st->off_usb_s};
    hy_scr_t prev = s_shown;
    hy_scr_t now_st = hy_screen_event(&s_scr, ev, now, &cfg);
    if (now_st != prev) {
        apply(prev, now_st, why ? why : names[ev]);
    }
}

void screen_init(void)
{
    hy_power_state_t ps;
    hy_power_state(&ps);
    bool usb = ps.pmu_ok ? ps.batt.raw.vbus : true;   /* unknown before the first reading: never off too early */
    hy_screen_init(&s_scr, usb, (uint32_t)now_ms64());
    s_shown = HY_SCR_ON;
    s_chime_gen = hy_sched_generation();
    s_inited = true;
    hy_power_screen_state(0);
    ESP_LOGI(TAG, "screen: init (usb %d)", usb);
}

void screen_event(hy_scr_ev_t ev)
{
    event(ev, NULL);
}

hy_scr_t screen_state(void)
{
    return s_shown;
}

void screen_wake(const char *why)
{
    event(HY_SE_DUE, why);   /* the same wake as a newly showing due card, on USB and on battery */
}

/* LVGL task, every second: USB from the PMU state (a dropped callback cannot leave the timers on the wrong power
 * source), the dim and off timers, and the sleep request while OFF */
void screen_tick(void)
{
    if (!s_inited) {
        return;
    }
    if (!s_nousb) {
        hy_power_state_t ps;
        hy_power_state(&ps);
        if (ps.pmu_ok && ps.batt.raw.vbus != s_scr.usb) {
            event(ps.batt.raw.vbus ? HY_SE_USB_IN : HY_SE_USB_OUT, NULL);
        }
    }
    event(HY_SE_TICK, NULL);
    if (s_shown == HY_SCR_OFF && !poweroff_countdown_visible()) {
        hy_power_screen_off(true);   /* the power task cleared the request when it woke; the screen stays OFF */
    }
}

/* After each scheduler evaluation: a chime moment (also one silenced by DND or sound off) */
void screen_sched_evaluated(void)
{
    uint32_t gen = hy_sched_generation();
    if (gen == s_chime_gen) {
        return;
    }
    s_chime_gen = gen;
    const hy_snapshot_t *sn = hy_sched_snapshot();
    if (!sn->tracking || !sn->out.chime) {
        return;
    }
    if (!s_scr.usb) {
        hy_power_chime_on_battery();
    }
    event(HY_SE_CHIME, NULL);
}

void screen_usb(bool in)
{
    if (!s_nousb) {
        event(in ? HY_SE_USB_IN : HY_SE_USB_OUT, NULL);
    }
}

/* A touch woke the CPU from the sleep mode: LVGL reads the stroke after this runs, so the stroke that starts within
 * GUARD_MS still counts as waking */
void screen_touch_wake(void)
{
    s_guard = true;
    s_guard_t0 = lv_tick_get();
    event(HY_SE_TOUCH, "touch");
}

bool screen_wake_guard_take(void)
{
    bool g = s_guard && lv_tick_elaps(s_guard_t0) < GUARD_MS;
    s_guard = false;
    return g;
}

/* A stroke pressed or released on an awake screen: restarts the timers. A stroke on DIM or OFF wakes through the
 * navigation (WAKE, idle_wake) on its release. */
void screen_activity(void)
{
    if (s_shown == HY_SCR_ON) {
        event(HY_SE_TOUCH, NULL);
    }
}

void screen_pwr_short(void)
{
    ESP_LOGI(TAG, "pwr short: screen %s", s_shown == HY_SCR_OFF ? "on" : "off");
    event(HY_SE_PWR_SHORT, "pwr");
}

void idle_wake(void)
{
    event(HY_SE_TOUCH, "touch");
}

void idle_wake_due(void)
{
    event(HY_SE_DUE, "due");
}

/* debug `idle`: as if dim_s passed without a touch */
void idle_enter(void)
{
    if (s_shown != HY_SCR_ON) {
        return;
    }
    s_scr.activity_ms = (uint32_t)now_ms64() - (uint32_t)hy_settings()->dim_s * 1000u;
    event(HY_SE_TICK, "debug");
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_active && s_shown == HY_SCR_DIM) {
        const hy_clock_port_t *ck = hy_clock_port();
        int64_t minute = (ck->valid(NULL) ? ck->now(NULL) : (int64_t)(lv_tick_get() / 1000)) / 60;
        if (minute != s_minute) {
            render();
        }
    }
}

#if CONFIG_HY_DEBUG_CONSOLE
/* `scr`: state and timers; `scr off`: OFF now; `scr nousb`: the state machine runs as on battery (the power task still
 * sees VBUS: LVGL pauses, no light sleep); `scr usb`: back to the PMU's USB state */
void screen_debug(const char *arg)
{
    static const char *const st[] = {"ON", "DIM", "OFF"};
    if (strcmp(arg, "off") == 0) {
        if (s_shown != HY_SCR_OFF) {
            event(HY_SE_PWR_SHORT, "debug");
        }
    } else if (strcmp(arg, "nousb") == 0) {
        s_nousb = true;
        event(HY_SE_USB_OUT, "debug");
    } else if (strcmp(arg, "usb") == 0) {
        s_nousb = false;
        screen_tick();
    } else if (arg[0]) {
        ESP_LOGE(TAG, "usage: scr [off|nousb|usb]");
        return;
    }
    const hy_settings_t *se = hy_settings();
    uint32_t idle_s = ((uint32_t)now_ms64() - s_scr.activity_ms) / 1000u;
    ESP_LOGI(TAG, "scr: %s, usb %d%s, hold %d, idle %u s (dim %u s, off battery %u s, off usb %u s)", st[s_shown],
             s_scr.usb, s_nousb ? " (nousb)" : "", s_scr.hold, (unsigned)idle_s, (unsigned)se->dim_s,
             (unsigned)se->off_bat_s, (unsigned)se->off_usb_s);
}
#endif

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
    s_status = ui_label(s_grp, hy_type.status, HY_C_TEXT2, "");
    idle_status_refresh();
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 110);
    s_bri = hy_settings()->brightness;
    lv_timer_create(tick_cb, 500, NULL);
}
