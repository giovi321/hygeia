#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hy/actions.h"
#include "hy/audio.h"
#include "hy/clock.h"
#include "hy/events.h"
#include "hy/occ.h"
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

static void fmt_wake(int64_t t, char *buf, size_t len)
{
    if (!t) {
        snprintf(buf, len, "none");
        return;
    }
    char d[32];
    hy_format_local(t, s_tz, d, sizeof d);      /* "Tue 27 Oct 22:04"; zone offsets are whole minutes */
    snprintf(buf, len, "%s:%02d", d, (int)(((t % 60) + 60) % 60));
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
    ESP_LOGI(TAG, "open=%u showing=%u new=%u miss=%u chime=%d next_wake=%s in %u ms",
             (unsigned)out.n_open, (unsigned)out.n_showing, (unsigned)out.n_newly_due, written,
             out.chime ? 1 : 0, wake, (unsigned)((total_us + 500) / 1000));
}

void hy_sched_run_tick(void)
{
    if (!s_inited) {
        return;
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
        s_ref_us = 0;
        if (s_tracking || !s_invalid_published) {
            hy_sched_stop_tracking(&s_sched);
            s_tracking = false;
            s_flash = false;
            memset(&s_snap, 0, sizeof s_snap);
            s_snap.sched = &s_sched;
            s_last_minute = -1;
            hy_clock_alarm_set(0);
            s_gen++;
            s_invalid_published = true;
            ESP_LOGI(TAG, "clock invalid, reminders paused");
        }
        return;
    }

    int64_t now = ck->now(NULL);
    int64_t mono = esp_timer_get_time();
    bool forced_jump = false;
    if (!synced && s_tracking && s_ref_us) {
        int64_t expect = s_ref_utc + (mono - s_ref_us) / 1000000;
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

    bool wake_due = s_snap.out.next_wake && now >= s_snap.out.next_wake;
    if (!(s_dirty || !s_tracking || synced || forced_jump || alarm || wake_due || now / 60 != s_last_minute)) {
        return;
    }

    if (!s_tracking) {
        hy_sched_start_tracking(&s_sched, now);
        s_tracking = true;
        ESP_LOGI(TAG, "tracking starts");
    } else if (forced_jump) {
        hy_sched_start_tracking(&s_sched, now);
        ESP_LOGI(TAG, "tracking restarts");
    } else if (synced && llabs(jump) > JUMP_RESTART_S) {
        /* first the state just before the jump, so slots already overdue then get their missed entries */
        int64_t pre = now - jump;
        if (pre > 0) {
            evaluate(pre, true);
        }
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
