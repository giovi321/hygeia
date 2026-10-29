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

static hy_sched_t s_sched;
static hy_snapshot_t s_snap;
static hy_log_t *s_logs;                /* LOG_CAP entries, PSRAM */
static hy_med_t *s_meds;                /* HY_MAX_MEDS entries, PSRAM: a stable copy for the snapshot */
static hy_snooze_t s_snoozes[HY_MAX_MEDS];
static char s_tz[HY_TZ_LEN];
static hy_tzchange_t s_change;

static bool s_inited, s_dirty, s_tracking, s_invalid_published, s_cap_warned;
static int64_t s_last_minute = -1;
static uint32_t s_seq, s_gen;
static uint32_t s_query_us;

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

esp_err_t hy_sched_run_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_logs = heap_caps_malloc(LOG_CAP * sizeof(hy_log_t), MALLOC_CAP_SPIRAM);
    s_meds = heap_caps_malloc(HY_MAX_MEDS * sizeof(hy_med_t), MALLOC_CAP_SPIRAM);
    if (!s_logs || !s_meds) {
        ESP_LOGE(TAG, "no PSRAM for the log buffer (%u bytes)", (unsigned)(LOG_CAP * sizeof(hy_log_t)));
        heap_caps_free(s_logs);
        heap_caps_free(s_meds);
        s_logs = NULL;
        s_meds = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (hy_ev_subscribe(event_cb, NULL) < 0) {
        ESP_LOGE(TAG, "event subscribe failed");
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

/* Every log the scheduler and the tiles can look at: [now - 8 days - max early window, now + 1), widened for
 * an as-needed minimum interval longer than that. One query for all medicines; when it fills the buffer
 * (possibly truncated), one query per active medicine, merged and sorted. */
static size_t load_logs(int64_t now, const hy_med_t *ms, size_t nm)
{
    int64_t max_early = 0, from;
    int64_t as_span = 0;
    for (size_t i = 0; i < nm; i++) {
        if (ms[i].archived) {
            continue;
        }
        if (ms[i].type == HY_SCHED_FIXED) {
            int64_t e = (int64_t)ms[i].early_window_min * 60;
            max_early = e > max_early ? e : max_early;
        } else {
            int64_t iv = (int64_t)ms[i].min_interval_min * 60;
            as_span = iv > as_span ? iv : as_span;
        }
    }
    from = now - LOOKBACK_S - max_early;
    if (now - as_span < from) {
        from = now - as_span;
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
    for (size_t i = 0; i < nm && n < LOG_CAP; i++) {
        if (!ms[i].archived) {
            n += st->query_log(NULL, from, now + 1, ms[i].id, s_logs + n, LOG_CAP - n);
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
    size_t nl = load_logs(now, s_meds, nm);
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

    bool flash = false;
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
                flash = true;
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
    s_snap.flash = flash;
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
    int64_t jump = 0;
    uint32_t seq = hy_clock_sync_seq(&jump);
    bool synced = seq != s_seq;
    s_seq = seq;

    const hy_clock_port_t *ck = hy_clock_port();
    if (!ck->valid(NULL)) {
        if (s_tracking || !s_invalid_published) {
            hy_sched_stop_tracking(&s_sched);
            s_tracking = false;
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
    bool wake_due = s_snap.out.next_wake && now >= s_snap.out.next_wake;
    if (!(s_dirty || !s_tracking || synced || alarm || wake_due || now / 60 != s_last_minute)) {
        return;
    }

    if (!s_tracking) {
        hy_sched_start_tracking(&s_sched, now);
        s_tracking = true;
        ESP_LOGI(TAG, "tracking starts");
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

void hy_sched_run_invalidate(void)
{
    s_dirty = true;
}

uint32_t hy_sched_run_query_us(void)
{
    return s_query_us;
}
