#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hy/actions.h"
#include "hy/audio.h"
#include "hy/clock.h"
#include "hy/events.h"
#include "hy/gap.h"
#include "hy/occ.h"
#include "hy/power_run.h"
#include "hy/sched_run.h"
#include "hy/store.h"

static const char *TAG = "sched";

#define LOG_CAP 4096
#define DAY_S 86400
#define LOOKBACK_S (8 * DAY_S)
#define JUMP_RESTART_S 120
#define UNSYNCED_SKIP_MAX 5     /* ticks skipped for an unannounced clock step before it counts as a jump */

static hy_sched_t s_sched;
static hy_snapshot_t s_snap;
static hy_log_t *s_logs;                /* LOG_CAP entries, PSRAM */
static hy_med_t *s_meds;                /* HY_MAX_MEDS entries, PSRAM: a stable copy for the snapshot */
static hy_snooze_t s_snoozes[HY_MAX_MEDS];
static char s_tz[HY_TZ_LEN];
static hy_tzchange_t s_change;

static bool s_inited, s_dirty, s_tracking, s_invalid_published, s_cap_warned, s_flash;
static int64_t s_last_minute = -1;
static uint32_t s_seq, s_gen;
static uint32_t s_query_us;
/* Monotonic cross-check: UTC and esp_timer at the last tick that passed it (ref_us 0 = none) */
static int64_t s_ref_utc, s_ref_us;
static unsigned s_unsynced_skips;

/* Untracked gaps. s_alive: the record the next tracking start measures its gap from (alive 0 = none): the
 * NVS heartbeat as it was at init (read before the power task starts rewriting it), then the last valid time
 * whenever the clock turns invalid while tracking. Consumed by the start that uses it, so a gap is recorded
 * once. s_last_gap_end: end of the last gap recorded this boot; the gap file stays in end order. */
static hy_alive_t s_alive;
static bool s_clock_was_invalid;
static int64_t s_last_gap_end;
/* A forward jump's gap, held HY_GAP_CONFIRM_S before it is recorded (ruling R12). RAM only. */
static hy_gap_pending_t s_pend;

/* out.next_wake of the last evaluation, 0 when none or not tracking; read by the power task */
static portMUX_TYPE s_wake_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_next_wake;

static void set_next_wake(int64_t t)
{
    portENTER_CRITICAL(&s_wake_mux);
    s_next_wake = t;
    portEXIT_CRITICAL(&s_wake_mux);
}

int64_t hy_sched_run_next_wake(void)
{
    portENTER_CRITICAL(&s_wake_mux);
    int64_t t = s_next_wake;
    portEXIT_CRITICAL(&s_wake_mux);
    return t;
}

/* Actions run in the LVGL task, so this runs there too. Our own DUE events need no new evaluation. */
static void event_cb(const hy_event_t *ev, void *ctx)
{
    (void)ctx;
    switch (ev->type) {
    case HY_EV_DUE:
    case HY_EV_BATTERY_LOW:
    case HY_EV_BATTERY_CRITICAL:
        break;
    default:
        s_dirty = true;
        break;
    }
}

static void free_buffers(void)
{
    heap_caps_free(s_logs);
    heap_caps_free(s_meds);
    s_logs = NULL;
    s_meds = NULL;
}

esp_err_t hy_sched_run_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_logs = heap_caps_malloc(LOG_CAP * sizeof(hy_log_t), MALLOC_CAP_SPIRAM);
    s_meds = heap_caps_malloc(HY_MAX_MEDS * sizeof(hy_med_t), MALLOC_CAP_SPIRAM);
    if (!s_logs || !s_meds) {
        ESP_LOGE(TAG, "no PSRAM for the log buffer (%u bytes)", (unsigned)(LOG_CAP * sizeof(hy_log_t)));
        free_buffers();
        return ESP_ERR_NO_MEM;
    }
    if (hy_ev_subscribe(event_cb, NULL) < 0) {
        ESP_LOGE(TAG, "event subscribe failed");
        free_buffers();
        return ESP_FAIL;
    }
    hy_sched_init(&s_sched);
    /* before hy_power_start: the power task's first pass overwrites the heartbeat */
    if (!hy_power_alive_load(&s_alive)) {
        s_alive.alive = 0;   /* no heartbeat yet (first boot): nothing to measure a gap from */
    }
    memset(&s_snap, 0, sizeof s_snap);
    s_snap.sched = &s_sched;
    s_seq = hy_clock_sync_seq(NULL);
    s_dirty = true;
    s_inited = true;
    ESP_LOGI(TAG, "runner ready, log buffer %u entries (%u bytes PSRAM)", (unsigned)LOG_CAP,
             (unsigned)(LOG_CAP * sizeof(hy_log_t)));
    return ESP_OK;
}

static int cmp_log(const void *a, const void *b)
{
    const hy_log_t *x = a, *y = b;
    if (x->ts != y->ts) {
        return x->ts < y->ts ? -1 : 1;
    }
    return x->id < y->id ? -1 : x->id > y->id;
}

/* The window one medicine needs: a fixed one 8 days plus its early window (scheduler and tile), an as-needed
 * one max(1 day, its minimum interval) (tile ring and status). */
static int64_t med_from(const hy_med_t *m, int64_t now)
{
    if (m->type == HY_SCHED_FIXED) {
        return now - LOOKBACK_S - (int64_t)m->early_window_min * 60;
    }
    int64_t iv = (int64_t)m->min_interval_min * 60;
    return now - (iv > DAY_S ? iv : DAY_S);
}

/* Every log the scheduler and the tiles can look at, ascending by ts: one query for all medicines over the
 * union of their windows. When it fills the buffer (possibly truncated), one query per active medicine over
 * its own window, merged and sorted. *truncated: the per-medicine queries still filled the buffer, so some
 * medicine may be missing logs. */
static size_t load_logs(int64_t now, const hy_med_t *ms, size_t nm, bool *truncated)
{
    *truncated = false;
    int64_t from = now - LOOKBACK_S;
    for (size_t i = 0; i < nm; i++) {
        if (!ms[i].archived) {
            int64_t f = med_from(&ms[i], now);
            from = f < from ? f : from;
        }
    }
    const hy_store_port_t *st = hy_store_port();
    size_t n = st->query_log(NULL, from, now + 1, 0, s_logs, LOG_CAP);
    if (n < LOG_CAP) {
        return n;
    }
    if (!s_cap_warned) {
        ESP_LOGW(TAG, "log query filled %u entries: falling back to per-medicine queries", (unsigned)LOG_CAP);
        s_cap_warned = true;
    }
    n = 0;
    for (size_t i = 0; i < nm; i++) {
        if (ms[i].archived) {
            continue;
        }
        size_t room = LOG_CAP - n;
        size_t got = room ? st->query_log(NULL, med_from(&ms[i], now), now + 1, ms[i].id, s_logs + n, room) : 0;
        n += got;
        if (got == room) {
            *truncated = true;   /* full: this medicine, or the next ones, may be cut short */
        }
    }
    qsort(s_logs, n, sizeof s_logs[0], cmp_log);
    return n;
}

static void fmt_local(int64_t t, char *buf, size_t len)
{
    char d[32];
    hy_format_local(t, s_tz, d, sizeof d);      /* "Tue 27 Oct 22:04"; zone offsets are whole minutes */
    snprintf(buf, len, "%s:%02d", d, (int)(((t % 60) + 60) % 60));
}

static void fmt_wake(int64_t t, char *buf, size_t len)
{
    if (!t) {
        snprintf(buf, len, "none");
        return;
    }
    fmt_local(t, buf, len);
}

/* Stores the gap and publishes its one HY_EV_UNTRACKED (hy_act_record_gap). */
static void record_gap(const hy_gap_t *g, hy_gap_reason_t why)
{
    char a[40], b[40];
    hy_clock_tz_copy(s_tz, sizeof s_tz);
    fmt_local(g->start, a, sizeof a);
    fmt_local(g->end, b, sizeof b);
    if (g->end <= s_last_gap_end) {
        /* only after a backward clock step this boot: the span lies in time already passed */
        ESP_LOGW(TAG, "gap: %s .. %s (%s) ends before the last recorded gap, not recorded", a, b,
                 hy_gap_reason_str(why));
        return;
    }
    hy_err_t err = hy_act_record_gap(g, why);
    if (err != HY_OK) {
        ESP_LOGE(TAG, "gap: %s .. %s (%s) not recorded: %s", a, b, hy_gap_reason_str(why), hy_err_str(err));
        return;
    }
    s_last_gap_end = g->end;
    ESP_LOGI(TAG, "gap: %s .. %s (%s)", a, b, hy_gap_reason_str(why));
}

/* Tracking starts at now (boot, or the clock valid again): the gap since the stored heartbeat, then a fresh
 * heartbeat at once, so a crash within the first minute still has a recent record. */
static void gap_on_start(int64_t now)
{
    hy_alive_t a = s_alive;
    bool was_invalid = s_clock_was_invalid;
    s_alive.alive = 0;
    s_clock_was_invalid = false;
    hy_gap_t g;
    hy_gap_reason_t why;
    if (a.alive > now) {
        ESP_LOGW(TAG, "gap: heartbeat ahead of clock by %lld s", (long long)(a.alive - now));
    } else if (hy_gap_on_start(&a, now, was_invalid, &g, &why)) {
        record_gap(&g, why);
    }
    hy_power_alive_write(now, false);
    hy_power_alive_enable(true);   /* from now on the power task keeps the heartbeat fresh */
}

/* The clock was set back to now: a pending jump gap is clipped to [start, now) or dropped. */
static void pend_on_back(int64_t now)
{
    char a[40], b[40];
    hy_gap_back_t r = hy_gap_pend_back(&s_pend, now);
    if (r == HY_GAP_PEND_CLIPPED) {
        hy_clock_tz_copy(s_tz, sizeof s_tz);
        fmt_local(s_pend.gap.start, a, sizeof a);
        fmt_local(s_pend.gap.end, b, sizeof b);
        ESP_LOGI(TAG, "gap: pending jump clipped to %s .. %s", a, b);
    } else if (r == HY_GAP_PEND_DROPPED) {
        ESP_LOGI(TAG, "gap: pending jump dropped (clock set back)");
    }
}

/* Records the pending jump gap at once (the clock turns invalid). */
static void pend_flush(void)
{
    hy_gap_t g;
    if (hy_gap_pend_take(&s_pend, &g)) {
        record_gap(&g, HY_GAP_JUMP);
    }
}

/* A clock jump from pre to now while tracking. Forward by HY_GAP_MIN_S or more: the gap [pre, now) is held
 * pending; one already pending is recorded first. Backward: the pending gap is clipped or dropped. The
 * heartbeat follows the new clock either way. pre <= 0: no usable time before the jump, no gap. */
static void gap_on_jump(int64_t pre, int64_t now)
{
    if (pre > 0 && now > pre) {
        hy_gap_t f;
        if (hy_gap_pend_jump(&s_pend, pre, now, &f)) {
            record_gap(&f, HY_GAP_JUMP);
        }
        if (now - pre >= HY_GAP_MIN_S) {   /* hy_gap_pend_jump made [pre, now) the pending gap */
            char a[40], b[40];
            hy_clock_tz_copy(s_tz, sizeof s_tz);
            fmt_local(pre, a, sizeof a);
            fmt_local(now, b, sizeof b);
            ESP_LOGI(TAG, "gap: pending jump %s .. %s, recorded in %d s unless the clock is set back", a, b,
                     HY_GAP_CONFIRM_S);
        }
    } else if (now < pre) {
        pend_on_back(now);
    }
    hy_power_alive_write(now, false);
}

static const char *code_of(uint32_t id, size_t nm)
{
    for (size_t i = 0; i < nm; i++) {
        if (s_meds[i].id == id) {
            return s_meds[i].code;
        }
    }
    return "?";
}

/* One evaluation at `now`. pre_jump: the state just before a clock jump; only the missed entries are
 * written (no due events, no chime, no snapshot), so slots already overdue then are not lost. */
static bool s_chime_deferred;

static void play_chime(void)
{
    const hy_settings_t *st = hy_settings();
    if (!st->sound) {
        ESP_LOGI(TAG, "chime silent (sound off)");
    } else if (st->dnd) {
        ESP_LOGI(TAG, "chime silent (do not disturb)");
    } else {
        if (!hy_audio_ok()) {
            /* not silenced, but the speaker failed: the UI flashes; the play is still attempted,
               so a speaker that recovers is heard again */
            s_flash = true;
            ESP_LOGW(TAG, "chime silent (audio unavailable)");
        }
        hy_audio_chime();
    }
}

static void evaluate(int64_t now, bool pre_jump)
{
    int64_t t0 = esp_timer_get_time();
    s_dirty = false;

    const hy_med_t *ms;
    size_t nm = hy_meds(&ms);
    if (nm > HY_MAX_MEDS) {
        nm = HY_MAX_MEDS;
    }
    memcpy(s_meds, ms, nm * sizeof *ms);
    const hy_snooze_t *sz;
    size_t nz = hy_snoozes(&sz);
    if (nz > HY_MAX_MEDS) {
        nz = HY_MAX_MEDS;
    }
    memcpy(s_snoozes, sz, nz * sizeof *sz);
    hy_clock_tz_copy(s_tz, sizeof s_tz);
    bool has_change = hy_clock_last_tzchange(&s_change);

    int64_t tq = esp_timer_get_time();
    bool truncated;
    size_t nl = load_logs(now, s_meds, nm, &truncated);
    uint32_t query_us = (uint32_t)(esp_timer_get_time() - tq);

    hy_sched_in_t in = {
        .meds = s_meds, .n_meds = nm,
        .logs = s_logs, .n_logs = nl,
        .snoozes = s_snoozes, .n_snoozes = nz,
        .tz = s_tz, .last_change = has_change ? &s_change : NULL,
        .now = now,
    };
    static hy_sched_out_t out;
    hy_sched_eval(&s_sched, &in, &out);

    unsigned written = 0;
    if (truncated && out.n_to_miss) {
        /* a medicine's taken entries may be missing from the buffer: a missed entry now could be false */
        ESP_LOGW(TAG, "logs truncated at %u entries: %u missed slot(s) not written this time",
                 (unsigned)LOG_CAP, (unsigned)out.n_to_miss);
    } else {
        for (size_t i = 0; i < out.n_to_miss; i++) {
            uint32_t id = 0;
            hy_err_t err = hy_act_mark_missed(out.to_miss[i].med_id, out.to_miss[i].slot, &id);
            if (err != HY_OK) {
                ESP_LOGW(TAG, "mark_missed med %s slot %lld: %s", code_of(out.to_miss[i].med_id, nm),
                         (long long)out.to_miss[i].slot, hy_err_str(err));
            } else if (id) {
                written++;
                ESP_LOGI(TAG, "missed: med %s slot %lld, log id %u", code_of(out.to_miss[i].med_id, nm),
                         (long long)out.to_miss[i].slot, (unsigned)id);
            }
        }
    }
    if (pre_jump) {
        ESP_LOGI(TAG, "pre-jump eval: open=%u miss=%u", (unsigned)out.n_open, written);
        return;
    }

    for (size_t i = 0; i < out.n_newly_due; i++) {
        hy_event_t ev = {.type = HY_EV_DUE, .ts = now, .med_id = out.newly_due[i].med_id,
                         .slot = out.newly_due[i].slot};
        ESP_LOGI(TAG, "due: med %s slot %lld", code_of(ev.med_id, nm), (long long)ev.slot);
        hy_ev_publish(&ev);
    }

    if (out.n_showing == 0) {
        s_flash = false;   /* nothing left to draw attention to */
    }
    if (out.chime) {
        if (hy_audio_pending()) {
            s_chime_deferred = true;   /* audio starts after the splash: play when it is up, no flash meanwhile */
            ESP_LOGI(TAG, "chime waits for the audio init");
        } else {
            play_chime();
        }
    }

    set_next_wake(out.next_wake);
    esp_err_t aerr = hy_clock_alarm_set(out.next_wake);
    if (aerr != ESP_OK) {
        ESP_LOGW(TAG, "alarm_set %lld: %s", (long long)out.next_wake, esp_err_to_name(aerr));
    }

    s_snap.in = in;
    s_snap.sched = &s_sched;
    s_snap.out = out;
    s_snap.tracking = true;
    s_snap.flash = s_flash;
    s_last_minute = now / 60;
    s_gen++;
    s_invalid_published = false;

    uint32_t total_us = (uint32_t)(esp_timer_get_time() - t0);
    s_query_us = query_us;
    char wake[40];
    fmt_wake(out.next_wake, wake, sizeof wake);
    /* "eval": how long this evaluation took, not the time to next_wake */
    ESP_LOGI(TAG, "open=%u showing=%u new=%u miss=%u chime=%d next_wake=%s (eval %u ms)",
             (unsigned)out.n_open, (unsigned)out.n_showing, (unsigned)out.n_newly_due, written,
             out.chime ? 1 : 0, wake, (unsigned)((total_us + 500) / 1000));
}

void hy_sched_run_tick(void)
{
    if (!s_inited) {
        return;
    }
    if (s_chime_deferred && !hy_audio_pending()) {
        s_chime_deferred = false;
        if (s_snap.out.n_showing > 0) {   /* still something showing: the chime that waited for the audio init */
            bool was = s_flash;
            play_chime();
            if (s_flash != was) {
                s_snap.flash = s_flash;
                s_gen++;   /* the UI reads the flash request from a new generation */
            }
        }
    }
    bool alarm = hy_clock_alarm_take();    /* the only caller: one-shot, re-armed after every evaluation */
    /* The sequence is read before the time: a set landing in between shows as a step without a sync,
     * which the monotonic check below holds back until the sequence catches up. */
    int64_t jump = 0;
    uint32_t seq = hy_clock_sync_seq(&jump);
    bool synced = seq != s_seq;

    const hy_clock_port_t *ck = hy_clock_port();
    if (!ck->valid(NULL)) {
        s_seq = seq;
        pend_flush();   /* a jump gap still pending is recorded before the clock gap that follows it */
        if (s_tracking && s_ref_us) {
            /* the last trusted time: the gap of this invalid period starts there; the power task stops its
             * heartbeat so a crash meanwhile keeps this record */
            hy_power_alive_enable(false);
            s_alive.alive = s_ref_utc;
            s_alive.clean = false;
            hy_power_alive_write(s_ref_utc, false);
        }
        s_clock_was_invalid = true;
        s_ref_us = 0;
        if (s_tracking || !s_invalid_published) {
            hy_sched_stop_tracking(&s_sched);
            s_tracking = false;
            s_flash = false;
            memset(&s_snap, 0, sizeof s_snap);
            s_snap.sched = &s_sched;
            s_last_minute = -1;
            hy_clock_alarm_set(0);
            set_next_wake(0);
            s_gen++;
            s_invalid_published = true;
            ESP_LOGI(TAG, "clock invalid, reminders paused");
        }
        return;
    }

    int64_t now = ck->now(NULL);
    int64_t mono = esp_timer_get_time();
    bool forced_jump = false;
    int64_t expect = 0;
    if (!synced && s_tracking && s_ref_us) {
        expect = s_ref_utc + (mono - s_ref_us) / 1000000;
        int64_t step = now - expect;
        if (llabs(step) > JUMP_RESTART_S) {
            if (++s_unsynced_skips <= UNSYNCED_SKIP_MAX) {
                ESP_LOGW(TAG, "clock moved %+lld s without a sync, tick skipped", (long long)step);
                return;
            }
            /* a step nobody announced (not hy_clock_set): treat it as a jump from now on */
            ESP_LOGW(TAG, "clock moved %+lld s without a sync for %u ticks: treated as a jump",
                     (long long)step, (unsigned)UNSYNCED_SKIP_MAX);
            forced_jump = true;
        }
    }
    s_unsynced_skips = 0;
    s_seq = seq;
    s_ref_utc = now;
    s_ref_us = mono;

    if (synced && jump < 0 && llabs(jump) <= JUMP_RESTART_S) {
        pend_on_back(now);   /* a small step back: no restart, but a pending jump gap may now be too long */
    }
    hy_gap_t confirmed;
    if (hy_gap_pend_due(&s_pend, now, &confirmed)) {
        record_gap(&confirmed, HY_GAP_JUMP);   /* HY_GAP_CONFIRM_S passed without a correction */
    }

    bool wake_due = s_snap.out.next_wake && now >= s_snap.out.next_wake;
    if (!(s_dirty || !s_tracking || synced || forced_jump || alarm || wake_due || now / 60 != s_last_minute)) {
        return;
    }

    if (!s_tracking) {
        gap_on_start(now);
        hy_sched_start_tracking(&s_sched, now);
        s_tracking = true;
        ESP_LOGI(TAG, "tracking starts");
    } else if (forced_jump) {
        gap_on_jump(expect, now);
        hy_sched_start_tracking(&s_sched, now);
        ESP_LOGI(TAG, "tracking restarts");
    } else if (synced && llabs(jump) > JUMP_RESTART_S) {
        /* first the state just before the jump, so slots already overdue then get their missed entries */
        int64_t pre = now - jump;
        if (pre > 0) {
            evaluate(pre, true);   /* returns before set_next_wake and the alarm: only missed entries */
        }
        gap_on_jump(pre, now);     /* guards pre > 0 itself */
        hy_sched_start_tracking(&s_sched, now);
        ESP_LOGI(TAG, "clock jump %+lld s, tracking restarts", (long long)jump);
    }
    evaluate(now, false);
}

const hy_snapshot_t *hy_sched_snapshot(void)
{
    return &s_snap;
}

uint32_t hy_sched_generation(void)
{
    return s_gen;
}

bool hy_sched_take_flash(void)
{
    bool f = s_flash;
    s_flash = false;
    s_snap.flash = false;
    return f;
}

void hy_sched_run_invalidate(void)
{
    s_dirty = true;
}

uint32_t hy_sched_run_query_us(void)
{
    return s_query_us;
}
