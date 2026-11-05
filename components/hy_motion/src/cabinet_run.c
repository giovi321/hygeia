/* Cabinet open runner. Everything that touches the IMU runs in the motion task (motion_run.c calls the
   hy_cab_int_* functions); the public setters only change shared state under s_cmux and notify the task. The
   template and the recent captures live in PSRAM under s_mu, which also serialises the template file (its
   buffer in hy_store is single-threaded by contract).
   Screen off and live: wake on motion is armed and the task blocks with no timeout; an interrupt either checks
   the reaction at once (Any movement) or captures accel and gyro until the movement ends and runs the matcher
   (Taught movement). The sleep mode ends only for a reaction (hy_power_motion_wake). Screen on or dimmed: the
   same trigger runs in software on the 50 Hz stream, where teach and test mode also run. */
#include "hy/cabinet_run.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cabinet_int.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hy/board.h"
#include "hy/cabinet_file.h"
#include "hy/cabretry.h"
#include "hy/cabteach.h"
#include "hy/clock.h"
#include "hy/power_run.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "lvgl.h"

static const char *TAG = "cabinet";

#define SAMPLE_MS        20
#define POST_LOCK_MS     1000
#define IO_FAILS_MAX     3
#define STILL_WAIT_MS    10000   /* after a capture: at most this long for stillness before re-arming */
#define RETRY_MS         5000    /* then this long with the IMU off before the next try */
#define EVQ_N            8

/* ---- shared state, under s_cmux ---- */
static portMUX_TYPE s_cmux = portMUX_INITIALIZER_UNLOCKED;
typedef struct { bool on, chime, light; uint8_t mode, sens; } cab_cfg_t;
static cab_cfg_t s_cfg = {false, true, true, HY_CAB_ANY, 3};
static bool s_fault;               /* an IMU fault since the last config change or retry: the feature is off */
static bool s_taught;              /* s_model.valid, for readers that do not take s_mu */
static int64_t s_taught_at;
static uint16_t s_trig_mg;
static float s_d_self, s_thr;
static bool s_teaching;            /* hy_cab_run_teach(true) in force; the motion task clears it at the end */
static bool s_checking;            /* hy_cab_run_check(true) in force, likewise */
static uint32_t s_teach_seq;       /* bumped by every teach or check start */
static bool s_testing;
static int64_t s_test_until_ms;
static hy_cab_status_t s_ctr;      /* counters only: wom_irq .. test_others */
static hy_cabev_t s_evq[EVQ_N];
static uint32_t s_evseq;
static volatile bool s_irq;        /* set by the interrupt hook */
static hy_cab_ui_t s_ui;

/* ---- PSRAM, under s_mu ---- */
static SemaphoreHandle_t s_mu;
static StaticSemaphore_t s_mu_buf;
static HY_EXT_BSS hy_cab_model_t s_model;
static HY_EXT_BSS hy_cab_recent_t s_recent[HY_CAB_RECENT_N];
static uint8_t s_rec_n, s_rec_w;

/* ---- motion task only ---- */
static HY_EXT_BSS hy_cab_cap_t s_cap;
static HY_EXT_BSS hy_cab_seq_t s_takes[HY_CAB_TAKES];
static HY_EXT_BSS hy_cab_model_t s_tmodel;
static int s_ioerr;
static hy_cabretry_t s_retry;     /* retries of a paused feature at screen wakes; the motion task only */
static int64_t s_last_react_ms = INT64_MIN / 2;   /* under s_cmux: hy_cab_run_react_undo writes it from the LVGL task */
static int64_t s_prev_react_ms = INT64_MIN / 2;   /* the value before the last reaction, for the undo */

/* Screen-off path */
static struct {
    bool armed, imu_on, need_still;
    uint16_t thr;
    uint8_t mode;
    hy_accel_t rest;
    int64_t wait_until;   /* holdoff or retry: nothing is armed before it; 0 none */
} s_sl;

/* Stream path */
enum { CAP_LIVE = 0, CAP_TEST, CAP_TEACH };
static struct {
    bool capturing, repower, have_rest, holdoff, tactive;
    uint8_t cap_for;
    hy_cab_still_t still;
    hy_accel_t rest;
    hy_accel_t hold;      /* the trigger sample: the accel held through the capture start */
    hy_accel_t hist[3];   /* the last three samples, [0] the newest */
    uint8_t hist_n;
    uint32_t holdoff_until;
    /* teach and check */
    hy_ct_t ct;
    uint32_t teach_seen;
    hy_accel_t trest;
} s_st;

static int64_t mono_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool live_locked(const cab_cfg_t *c)
{
    return c->on && (c->chime || c->light) && (c->mode == HY_CAB_ANY || s_taught) && !s_fault;
}

static bool cfg_get(cab_cfg_t *c)
{
    portENTER_CRITICAL(&s_cmux);
    *c = s_cfg;
    bool live = live_locked(c);
    portEXIT_CRITICAL(&s_cmux);
    return live;
}

/* ---- events to the LVGL task ---- */

static const char *ev_name(hy_cabev_kind_t k)
{
    static const char *const n[] = {"react", "match", "other", "too small", "too long", "trigger", "teach still",
                                    "teach go", "teach take", "teach odd", "teach done", "teach fail", "fault", "test end",
                                    "teach ready", "check end"};
    return (unsigned)k < sizeof n / sizeof n[0] ? n[k] : "?";
}

/* LVGL task */
static void ev_cb(void *p)
{
    uint32_t seq = (uint32_t)(uintptr_t)p;
    portENTER_CRITICAL(&s_cmux);
    bool lost = s_evseq - seq >= EVQ_N;   /* overwritten by later events */
    hy_cabev_t e = s_evq[seq % EVQ_N];
    void (*fn)(const hy_cabev_t *) = s_ui.event;
    portEXIT_CRITICAL(&s_cmux);
    if (!lost && fn) {
        fn(&e);
    }
}

/* Motion task. Queues the event for the LVGL task under the bounded LVGL lock; dropped without a handler. */
static void post_ev(hy_cabev_kind_t kind, uint8_t take, float dist, float thr)
{
    if ((kind >= HY_CABEV_TEACH_STILL && kind <= HY_CABEV_TEACH_FAIL) || kind == HY_CABEV_TEACH_READY ||
        kind == HY_CABEV_CHECK_END) {
        ESP_LOGI(TAG, "cabinet: %s %u", ev_name(kind), (unsigned)take);
    }
    portENTER_CRITICAL(&s_cmux);
    bool have = s_ui.event != NULL;
    uint32_t seq = s_evseq;
    if (have) {
        s_evq[seq % EVQ_N] = (hy_cabev_t){kind, take, dist, thr};
        s_evseq = seq + 1;
    }
    portEXIT_CRITICAL(&s_cmux);
    if (!have || !hy_board_display()) {
        return;
    }
    if (!hy_board_lock(POST_LOCK_MS)) {
        ESP_LOGE(TAG, "cabinet: post %s: lvgl lock timeout", ev_name(kind));
        return;
    }
    if (lv_async_call(ev_cb, (void *)(uintptr_t)seq) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "cabinet: post %s: lv_async_call failed", ev_name(kind));
    }
    hy_board_unlock();
}

/* ---- shared helpers ---- */

/* Any task except the motion task's match (it holds s_mu): the threshold at the current sensitivity */
static void thr_cache(void)
{
    portENTER_CRITICAL(&s_cmux);
    uint8_t sens = s_cfg.sens;
    portEXIT_CRITICAL(&s_cmux);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    float t = s_model.valid ? hy_cab_threshold(&s_model, sens) : 0.0f;
    xSemaphoreGive(s_mu);
    portENTER_CRITICAL(&s_cmux);
    s_thr = t;
    portEXIT_CRITICAL(&s_cmux);
}

static void IRAM_ATTR int_hook(void)
{
    portENTER_CRITICAL_ISR(&s_cmux);
    s_irq = true;
    portEXIT_CRITICAL_ISR(&s_cmux);
    hy_motion_int_wake_isr();
}

static bool irq_take(void)
{
    portENTER_CRITICAL(&s_cmux);
    bool v = s_irq;
    s_irq = false;
    portEXIT_CRITICAL(&s_cmux);
    return v;
}

/* ---- motion task: teach end, faults, reaction ---- */

static void teach_end(void)
{
    s_st.tactive = false;
    portENTER_CRITICAL(&s_cmux);
    if (s_teach_seq == s_st.teach_seen) {
        s_teaching = false;
        s_checking = false;
    }
    portEXIT_CRITICAL(&s_cmux);
}

/* The sequencer's abort event for the run in force; the run ends */
static void teach_abort(void)
{
    hy_ct_out_t o = hy_ct_abort(&s_st.ct);
    post_ev(o.ev, o.take, 0, 0);
    teach_end();
}

static void fault(const char *what, esp_err_t err)
{
    portENTER_CRITICAL(&s_cmux);
    bool was = s_fault;
    s_fault = true;
    bool testing = s_testing;
    bool teaching = s_teaching;
    bool checking = s_checking;
    s_testing = false;
    s_teaching = false;
    s_checking = false;
    portEXIT_CRITICAL(&s_cmux);
    if (s_st.tactive) {
        teach_abort();
    } else if (teaching) {
        post_ev(HY_CABEV_TEACH_FAIL, (uint8_t)HY_CAB_FAIL_ABORT, 0, 0);   /* the first stream sample never came */
    } else if (checking) {
        post_ev(HY_CABEV_CHECK_END, (uint8_t)HY_CAB_CHECK_ABORT, 0, 0);
    }
    if (testing) {
        ESP_LOGI(TAG, "cabinet: test off");
    }
    if (!was) {
        ESP_LOGE(TAG, "cabinet: imu fault, feature paused (%s: %s)", what, esp_err_to_name(err));
        post_ev(HY_CABEV_FAULT, 0, 0, 0);
    }
}

/* Three I2C errors in a row are a fault */
static void io_result(esp_err_t err, const char *what)
{
    if (err == ESP_OK) {
        s_ioerr = 0;
        return;
    }
    if (++s_ioerr >= IO_FAILS_MAX) {
        s_ioerr = 0;
        fault(what, err);
    }
}

/* Screen wake (motion task, once per wake, before the IMU is powered for the stream). A paused feature is tried
 * again: the sensor may have been wedged only for a moment (a stuck CTRL9 handshake, see qmi8658.c). Bounded by
 * hy_cabretry (at most HY_CABRETRY_PER_HOUR an hour, HY_CABRETRY_GAP_MS apart); each decision is logged. A settings
 * change still clears the fault at once (hy_cab_run_config). */
void hy_cab_int_wake(void)
{
    portENTER_CRITICAL(&s_cmux);
    bool paused = s_fault;
    portEXIT_CRITICAL(&s_cmux);
    if (!paused) {
        return;
    }
    int64_t now = mono_ms(), wait = 0;
    if (!hy_cabretry_wake(&s_retry, now, &wait)) {
        ESP_LOGI(TAG, "cabinet: imu fault, no retry at this wake (next in %lld s, at most %d an hour)",
                 (long long)((wait + 999) / 1000), HY_CABRETRY_PER_HOUR);
        return;
    }
    esp_err_t ierr = hy_board_imu_init();   /* a no-op while the driver is ready; finds and resets a lost sensor */
    ESP_LOGW(TAG, "cabinet: imu fault, retrying at the screen wake (attempt %u of %d this hour, sensor init %s)",
             (unsigned)hy_cabretry_count(&s_retry, now), HY_CABRETRY_PER_HOUR, esp_err_to_name(ierr));
    s_ioerr = 0;
    portENTER_CRITICAL(&s_cmux);
    s_fault = false;
    portEXIT_CRITICAL(&s_cmux);
}

static bool faulted(void)
{
    portENTER_CRITICAL(&s_cmux);
    bool f = s_fault;
    portEXIT_CRITICAL(&s_cmux);
    return f;
}

/* A match (Taught) or a trigger (Any) outside test mode: reacts when the clock is valid, a dose wants attention
   now, a switch is on and no reaction came in the last HY_CAB_REACT_COOLDOWN_S. The LVGL task re-checks before
   acting. */
static void react_check(float dist, float thr)
{
    cab_cfg_t c;
    cfg_get(&c);
    int64_t att = 0, mono = mono_ms();
    const char *why = NULL;
    if (!hy_clock_valid()) {
        why = "clock not valid";
    } else if ((att = hy_sched_run_attention_from()) == 0 || (int64_t)time(NULL) < att) {
        why = "not due";
    } else if (!c.chime && !c.light) {
        why = "no switch";
    } else {
        portENTER_CRITICAL(&s_cmux);
        bool cool = mono - s_last_react_ms < (int64_t)HY_CAB_REACT_COOLDOWN_S * 1000;
        portEXIT_CRITICAL(&s_cmux);
        if (cool) {
            why = "cooldown";
        }
    }
    if (why) {
        ESP_LOGI(TAG, "cabinet: no reaction (%s)", why);
        return;
    }
    portENTER_CRITICAL(&s_cmux);
    s_prev_react_ms = s_last_react_ms;
    s_last_react_ms = mono;
    s_ctr.reactions++;
    portEXIT_CRITICAL(&s_cmux);
    ESP_LOGI(TAG, "cabinet: react (attention since %lld)", (long long)att);
    post_ev(HY_CABEV_REACT, 0, dist, thr);   /* queued first: LVGL runs it in its first pass after the wake */
    hy_power_motion_wake();
}

/* The screen state lives in the LVGL task and the motion task only knows "on or dimmed" (hy_motion_int_on), so the
   runner cannot tell a lit screen from a dimmed one without a race. The UI declines a reaction while the screen is ON
   and calls this to give the pause and the counter back, as if the reaction had not come. Only one reaction can be
   pending (the pause blocks a second), so restoring the value before it is exact. */
void hy_cab_run_react_undo(void)
{
    portENTER_CRITICAL(&s_cmux);
    s_last_react_ms = s_prev_react_ms;
    if (s_ctr.reactions > 0) {
        s_ctr.reactions--;
    }
    portEXIT_CRITICAL(&s_cmux);
}

/* A finished Taught capture in s_cap (live or test): match, recent ring, counters, then the reaction or the
   test count */
static void capture_done(hy_cap_res_t res, bool test)
{
    cab_cfg_t c;
    cfg_get(&c);
    uint16_t n = s_cap.seq.n;
    float dist = INFINITY, thr = 0.0f;
    bool match = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_model.valid) {
        thr = hy_cab_threshold(&s_model, c.sens);
        if (res == HY_CAP_DONE) {
            match = hy_cab_match(&s_model, c.sens, &s_cap.seq, &dist);
        }
    }
    hy_cab_recent_t *r = &s_recent[s_rec_w];
    r->kind = test ? HY_CAB_REC_TEST : HY_CAB_REC_LIVE;
    r->at = hy_clock_valid() ? (int64_t)time(NULL) : 0;
    r->dist = dist;
    r->thr = thr;
    r->match = match;
    r->seq = s_cap.seq;
    s_rec_w = (uint8_t)((s_rec_w + 1) % HY_CAB_RECENT_N);
    if (s_rec_n < HY_CAB_RECENT_N) {
        s_rec_n++;
    }
    xSemaphoreGive(s_mu);
    const char *what = res == HY_CAP_TOO_SHORT ? " (too short)" : res == HY_CAP_TOO_LONG ? " (too long)" : "";
    ESP_LOGI(TAG, "cabinet: capture %u samples dist %.3f thr %.3f %s%s%s", (unsigned)n, (double)dist, (double)thr,
             match ? "match" : "other", what, test ? " (test)" : "");
    portENTER_CRITICAL(&s_cmux);
    s_ctr.captures++;
    s_ctr.capture_ms += (uint32_t)n * SAMPLE_MS;
    if (test) {
        if (match) {
            s_ctr.test_matches++;
        } else {
            s_ctr.test_others++;
        }
    } else if (match) {
        s_ctr.matches++;
    } else {
        s_ctr.others++;
    }
    portEXIT_CRITICAL(&s_cmux);
    if (test) {
        post_ev(match ? HY_CABEV_MATCH : HY_CABEV_OTHER, 0, dist, thr);
    } else if (match) {
        react_check(dist, thr);
    }
}

/* ---- screen-off path ---- */

bool hy_cab_int_sleep_live(void)
{
    cab_cfg_t c;
    return cfg_get(&c);
}

bool hy_cab_int_imu_held(void)
{
    return s_sl.imu_on;
}

void hy_cab_int_sleep_end(void)
{
    if (s_sl.imu_on) {
        esp_err_t err = hy_board_imu_wom_disarm();   /* also ends a capture or the accel-only still wait */
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "cabinet: disarm: %s", esp_err_to_name(err));
        }
    }
    memset(&s_sl, 0, sizeof s_sl);
}

/* Accel only until still (true, IMU on), or false: the screen came on, the feature went off, a fault, or no
   stillness within STILL_WAIT_MS (the IMU off and a retry after RETRY_MS). */
static bool sleep_still_wait(void)
{
    esp_err_t err = hy_board_imu_power(true);
    io_result(err, "power up");
    if (err != ESP_OK) {
        s_sl.imu_on = false;
        s_sl.wait_until = mono_ms() + RETRY_MS;
        return false;
    }
    s_sl.imu_on = true;
    hy_cab_still_t st;
    hy_cab_still_reset(&st);
    int64_t end = mono_ms() + STILL_WAIT_MS;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_MS));
        if (hy_motion_int_off_end() || !hy_cab_int_sleep_live()) {
            return false;
        }
        int16_t mg[3];
        err = hy_board_imu_read(mg);
        io_result(err, "read");
        if (faulted()) {
            return false;
        }
        if (err == ESP_OK && hy_cab_still_feed(&st, (uint32_t)mono_ms(), (hy_accel_t){mg[0], mg[1], mg[2]}, NULL)) {
            return true;
        }
        if (mono_ms() >= end) {
            hy_board_imu_power(false);
            s_sl.imu_on = false;
            s_sl.wait_until = mono_ms() + RETRY_MS;
            ESP_LOGI(TAG, "cabinet: not still after %d s, retry in %d s", STILL_WAIT_MS / 1000, RETRY_MS / 1000);
            return false;
        }
    }
}

/* Taught movement after an interrupt: accel and gyro every 20 ms until the capture ends */
static void sleep_capture(void)
{
    s_sl.need_still = true;   /* whatever happens below, the next arm waits for stillness first */
    /* The trigger sample: one reading from the armed low-power stream, taken before the switch. It starts the
       capture, is its sample 0 and is the value held through the start span, as on the stream path. The rest
       vector stands in when the read fails. */
    int16_t t[3];
    hy_accel_t trig = s_sl.rest;
    bool trig_ok = hy_board_imu_read(t) == ESP_OK;
    if (trig_ok) {
        trig = (hy_accel_t){t[0], t[1], t[2]};
    }
    uint32_t t0 = (uint32_t)mono_ms();
    ESP_LOGI(TAG, "cabinet: trigger read %s, minus rest %d %d %d mg", trig_ok ? "ok" : "failed, rest used",
             trig.x - s_sl.rest.x, trig.y - s_sl.rest.y, trig.z - s_sl.rest.z);
    esp_err_t err = hy_board_imu_capture_on();
    io_result(err, "capture on");
    if (err != ESP_OK) {
        s_sl.imu_on = false;   /* left powered down */
        return;
    }
    hy_cab_cap_start(&s_cap, s_sl.rest, true, t0);
    hy_imu6_t first = {{trig.x, trig.y, trig.z}, {0, 0, 0}};   /* accel only; the gyro start is masked */
    hy_cab_cap_feed(&s_cap, t0, &first);
    TickType_t last = xTaskGetTickCount();
    hy_cap_res_t res = HY_CAP_MORE;
    while (res == HY_CAP_MORE) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_MS));
        if (hy_motion_int_off_end() || !hy_cab_int_sleep_live()) {
            ESP_LOGI(TAG, "cabinet: capture dropped");
            return;
        }
        int16_t a[3], g[3];
        err = hy_board_imu_read6(a, g);
        io_result(err, "read6");
        if (faulted()) {
            return;
        }
        if (err != ESP_OK) {
            continue;
        }
        hy_imu6_t x = {{a[0], a[1], a[2]}, {g[0], g[1], g[2]}};
        res = hy_cab_cap_feed_held(&s_cap, (uint32_t)mono_ms(), &x, trig);   /* the trigger sample, held through the start span */
    }
    capture_done(res, false);
}

void hy_cab_int_sleep_step(void)
{
    cab_cfg_t c;
    cfg_get(&c);
    int64_t now = mono_ms();
    if (s_sl.wait_until && now < s_sl.wait_until) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_sl.wait_until - now) + 1);   /* the IMU is off meanwhile */
        return;
    }
    s_sl.wait_until = 0;
    if (s_sl.need_still) {
        if (!sleep_still_wait()) {
            return;
        }
        s_sl.need_still = false;
    }
    portENTER_CRITICAL(&s_cmux);
    uint16_t trig = s_trig_mg;
    portEXIT_CRITICAL(&s_cmux);
    uint16_t thr = c.mode == HY_CAB_TAUGHT ? trig : hy_cab_wom_mg(c.sens);
    if (thr == 0) {
        /* forgotten since the live check: block until a setter wakes the task, which then sees the feature off */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        return;
    }
    if (!s_sl.armed || s_sl.thr != thr || s_sl.mode != c.mode) {
        irq_take();
        int16_t r[3];
        esp_err_t err = hy_board_imu_wom_arm(thr, r);
        s_sl.armed = false;
        s_sl.imu_on = err == ESP_OK;   /* every failure leaves the IMU powered down */
        if (err == ESP_ERR_TIMEOUT) {
            /* no settled reading: the device is moving, not a fault */
            ESP_LOGD(TAG, "cabinet: moving, arm in %d s", RETRY_MS / 1000);
            s_sl.wait_until = now + RETRY_MS;
            return;
        }
        if (err != ESP_OK) {
            fault("wom arm", err);
            return;
        }
        s_ioerr = 0;
        s_sl.armed = true;
        s_sl.thr = thr;
        s_sl.mode = c.mode;
        s_sl.rest = (hy_accel_t){r[0], r[1], r[2]};
        ESP_LOGI(TAG, "cabinet: armed %s %u mg", c.mode == HY_CAB_TAUGHT ? "taught" : "any", (unsigned)thr);
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* no timeout: the interrupt, a setter or the screen wakes it */
    if (!irq_take()) {
        return;
    }
    portENTER_CRITICAL(&s_cmux);
    s_ctr.wom_irq++;
    portEXIT_CRITICAL(&s_cmux);
    ESP_LOGI(TAG, "cabinet: trigger");
    s_sl.armed = false;
    if (s_sl.mode == HY_CAB_ANY) {
        esp_err_t err = hy_board_imu_wom_disarm();
        s_sl.imu_on = err != ESP_OK;   /* a failed disarm is retried by sleep_end */
        io_result(err, "disarm");
        react_check(0, 0);
        s_sl.wait_until = mono_ms() + HY_CAB_ANY_HOLDOFF_MS;
        return;
    }
    sleep_capture();
}

/* ---- stream path ---- */

static void stream_reset(void)
{
    hy_cab_still_reset(&s_st.still);
    s_st.have_rest = false;
    s_st.hist_n = 0;
}

/* Back to the accel-only stream after a capture or a failed switch; a failure makes the motion task power up */
static void stream_accel(void)
{
    esp_err_t err = hy_board_imu_power(true);
    io_result(err, "power up");
    if (err != ESP_OK) {
        s_st.repower = true;
    }
}

static void capture_abort(void)
{
    if (s_st.capturing) {
        s_st.capturing = false;
        stream_accel();
        ESP_LOGI(TAG, "cabinet: capture dropped");
    }
}

static void capture_begin(uint32_t t, hy_accel_t a, uint8_t for_what, hy_accel_t rest)
{
    esp_err_t err = hy_board_imu_capture_on();
    io_result(err, "capture on");
    if (err != ESP_OK) {
        stream_accel();   /* capture_on left the IMU powered down */
        stream_reset();
        return;
    }
    hy_cab_cap_start(&s_cap, rest, true, t);
    hy_imu6_t x = {{a.x, a.y, a.z}, {0, 0, 0}};   /* the trigger sample, accel only; the gyro start is masked */
    hy_cab_cap_feed(&s_cap, t, &x);
    s_st.capturing = true;
    s_st.cap_for = for_what;
    s_st.hold = a;
}

void hy_cab_int_screen_off(void)
{
    portENTER_CRITICAL(&s_cmux);
    bool testing = s_testing;
    bool teaching = s_teaching, checking = s_checking;
    s_testing = false;
    s_teaching = false;
    s_checking = false;
    portEXIT_CRITICAL(&s_cmux);
    if (s_st.tactive) {
        teach_abort();
    } else if (teaching) {
        post_ev(HY_CABEV_TEACH_FAIL, (uint8_t)HY_CAB_FAIL_ABORT, 0, 0);   /* the first stream sample never came */
    } else if (checking) {
        post_ev(HY_CABEV_CHECK_END, (uint8_t)HY_CAB_CHECK_ABORT, 0, 0);
    }
    if (testing) {
        ESP_LOGI(TAG, "cabinet: test off (screen off)");
        post_ev(HY_CABEV_TEST_END, 0, 0, 0);
    }
    s_st.capturing = false;   /* the motion task powers the IMU down or the screen-off path takes it over */
    s_st.repower = false;
    s_st.holdoff = false;
    stream_reset();
}

void hy_cab_int_screen_on(void)
{
    memset(&s_sl, 0, sizeof s_sl);   /* the stream's power up leaves wake on motion */
    s_st.capturing = false;
    s_st.repower = false;
    s_st.holdoff = false;
    stream_reset();
}

bool hy_cab_int_stream_gyro(void)
{
    return s_st.capturing;
}

bool hy_cab_int_stream_repower(void)
{
    bool r = s_st.repower;
    s_st.repower = false;
    if (r) {
        s_st.capturing = false;
        stream_reset();
    }
    return r;
}

void hy_cab_int_stream_error(esp_err_t err)
{
    cab_cfg_t c;
    bool live = cfg_get(&c);
    portENTER_CRITICAL(&s_cmux);
    bool active = live || s_testing || s_teaching || s_checking;
    portEXIT_CRITICAL(&s_cmux);
    if (active && err == ESP_ERR_INVALID_STATE) {
        fault("imu absent", err);   /* no IMU at all cannot recover: end a teach or test at once */
    } else if (active) {
        io_result(err, s_st.capturing ? "read6" : "read");
    }
}

static void tc_apply(hy_ct_out_t o, uint32_t t, hy_accel_t a);

static void teach_save(uint32_t t, hy_accel_t a)
{
    int64_t created = hy_clock_valid() ? (int64_t)time(NULL) : 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    hy_err_t e = hy_cab_file_save(&s_tmodel, created);
    if (e == HY_OK) {
        s_model = s_tmodel;
    }
    xSemaphoreGive(s_mu);
    if (e != HY_OK) {
        ESP_LOGE(TAG, "cabinet: template save failed: %s", hy_err_str(e));
        tc_apply(hy_ct_saved(&s_st.ct, false), t, a);
        return;
    }
    portENTER_CRITICAL(&s_cmux);
    s_taught = true;
    s_taught_at = created;
    s_trig_mg = s_tmodel.trig_mg;
    s_d_self = s_tmodel.d_self;
    portEXIT_CRITICAL(&s_cmux);
    thr_cache();
    ESP_LOGI(TAG, "cabinet: taught, %u takes, d_self %.3f trigger %u mg", (unsigned)s_tmodel.n, (double)s_tmodel.d_self,
             (unsigned)s_tmodel.trig_mg);
    tc_apply(hy_ct_saved(&s_st.ct, true), t, a);
}

static void teach_build(uint32_t t, hy_accel_t a)
{
    int odd = -1;
    hy_err_t e = hy_cab_model_build(&s_tmodel, s_takes, &odd);
    hy_ct_out_t o = hy_ct_build_result(&s_st.ct, t, e == HY_OK, odd, isfinite(s_tmodel.d_self));
    if (o.has_ev && o.ev == HY_CABEV_TEACH_ODD) {
        ESP_LOGI(TAG, "cabinet: take %d differs (d_self %.3f), redo %u of %d", odd + 1, (double)s_tmodel.d_self,
                 (unsigned)s_st.ct.redos, HY_CT_REDOS);
    } else if (o.accept_as_is) {
        ESP_LOGI(TAG, "cabinet: take %d still differs, accepted as is", odd + 1);
    } else if (!o.save) {
        ESP_LOGW(TAG, "cabinet: no usable template (%s, d_self %.3f)", hy_err_str(e), (double)s_tmodel.d_self);
    }
    tc_apply(o, t, a);
}

/* The match of a finished check capture; ends the run */
static void check_match(void)
{
    cab_cfg_t c;
    cfg_get(&c);
    float dist = INFINITY, thr = 0.0f;
    bool match = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_model.valid) {
        thr = hy_cab_threshold(&s_model, c.sens);
        match = hy_cab_match(&s_model, c.sens, &s_cap.seq, &dist);
    }
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "cabinet: check %u samples dist %.3f thr %.3f %s", (unsigned)s_cap.seq.n, (double)dist, (double)thr,
             match ? "match" : "other");
    hy_ct_out_t o = hy_ct_check_result(&s_st.ct, match);
    post_ev(o.ev, o.take, dist, thr);
    teach_end();
}

/* Acts on what the sequencer returned, in the order documented in hy/cabteach.h */
static void tc_apply(hy_ct_out_t o, uint32_t t, hy_accel_t a)
{
    if (o.has_ev) {
        post_ev(o.ev, o.take, o.ev == HY_CABEV_TEACH_DONE ? s_tmodel.d_self : 0.0f, 0.0f);
    }
    if (o.reset_still) {
        stream_reset();
    }
    if (o.start_capture) {
        capture_begin(t, a, CAP_TEACH, s_st.trest);
        if (!s_st.capturing) {
            tc_apply(hy_ct_capture_failed(&s_st.ct, t), t, a);   /* the switch failed: try the take again */
        }
    }
    if (o.store && s_st.ct.mode == HY_CT_TEACH) {
        s_takes[o.slot] = s_cap.seq;
    }
    if (o.build) {
        teach_build(t, a);
    }
    if (o.save) {
        if (o.accept_as_is) {
            s_tmodel.valid = true;
        }
        teach_save(t, a);
    }
    if (o.match) {
        check_match();
    }
    if (o.end) {
        teach_end();
    }
}

static void teach_begin(uint32_t t, bool check)
{
    capture_abort();
    s_st.tactive = true;
    stream_reset();
    ESP_LOGI(TAG, "cabinet: %s start", check ? "check" : "teach");
    tc_apply(hy_ct_begin(&s_st.ct, check ? HY_CT_CHECK : HY_CT_TEACH, t), t, (hy_accel_t){0, 0, 0});
}

/* One stream sample while teaching or checking and not capturing */
static void teach_step(uint32_t t, hy_accel_t a, bool still, hy_accel_t rest, bool hit)
{
    hy_ct_out_t o = hy_ct_step(&s_st.ct, t, still, hit);
    if (o.take_rest) {
        s_st.trest = rest;
    }
    tc_apply(o, t, a);
}

void hy_cab_int_stream_sample(uint32_t t, hy_accel_t a, const int16_t *g, bool suspended)
{
    s_ioerr = 0;
    cab_cfg_t c;
    bool live = cfg_get(&c);
    int64_t mono = mono_ms();
    portENTER_CRITICAL(&s_cmux);
    bool teaching = s_teaching, checking = s_checking;
    uint32_t tseq = s_teach_seq;
    bool test_end = s_testing && mono >= s_test_until_ms;
    if (test_end) {
        s_testing = false;
    }
    bool testing = s_testing;
    portEXIT_CRITICAL(&s_cmux);
    if (test_end) {
        ESP_LOGI(TAG, "cabinet: test off (time)");
        post_ev(HY_CABEV_TEST_END, 0, 0, 0);
    }
    if ((teaching || checking) && tseq != s_st.teach_seen) {
        s_st.teach_seen = tseq;
        teach_begin(t, checking);
    } else if (!teaching && !checking && s_st.tactive) {
        s_st.tactive = false;   /* cancelled by the UI */
        capture_abort();
        ESP_LOGI(TAG, "cabinet: %s cancelled", s_st.ct.mode == HY_CT_CHECK ? "check" : "teach");
    }
    hy_cab_stream_plan_t plan = hy_cab_stream_plan(suspended, s_st.tactive, testing, live);
    if (!plan.run) {
        /* Suspended: the Undo toast shows, no trigger; the still tracker starts again when it closes */
        if (plan.abort_teach) {
            capture_abort();
            teach_abort();
        }
        if (plan.abort_capture) {
            capture_abort();
        }
        if (plan.reset) {
            stream_reset();
        }
        return;
    }
    if (s_st.capturing) {
        hy_imu6_t x = {{a.x, a.y, a.z}, {g ? g[0] : 0, g ? g[1] : 0, g ? g[2] : 0}};
        hy_cap_res_t res = hy_cab_cap_feed_held(&s_cap, t, &x, s_st.hold);
        if (res == HY_CAP_MORE) {
            return;
        }
        s_st.capturing = false;
        stream_accel();
        stream_reset();
        if (s_st.cap_for == CAP_TEACH) {
            if (s_st.tactive) {
                tc_apply(hy_ct_capture_done(&s_st.ct, t, res), t, a);
            }
        } else {
            capture_done(res, s_st.cap_for == CAP_TEST);
        }
        return;
    }

    hy_accel_t r = {0, 0, 0};
    bool still = hy_cab_still_feed(&s_st.still, t, a, &r);
    if (still) {
        s_st.rest = r;
        s_st.have_rest = true;
    }
    portENTER_CRITICAL(&s_cmux);
    uint16_t trig = s_trig_mg;
    portEXIT_CRITICAL(&s_cmux);
    /* A check uses the trigger in use (the model's), so a recognised movement is one the device would act on */
    uint16_t thr = s_st.tactive ? (s_st.ct.mode == HY_CT_CHECK ? trig : HY_CAB_TEACH_TRIG_MG)
                                : c.mode == HY_CAB_TAUGHT ? trig : hy_cab_wom_mg(c.sens);
    bool hit = s_st.hist_n >= 3 && hy_cab_wom_hit(hy_cab_wom_prev(s_st.hist[1], s_st.hist[2]), a, thr);
    s_st.hist[2] = s_st.hist[1];
    s_st.hist[1] = s_st.hist[0];
    s_st.hist[0] = a;
    if (s_st.hist_n < 3) {
        s_st.hist_n++;
    }

    if (s_st.tactive) {
        teach_step(t, a, still, r, hit);
        return;
    }
    if (s_st.holdoff && (int32_t)(t - s_st.holdoff_until) < 0) {
        return;
    }
    s_st.holdoff = false;
    if (!hit || !s_st.have_rest) {
        return;
    }
    ESP_LOGI(TAG, "cabinet: trigger%s", testing ? " (test)" : "");
    if (testing) {
        post_ev(HY_CABEV_TRIGGER, 0, 0, 0);
    }
    if (c.mode == HY_CAB_ANY) {
        if (testing) {
            portENTER_CRITICAL(&s_cmux);
            s_ctr.test_matches++;
            portEXIT_CRITICAL(&s_cmux);
            post_ev(HY_CABEV_MATCH, 0, 0, 0);
        } else {
            react_check(0, 0);
        }
        s_st.holdoff = true;
        s_st.holdoff_until = t + HY_CAB_ANY_HOLDOFF_MS;
        return;
    }
    capture_begin(t, a, testing ? CAP_TEST : CAP_LIVE, s_st.rest);
}

bool hy_cab_int_rot_locked(void)
{
    portENTER_CRITICAL(&s_cmux);
    bool l = s_teaching || s_checking || s_testing;
    portEXIT_CRITICAL(&s_cmux);
    return l;
}

/* ---- public ---- */

esp_err_t hy_cab_run_init(const hy_cab_ui_t *ui)
{
    if (s_mu) {
        return ESP_ERR_INVALID_STATE;
    }
    s_mu = xSemaphoreCreateMutexStatic(&s_mu_buf);
    if (ui) {
        s_ui = *ui;
    }
    int64_t created = 0;
    hy_err_t e = hy_cab_file_load(&s_model, &created);
    if (e == HY_OK) {
        portENTER_CRITICAL(&s_cmux);
        s_taught = true;
        s_taught_at = created;
        s_trig_mg = s_model.trig_mg;
        s_d_self = s_model.d_self;
        portEXIT_CRITICAL(&s_cmux);
        ESP_LOGI(TAG, "cabinet: template loaded (created %lld, d_self %.3f, trigger %u mg)", (long long)created,
                 (double)s_model.d_self, (unsigned)s_model.trig_mg);
    } else {
        s_model.valid = false;
        if (e == HY_E_CORRUPT || e == HY_E_VERSION) {
            ESP_LOGW(TAG, "cabinet: template unreadable (%s), not taught", e == HY_E_CORRUPT ? "corrupt" : "version");
        } else if (e != HY_E_NOT_FOUND) {
            ESP_LOGW(TAG, "cabinet: template not loaded (%s), not taught", hy_err_str(e));
        }
    }
    thr_cache();
    hy_board_imu_int_hook(int_hook);
    return ESP_OK;
}

void hy_cab_run_config(const hy_settings_t *s)
{
    cab_cfg_t n = {s->cab_on, s->cab_chime, s->cab_light, s->cab_mode == HY_CAB_TAUGHT ? HY_CAB_TAUGHT : HY_CAB_ANY,
                   s->cab_sens};
    portENTER_CRITICAL(&s_cmux);
    bool changed = memcmp(&n, &s_cfg, sizeof n) != 0;
    if (changed) {
        s_cfg = n;
        s_fault = false;
    }
    portEXIT_CRITICAL(&s_cmux);
    if (!changed) {
        return;
    }
    if (s_mu) {
        thr_cache();
    }
    ESP_LOGI(TAG, "cabinet: config on %d mode %s chime %d light %d sensitivity %u", n.on,
             n.mode == HY_CAB_TAUGHT ? "taught" : "any", n.chime, n.light, (unsigned)n.sens);
    hy_motion_int_wake();
}

esp_err_t hy_cab_run_teach(bool start)
{
    bool on = hy_motion_int_on();   /* outside s_cmux: it takes the motion task's lock */
    portENTER_CRITICAL(&s_cmux);
    bool ok = !start || (!s_fault && on);
    bool test_off = false;
    if (ok && start) {
        s_teaching = true;
        s_checking = false;
        s_teach_seq++;
        test_off = s_testing;
        s_testing = false;
    } else if (!start) {
        s_teaching = false;
    }
    portEXIT_CRITICAL(&s_cmux);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }
    if (test_off) {
        post_ev(HY_CABEV_TEST_END, 0, 0, 0);   /* the test the teach replaced ends like any other test */
    }
    hy_motion_int_wake();
    return ESP_OK;
}

esp_err_t hy_cab_run_check(bool start)
{
    bool screen = hy_motion_int_on();   /* outside s_cmux: it takes the motion task's lock */
    portENTER_CRITICAL(&s_cmux);
    bool ok = !start || (!s_fault && !s_teaching && !s_testing && screen && s_taught);
    if (ok && start) {
        s_checking = true;
        s_teach_seq++;
    } else if (!start) {
        s_checking = false;
    }
    portEXIT_CRITICAL(&s_cmux);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }
    hy_motion_int_wake();
    return ESP_OK;
}

esp_err_t hy_cab_run_test(bool on)
{
    bool screen = hy_motion_int_on();   /* outside s_cmux: it takes the motion task's lock */
    portENTER_CRITICAL(&s_cmux);
    bool ok = !on || (!s_fault && !s_teaching && !s_checking && screen && (s_cfg.mode == HY_CAB_ANY || s_taught));
    bool was = s_testing;
    if (ok) {
        s_testing = on;
        if (on) {
            s_test_until_ms = mono_ms() + (int64_t)HY_CAB_TEST_MAX_S * 1000;
            s_ctr.test_matches = 0;
            s_ctr.test_others = 0;
        }
    }
    portEXIT_CRITICAL(&s_cmux);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }
    if (was != on) {
        ESP_LOGI(TAG, "cabinet: test %s", on ? "on" : "off");
        if (was && !on) {
            post_ev(HY_CABEV_TEST_END, 0, 0, 0);   /* a web or console Stop updates the device view */
        }
    }
    hy_motion_int_wake();
    return ESP_OK;
}

esp_err_t hy_cab_run_forget(void)
{
    if (!s_mu) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    hy_err_t e = hy_cab_file_delete();
    s_model.valid = false;
    xSemaphoreGive(s_mu);
    portENTER_CRITICAL(&s_cmux);
    s_taught = false;
    s_taught_at = 0;
    s_trig_mg = 0;
    s_d_self = 0;
    s_thr = 0;
    bool test_off = false;
    if (s_cfg.mode == HY_CAB_TAUGHT) {
        test_off = s_testing;
        s_testing = false;
    }
    portEXIT_CRITICAL(&s_cmux);
    if (test_off) {
        post_ev(HY_CABEV_TEST_END, 0, 0, 0);
    }
    ESP_LOGI(TAG, "cabinet: template forgotten (%s)", hy_err_str(e));
    hy_motion_int_wake();
    return e == HY_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t hy_cab_run_forget_apply(void)
{
    esp_err_t e = hy_cab_run_forget();
    if (hy_settings()->cab_mode == HY_CAB_TAUGHT) {
        hy_settings_t n = *hy_settings();
        n.cab_mode = HY_CAB_ANY;
        esp_err_t se = hy_settings_update(&n);
        hy_cab_run_config(hy_settings());
        ESP_LOGI(TAG, "cabinet: trigger set back to any movement: %s", esp_err_to_name(se));
    }
    return e;
}

void hy_cab_run_status(hy_cab_status_t *out)
{
    portENTER_CRITICAL(&s_cmux);
    *out = s_ctr;
    out->taught = s_taught;
    out->taught_at = s_taught_at;
    out->mode = s_cfg.mode;
    out->live = live_locked(&s_cfg);
    out->testing = s_testing;
    out->teaching = s_teaching;
    out->checking = s_checking;
    out->d_self = s_d_self;
    out->thr = s_thr;
    out->trig_mg = s_trig_mg;
    portEXIT_CRITICAL(&s_cmux);
}

size_t hy_cab_run_snapshot(hy_cab_model_t *model_out, hy_cab_recent_t *recent_out, size_t max_recent)
{
    if (!s_mu) {
        if (model_out) {
            model_out->valid = false;
        }
        return 0;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (model_out) {
        *model_out = s_model;
    }
    size_t n = 0;
    if (recent_out) {
        n = s_rec_n < max_recent ? s_rec_n : max_recent;
        size_t first = (size_t)(s_rec_w + HY_CAB_RECENT_N - s_rec_n) % HY_CAB_RECENT_N;
        for (size_t i = 0; i < n; i++) {
            recent_out[i] = s_recent[(first + i) % HY_CAB_RECENT_N];
        }
    }
    xSemaphoreGive(s_mu);
    return n;
}

#if CONFIG_HY_DEBUG_CONSOLE
void hy_cab_run_debug(const char *args)
{
    esp_err_t err;
    const char *what;
    if (strncmp(args, "teach stop", 10) == 0) {
        what = "teach stop";
        err = hy_cab_run_teach(false);
    } else if (strncmp(args, "teach", 5) == 0) {
        what = "teach";
        err = hy_cab_run_teach(true);
    } else if (strncmp(args, "check stop", 10) == 0) {
        what = "check stop";
        err = hy_cab_run_check(false);
    } else if (strncmp(args, "check", 5) == 0) {
        what = "check";
        err = hy_cab_run_check(true);
    } else if (strncmp(args, "test on", 7) == 0) {
        what = "test on";
        err = hy_cab_run_test(true);
    } else if (strncmp(args, "test off", 8) == 0) {
        what = "test off";
        err = hy_cab_run_test(false);
    } else if (strncmp(args, "forget", 6) == 0) {
        what = "forget";
        err = hy_cab_run_forget();
    } else {
        printf("usage: cab teach [stop] | cab check [stop] | cab test on|off | cab forget (teach, check and test need the screen on)\n");
        return;
    }
    printf("cab %s: %s\n", what, esp_err_to_name(err));
}

void hy_cab_run_debug_stat(void)
{
    hy_cab_status_t s;
    hy_cab_run_status(&s);
    printf("cabstat: taught %d at %lld mode %s live %d testing %d teaching %d checking %d d_self %.3f thr %.3f trigger %u mg\n",
           s.taught, (long long)s.taught_at, s.mode == HY_CAB_TAUGHT ? "taught" : "any", s.live, s.testing,
           s.teaching, s.checking, (double)s.d_self, (double)s.thr, (unsigned)s.trig_mg);
    printf("cabstat: wom irq %lu captures %lu matches %lu others %lu reactions %lu capture ms %lu test matches %lu "
           "test others %lu\n", (unsigned long)s.wom_irq, (unsigned long)s.captures, (unsigned long)s.matches,
           (unsigned long)s.others, (unsigned long)s.reactions, (unsigned long)s.capture_ms,
           (unsigned long)s.test_matches, (unsigned long)s.test_others);
}
#endif
