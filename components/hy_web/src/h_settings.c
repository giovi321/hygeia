/* Settings, time, chime and power-off routes. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hy/audio.h"
#include "hy/clock.h"
#include "hy/actions.h"
#include "hy/net.h"
#include "hy/sched_run.h"
#include "hy/summary.h"
#include "hy/tzdb.h"
#include "web_internal.h"

static const char *TAG = "web_settings";

static int64_t utc_ms_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---------------------------------------------------------------- GET, PUT /api/settings */

typedef struct {
    const char *body;
    hy_settings_t now;
    bool failed;
    hy_api_err_t aerr;
    esp_err_t save;
} sctx_t;

static void job_settings_get(void *arg)
{
    sctx_t *c = arg;
    c->now = *hy_settings();
}

static esp_err_t send_settings(web_req_t *r, const hy_settings_t *s)
{
    char *json = hy_api_settings_json(s);
    esp_err_t e = json ? web_send_json(r, 200, json) : web_oom(r);
    free(json);
    return e;
}

static esp_err_t h_settings_get(web_req_t *r)
{
    hy_job_t *j = web_job_new(r, job_settings_get, sizeof(sctx_t));
    esp_err_t sent;
    if (!j) {
        return web_oom(r);
    }
    sctx_t *c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!c) {
        return sent;
    }
    esp_err_t e = send_settings(r, &c->now);
    hy_job_put(j);
    return e;
}

/* Merge, clamp and save, then tell the screen side. The applied rotation is never written from here; the callback
 * (rotation lock, brightness and the views that show a setting) is the device UI's own refresh. */
static void job_settings_put(void *arg)
{
    sctx_t *c = arg;
    hy_settings_t old = *hy_settings();
    hy_settings_t next;
    if (hy_api_settings_from_json(c->body && c->body[0] ? c->body : "{}", &old, &next, &c->aerr) != HY_OK) {
        c->failed = true;
        return;
    }
    next.rot = old.rot;
    c->save = hy_settings_update(&next);   /* the RAM copy is updated even when the save fails */
    c->now = *hy_settings();
    if (c->now.cab_window_min != old.cab_window_min) {
        hy_sched_run_invalidate();   /* the attention instant depends on the window */
    }
    hy_audio_set_volume(c->now.volume);
    hy_net_set_keep_batt(c->now.wifi_keep_batt);
    const hy_web_ui_t *ui = web_ui();
    if (ui && ui->settings_applied) {
        ui->settings_applied(&old, &c->now);
    }
}

static esp_err_t h_settings_put(web_req_t *r)
{
    hy_job_t *j = web_job_new(r, job_settings_put, sizeof(sctx_t));
    esp_err_t sent;
    if (!j) {
        return web_oom(r);
    }
    sctx_t *c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!c) {
        return sent;
    }
    esp_err_t e;
    if (c->failed) {
        e = web_fail_api(r, &c->aerr);
    } else if (c->save != ESP_OK) {
        e = web_fail(r, HY_E_IO);
    } else {
        e = send_settings(r, &c->now);
    }
    hy_job_put(j);
    return e;
}

/* ---------------------------------------------------------------- POST /api/time */

typedef struct {
    const char *body;
    hy_api_time_t t;
    bool set, zone_set;
    int64_t diff_ms;
    esp_err_t err;     /* hy_clock_set, then the zone: the first failure */
} tctx_t;

/* {"utc_ms":..,"auto":true}: set only when more than 2 s off or the clock is invalid.
 * {"utc_ms":..,"zone":"Europe/Zurich"}: manual, always sets, and takes the zone. */
static void job_time(void *arg)
{
    tctx_t *c = arg;
    bool valid = hy_clock_valid();
    int64_t dev = utc_ms_now();
    c->diff_ms = valid ? c->t.utc_ms - dev : 0;
    if (!hy_api_time_should_set(&c->t, dev, valid)) {
        return;
    }
    int64_t jump;
    uint32_t seq = hy_clock_sync_seq(&jump);
    esp_err_t e = hy_clock_set(c->t.utc_ms / 1000, NULL, "browser");
    c->set = hy_clock_sync_seq(&jump) != seq;   /* in force, even when persisting it failed */
    c->err = e;
    if (c->set && c->t.has_zone) {
        esp_err_t z = hy_clock_set_zone(c->t.zone, hy_tzdb_posix(c->t.zone));
        c->zone_set = z == ESP_OK;
        if (z != ESP_OK && c->err == ESP_OK) {
            c->err = z;
        }
    }
    if (c->set) {
        hy_sched_run_invalidate();
    }
    const hy_web_ui_t *ui = web_ui();
    if ((c->set || c->zone_set) && ui && ui->time_changed) {
        ui->time_changed();
    }
}

static esp_err_t h_time(web_req_t *r)
{
    tctx_t t0 = {0};
    hy_api_err_t ae;
    if (hy_api_time_from_json(web_body_or_empty(r), hy_clock_build_epoch(), &t0.t, &ae) != HY_OK) {
        return web_fail_api(r, &ae);
    }
    hy_job_t *j = web_job_new(r, job_time, sizeof(tctx_t));
    esp_err_t sent;
    if (!j) {
        return web_oom(r);
    }
    ((tctx_t *)hy_job_ctx(j))->t = t0.t;
    tctx_t *c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!c) {
        return sent;
    }
    esp_err_t e;
    if (!c->set && c->err == ESP_ERR_INVALID_ARG) {
        e = web_send_err(r, 400, "invalid", "utc_ms", "The time was refused");
    } else if (!c->set && c->err != ESP_OK) {   /* not set at all */
        e = web_send_err(r, 500, "internal", NULL, "The device could not set the time");
    } else if (c->err != ESP_OK) {              /* set, but not saved: the zone or the time source is lost on reboot */
        e = web_send_err(r, 503, "storage", NULL, "The time is set but could not be saved");
    } else {
        char buf[72];
        snprintf(buf, sizeof buf, "{\"set\":%s,\"diff_ms\":%lld,\"zone_set\":%s}", c->set ? "true" : "false",
                 (long long)c->diff_ms, c->zone_set ? "true" : "false");
        e = web_send_json(r, 200, buf);
    }
    hy_job_put(j);
    return e;
}

/* ---------------------------------------------------------------- POST /api/chime/test */

/* The same chime as Settings > Sound > Play chime: plays whatever the Sound and Do not disturb toggles say */
static esp_err_t h_chime(web_req_t *r)
{
    bool ok = hy_audio_ok();
    if (ok) {
        hy_audio_chime();
    }
    return web_send_json(r, 200, ok ? "{\"played\":true}" : "{\"played\":false}");
}

/* ---------------------------------------------------------------- POST /api/power/off */

#define POWER_DELAY_US 1000000

#define POWER_TRIES 10

static esp_timer_handle_t s_power_timer;
static int s_power_tries;

/* httpd task: posting to the LVGL task can wait for the board lock, which the esp_timer task should not. A busy LVGL
 * task (a long job) is retried once a second. */
static void power_work(void *arg)
{
    (void)arg;
    if (web_ui_power_off()) {
        return;
    }
    if (++s_power_tries < POWER_TRIES) {
        esp_timer_start_once(s_power_timer, POWER_DELAY_US);
    } else {
        ESP_LOGE(TAG, "power off not delivered: the LVGL task stayed busy");
    }
}

static void power_fire(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "power off requested from the web UI");
    if (!web_queue_work(power_work, NULL)) {   /* the server is stopping: post from here */
        power_work(NULL);
    }
}

typedef struct {
    const char *body;
    bool due_24h;
} pctx_t;

static void job_power(void *arg)
{
    pctx_t *c = arg;
    if (!hy_clock_valid()) {
        return;
    }
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    char tz[HY_TZ_LEN];
    hy_clock_tz_copy(tz, sizeof tz);
    c->due_24h = hy_due_within(ms, n, tz, (int64_t)time(NULL), 24 * 3600);
}

/* {"confirm":true} powers the device off one second after the answer. Without it: 409 "confirm" with the same
 * "due_24h" flag, so the page can warn first. */
static esp_err_t h_power_off(web_req_t *r)
{
    cJSON *o = cJSON_Parse(web_body_or_empty(r));
    const cJSON *cf = o ? cJSON_GetObjectItemCaseSensitive(o, "confirm") : NULL;
    bool confirmed = cJSON_IsTrue(cf);
    bool bad = !o || !cJSON_IsObject(o);
    cJSON_Delete(o);
    if (bad) {
        return web_send_err(r, 400, "invalid", NULL, "Send {\"confirm\":true}");
    }
    hy_job_t *j = web_job_new(r, job_power, sizeof(pctx_t));
    esp_err_t sent;
    if (!j) {
        return web_oom(r);
    }
    pctx_t *c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!c) {
        return sent;
    }
    bool due = c->due_24h;
    hy_job_put(j);
    char buf[96];
    if (!confirmed) {
        snprintf(buf, sizeof buf,
                 "{\"error\":\"confirm\",\"message\":\"Send {\\\"confirm\\\":true} to power off\",\"due_24h\":%s}",
                 due ? "true" : "false");
        return web_send_json(r, 409, buf);
    }
    snprintf(buf, sizeof buf, "{\"powering_off\":true,\"due_24h\":%s}", due ? "true" : "false");
    esp_err_t e = web_send_json(r, 200, buf);
    if (!s_power_timer) {
        const esp_timer_create_args_t a = {.callback = power_fire, .name = "web_power"};
        if (esp_timer_create(&a, &s_power_timer) != ESP_OK) {
            s_power_timer = NULL;
        }
    }
    if (s_power_timer) {
        s_power_tries = 0;
        esp_timer_stop(s_power_timer);
        esp_timer_start_once(s_power_timer, POWER_DELAY_US);
    } else {
        ESP_LOGE(TAG, "no timer for the power off");
    }
    return e;
}

/* ---------------------------------------------------------------- route table */

web_handler_t web_settings_route(hy_route_id_t id)
{
    switch (id) {
    case HY_R_SETTINGS: return h_settings_get;
    case HY_R_SETTINGS_PUT: return h_settings_put;
    case HY_R_TIME: return h_time;
    case HY_R_CHIME_TEST: return h_chime;
    case HY_R_POWER_OFF: return h_power_off;
    default: return NULL;
    }
}
