/* Core routes: info, state, pairing and paired browsers. */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hy/actions.h"
#include "hy/auth.h"
#include "hy/clock.h"
#include "hy/net.h"
#include "hy/occ.h"
#include "hy/power_run.h"
#include "hy/sched_run.h"
#include "hy/summary.h"
#include "bridge.h"
#include "web_internal.h"

static const char *TAG = "web_core";

/* ---------------------------------------------------------------- pairing state */

static portMUX_TYPE s_pmux = portMUX_INITIALIZER_UNLOCKED;
static hy_pair_t s_pair;
static hy_pair_gate_t s_gate;   /* start gap, cool-down after a Cancel or an unused code, starts per 10 minutes */

static int64_t mono_s(void)
{
    return esp_timer_get_time() / 1000000;
}

void web_pair_init(void)
{
    portENTER_CRITICAL(&s_pmux);
    hy_pair_init(&s_pair);
    hy_pair_gate_init(&s_gate);
    portEXIT_CRITICAL(&s_pmux);
}

bool web_pair_peek(char code[7], int64_t *expires_mono)
{
    portENTER_CRITICAL(&s_pmux);
    bool on = hy_pair_active(&s_pair, mono_s());
    if (on) {
        memcpy(code, s_pair.code, 7);
        *expires_mono = s_pair.expires;
    }
    portEXIT_CRITICAL(&s_pmux);
    return on;
}

void hy_web_pair_cancel(void)
{
    portENTER_CRITICAL(&s_pmux);
    memset(s_pair.code, 0, sizeof s_pair.code);   /* the fail count and any block stay */
    s_pair.expires = 0;
    hy_pair_gate_ended(&s_gate, mono_s(), true);   /* starts wait a minute */
    portEXIT_CRITICAL(&s_pmux);
    ESP_LOGI(TAG, "pairing cancelled on the screen");
}

#if CONFIG_HY_DEBUG_CONSOLE
void hy_web_debug_pair(void)
{
    char code[7];
    int64_t exp;
    portENTER_CRITICAL(&s_pmux);
    uint8_t fails = s_pair.fails;
    int64_t blocked = s_pair.blocked_until;
    portEXIT_CRITICAL(&s_pmux);
    int64_t now = mono_s();
    if (web_pair_peek(code, &exp)) {
        printf("pair: code %s, %lld s left, %u wrong\n", code, (long long)(exp - now), (unsigned)fails);
    } else if (blocked > now) {
        printf("pair: blocked for %lld s\n", (long long)(blocked - now));
    } else {
        printf("pair: no code running, %u wrong\n", (unsigned)fails);
    }
    memset(code, 0, sizeof code);
}
#endif

/* ---------------------------------------------------------------- JSON helpers */

static esp_err_t send_cjson(web_req_t *r, int status, cJSON *o)
{
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    if (!s) {
        return web_send_err(r, 500, "internal", NULL, "Out of memory");
    }
    esp_err_t e = web_send_json(r, status, s);
    cJSON_free(s);
    return e;
}

static int64_t utc_ms_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---------------------------------------------------------------- GET /api/info (open) */

static esp_err_t h_info(web_req_t *r)
{
    hy_net_status_t st;
    hy_net_status(&st);
    cJSON *o = cJSON_CreateObject();
    if (o) {
        cJSON_AddNumberToObject(o, "api", HY_API_VERSION);
        cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
        cJSON_AddStringToObject(o, "name", st.host);
        cJSON_AddBoolToObject(o, "paired", r->sess >= 0);
        cJSON_AddNumberToObject(o, "utc_ms", (double)utc_ms_now());
        cJSON_AddBoolToObject(o, "clock_valid", hy_clock_valid());
    }
    return send_cjson(r, 200, o);
}

/* ---------------------------------------------------------------- GET /api/state */

typedef struct {
    hy_api_state_t st;
    hy_api_tile_t tiles[HY_MAX_MEDS];
    hy_api_due_t due[HY_SCHED_MAX_OPEN];
} state_ctx_t;

static const hy_med_t *snap_med(const hy_snapshot_t *sn, const hy_med_t *live)
{
    for (size_t i = 0; i < sn->in.n_meds; i++) {
        if (sn->in.meds[i].id == live->id) {
            return &sn->in.meds[i];
        }
    }
    return live;
}

/* LVGL task: the scheduler snapshot, the medicines, the clock, the battery and the network in one consistent copy */
static void job_state(void *arg)
{
    state_ctx_t *c = arg;
    hy_api_state_t *s = &c->st;
    const hy_snapshot_t *sn = hy_sched_snapshot();

    s->clock_valid = hy_clock_valid();
    s->utc_ms = s->clock_valid ? utc_ms_now() : 0;
    hy_clock_zone_copy(s->zone, sizeof s->zone);
    hy_clock_last_sync(s->sync_src, sizeof s->sync_src, &s->sync_at);

    hy_power_state_t ps;
    hy_power_state(&ps);
    s->batt_pct = ps.batt.pct;
    s->usb = ps.pmu_ok ? ps.batt.raw.vbus : true;
    s->charging = ps.batt.raw.chg == HY_CHG_CHARGING;
    if (!hy_batt_estimate(&ps.batt, s->estimate, sizeof s->estimate)) {
        s->estimate[0] = 0;
    }

    hy_net_status_t ns;
    hy_net_status(&ns);
    s->net_mode = (uint8_t)ns.saved;
    s->net_state = (uint8_t)ns.state;
    strlcpy(s->ssid, ns.ssid, sizeof s->ssid);
    s->rssi = ns.rssi;
    if (ns.ip) {
        snprintf(s->ip, sizeof s->ip, "%u.%u.%u.%u", (unsigned)(ns.ip & 0xff), (unsigned)((ns.ip >> 8) & 0xff),
                 (unsigned)((ns.ip >> 16) & 0xff), (unsigned)(ns.ip >> 24));
    }
    strlcpy(s->host, ns.host, sizeof s->host);

    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    int64_t now = sn->in.now ? sn->in.now : (int64_t)(s->utc_ms / 1000);
    size_t nt = 0;
    for (size_t i = 0; i < n && nt < HY_MAX_MEDS; i++) {
        if (ms[i].archived) {
            continue;
        }
        hy_api_tile_t *t = &c->tiles[nt++];
        const hy_med_t *m = &ms[i];
        t->id = m->id;
        strlcpy(t->code, m->code, sizeof t->code);
        strlcpy(t->name, m->name, sizeof t->name);
        t->color = m->color;
        t->pills = m->pills_enabled;
        t->pills_left = m->pills_left;
        if (sn->in.tz && sn->in.now) {
            hy_tile_view_t tv;
            hy_tile_view(snap_med(sn, m), &sn->in, sn->sched, &tv);
            t->kind = (uint8_t)tv.kind;
            t->ring = tv.ring;
            t->last_taken = tv.last_taken;
        } else {   /* no evaluation yet (clock invalid): no state to show */
            t->kind = m->type == HY_SCHED_AS_NEEDED ? HY_TILE_AS_NEEDED : HY_TILE_NO_DOSE;
        }
        if (s->clock_valid && m->type == HY_SCHED_FIXED && sn->in.tz) {
            int64_t next;
            if (hy_occurrences(m, now + 1, now + 8 * 86400 + 1, sn->in.tz, &next, 1) == 1) {
                t->next_due = next;
            }
        }
    }
    s->tiles = c->tiles;
    s->n_tiles = nt;

    size_t nd = 0;
    for (size_t i = 0; i < sn->out.n_open && nd < HY_SCHED_MAX_OPEN; i++) {
        hy_api_due_t *d = &c->due[nd++];
        d->med_id = sn->out.open[i].med_id;
        d->slot = sn->out.open[i].slot;
        d->state = (uint8_t)sn->out.open[i].state;
        d->snoozed_until = sn->out.open[i].snoozed_until;
    }
    s->due = c->due;
    s->n_due = nd;
}

static esp_err_t h_state(web_req_t *r)
{
    hy_job_t *j = hy_job_new(job_state, sizeof(state_ctx_t));
    if (!j) {
        return web_send_err(r, 500, "internal", NULL, "Out of memory");
    }
    esp_err_t res = hy_job_run(j, HY_JOB_TIMEOUT_MS);
    esp_err_t e;
    if (res != ESP_OK) {
        e = web_send_busy(r);
    } else {
        char *json = hy_api_state_json(&((state_ctx_t *)hy_job_ctx(j))->st);
        e = json ? web_send_json(r, 200, json) : web_send_err(r, 500, "internal", NULL, "Out of memory");
        cJSON_free(json);
    }
    hy_job_put(j);
    return e;
}

/* ---------------------------------------------------------------- POST /api/pair/start (open, X-Hygeia) */

/* LVGL task: the overlay shows the running code, unless a due card or the battery power-off countdown is on the screen
 * (the overlay never covers a reminder). */
typedef struct {
    bool busy;
} pair_show_ctx_t;

static void job_pair_show(void *ctx)
{
    pair_show_ctx_t *c = ctx;
    const hy_web_ui_t *ui = web_ui();
    if (ui && ui->pair_busy && ui->pair_busy()) {
        c->busy = true;
        return;
    }
    char code[7];
    int64_t exp;
    if (ui && ui->pair_show && web_pair_peek(code, &exp)) {
        ui->pair_show(code, exp);
    }
    memset(code, 0, sizeof code);
}

/* The code just started is withdrawn without a cool-down (the screen could not show it). */
static void pair_withdraw(void)
{
    portENTER_CRITICAL(&s_pmux);
    memset(s_pair.code, 0, sizeof s_pair.code);
    s_pair.expires = 0;
    hy_pair_gate_ended(&s_gate, mono_s(), false);
    portEXIT_CRITICAL(&s_pmux);
}

static esp_err_t h_pair_start(web_req_t *r)
{
    int64_t now = mono_s();
    int64_t wait = 0;
    portENTER_CRITICAL(&s_pmux);
    hy_pair_wait_t why = hy_pair_gate_check(&s_gate, now, &wait);
    hy_err_t e = why != HY_PAIR_GO ? HY_E_FULL : hy_pair_start(&s_pair, esp_random(), now);
    if (e == HY_OK) {
        hy_pair_gate_started(&s_gate, now);
    }
    int64_t blocked = s_pair.blocked_until;
    portEXIT_CRITICAL(&s_pmux);
    char ra[24];
    if (why != HY_PAIR_GO) {   /* a running code stays: another browser on the LAN cannot keep replacing it */
        snprintf(ra, sizeof ra, "%lld", (long long)wait);
        httpd_resp_set_hdr(r->req, "Retry-After", ra);
        const char *msg = why == HY_PAIR_WAIT_GAP ? "Pairing was started a moment ago, use the code on the screen"
                          : why == HY_PAIR_WAIT_COOLDOWN ? "Pairing was cancelled or the code expired, try again in a minute"
                                                         : "Pairing was started too often, try again later";
        return web_send_err(r, 429, "too_many", NULL, msg);
    }
    if (e != HY_OK) {
        snprintf(ra, sizeof ra, "%lld", (long long)(blocked > now ? blocked - now : 1));
        httpd_resp_set_hdr(r->req, "Retry-After", ra);
        return web_send_err(r, 429, "blocked", NULL, "Too many wrong codes, try again in a minute");
    }
    hy_job_t *j = hy_job_new(job_pair_show, sizeof(pair_show_ctx_t));
    if (!j) {
        pair_withdraw();
        return web_send_err(r, 500, "internal", NULL, "Out of memory");
    }
    esp_err_t res = hy_job_run(j, HY_JOB_TIMEOUT_MS);
    bool busy = res == ESP_OK && ((pair_show_ctx_t *)hy_job_ctx(j))->busy;
    hy_job_put(j);
    if (res != ESP_OK) {   /* a job that ran late may have shown the code: the overlay closes */
        pair_withdraw();
        web_ui_pair_hide(HY_WEB_PAIR_CANCEL);
        return web_send_busy(r);
    }
    if (busy) {
        pair_withdraw();
        return web_send_err(r, 409, "busy", NULL, "The device is showing a reminder, try again when it closes");
    }
    ESP_LOGI(TAG, "pairing code shown on the screen for %d s", HY_PAIR_TTL_S);
    char buf[48];
    snprintf(buf, sizeof buf, "{\"expires_in\":%d}", HY_PAIR_TTL_S);
    return web_send_json(r, 200, buf);
}

/* ---------------------------------------------------------------- POST /api/pair {"code":"482913"} (open) */

#define PAIR_CODE_MAX 16   /* longer strings are refused before the spinlock (the code walk skips spaces) */

static esp_err_t h_pair(web_req_t *r)
{
    cJSON *o = cJSON_Parse(r->body);
    const cJSON *code = o ? cJSON_GetObjectItemCaseSensitive(o, "code") : NULL;
    if (!cJSON_IsString(code)) {
        cJSON_Delete(o);
        return web_send_err(r, 400, "invalid", "code", "Send {\"code\":\"123456\"}");
    }
    if (strnlen(code->valuestring, PAIR_CODE_MAX + 1) > PAIR_CODE_MAX) {
        cJSON_Delete(o);
        return web_send_err(r, 400, "invalid", "code", "The code has 6 digits");
    }
    int64_t now = mono_s();
    portENTER_CRITICAL(&s_pmux);
    hy_err_t e = hy_pair_try(&s_pair, code->valuestring, now);
    bool blocked = s_pair.blocked_until > now;
    if (e == HY_OK || blocked) {   /* used, or ended by the fifth wrong code (which has its own block) */
        hy_pair_gate_ended(&s_gate, now, false);
    }
    portEXIT_CRITICAL(&s_pmux);
    cJSON_Delete(o);

    if (e == HY_E_NOT_FOUND) {
        return web_send_err(r, 404, "not_found", "code", "No pairing code is running: start pairing again");
    }
    if (e != HY_OK) {
        if (blocked) {
            ESP_LOGW(TAG, "fifth wrong pairing code: pairing blocked for %d s", HY_PAIR_BLOCK_S);
            web_ui_pair_hide(HY_WEB_PAIR_BLOCKED);
        }
        return web_send_err(r, 422, "wrong_code", "code", "Wrong code");
    }

    uint8_t tok[32];
    esp_fill_random(tok, sizeof tok);
    char ua[160] = "";
    httpd_req_get_hdr_value_str(r->req, "User-Agent", ua, sizeof ua);
    esp_err_t err = web_sess_add(tok, ua, NULL);
    if (err != ESP_OK) {   /* the code is used up: the overlay closes as failed and the browser starts again */
        memset(tok, 0, sizeof tok);
        web_ui_pair_hide(HY_WEB_PAIR_FAILED);
        return web_send_hy_err(r, HY_E_IO, "The pairing could not be saved");
    }
    char cookie[128];
    int k = snprintf(cookie, sizeof cookie, "hy_s=");
    for (int i = 0; i < 32; i++) {
        k += snprintf(cookie + k, sizeof cookie - k, "%02x", tok[i]);
    }
    memset(tok, 0, sizeof tok);
    snprintf(cookie + k, sizeof cookie - k, "; Path=/; HttpOnly; SameSite=Strict; Max-Age=31536000");
    web_ui_pair_hide(HY_WEB_PAIR_DONE);
    web_ui_toast_ok("Browser paired");
    httpd_resp_set_hdr(r->req, "Set-Cookie", cookie);
    esp_err_t se = web_send_json(r, 200, "{\"paired\":true}");
    memset(cookie, 0, sizeof cookie);
    return se;
}

/* ---------------------------------------------------------------- GET/DELETE /api/browsers */

static esp_err_t h_browsers(web_req_t *r)
{
    web_sess_info_t s[WEB_SESS_MAX];
    size_t n = web_sess_list(s, WEB_SESS_MAX);
    cJSON *o = cJSON_CreateObject();
    cJSON *a = o ? cJSON_AddArrayToObject(o, "browsers") : NULL;
    for (size_t i = 0; a && i < n; i++) {
        cJSON *b = cJSON_CreateObject();
        if (!b) {
            break;
        }
        cJSON_AddNumberToObject(b, "index", s[i].slot);
        cJSON_AddStringToObject(b, "label", s[i].label);
        if (s[i].created) {
            cJSON_AddNumberToObject(b, "created", (double)s[i].created);
        } else {
            cJSON_AddNullToObject(b, "created");
        }
        cJSON_AddBoolToObject(b, "current", s[i].slot == r->sess);
        cJSON_AddItemToArray(a, b);
    }
    if (o) {
        cJSON_AddNumberToObject(o, "max", WEB_SESS_MAX);
    }
    return send_cjson(r, 200, o);
}

/* {"index":n} forgets one browser, {"all":true} every one (this browser included) */
static esp_err_t h_browsers_del(web_req_t *r)
{
    cJSON *o = cJSON_Parse(r->body);
    const cJSON *all = o ? cJSON_GetObjectItemCaseSensitive(o, "all") : NULL;
    const cJSON *idx = o ? cJSON_GetObjectItemCaseSensitive(o, "index") : NULL;
    int slot;
    if (cJSON_IsTrue(all)) {
        slot = -1;
    } else if (cJSON_IsNumber(idx) && idx->valuedouble >= 0 && idx->valuedouble < WEB_SESS_MAX &&
               idx->valuedouble == (double)(int)idx->valuedouble) {
        slot = (int)idx->valuedouble;
    } else {
        cJSON_Delete(o);
        return web_send_err(r, 400, "invalid", "index", "Send {\"index\":n} or {\"all\":true}");
    }
    cJSON_Delete(o);
    esp_err_t err = web_sess_forget(slot);
    if (err == ESP_ERR_NOT_FOUND) {
        return web_send_err(r, 404, "not_found", "index", "No paired browser at that index");
    }
    if (err != ESP_OK) {
        return web_send_hy_err(r, HY_E_IO, "The change could not be saved");
    }
    char buf[32];
    snprintf(buf, sizeof buf, "{\"paired\":%u}", (unsigned)web_sess_count());
    return web_send_json(r, 200, buf);
}

/* ---------------------------------------------------------------- route table */

/* The REST handler files define these; until then their routes answer 501. */
__attribute__((weak)) web_handler_t web_meds_route(hy_route_id_t id) { (void)id; return NULL; }
__attribute__((weak)) web_handler_t web_log_route(hy_route_id_t id) { (void)id; return NULL; }
__attribute__((weak)) web_handler_t web_settings_route(hy_route_id_t id) { (void)id; return NULL; }
__attribute__((weak)) web_handler_t web_wifi_route(hy_route_id_t id) { (void)id; return NULL; }
__attribute__((weak)) web_handler_t web_data_route(hy_route_id_t id) { (void)id; return NULL; }

web_handler_t web_route_handler(hy_route_id_t id)
{
    switch (id) {
    case HY_R_INFO: return h_info;
    case HY_R_STATE: return h_state;
    case HY_R_PAIR_START: return h_pair_start;
    case HY_R_PAIR: return h_pair;
    case HY_R_BROWSERS: return h_browsers;
    case HY_R_BROWSERS_DEL: return h_browsers_del;
    default: break;
    }
    web_handler_t h = web_meds_route(id);
    if (!h) {
        h = web_log_route(id);
    }
    if (!h) {
        h = web_settings_route(id);
    }
    if (!h) {
        h = web_wifi_route(id);
    }
    if (!h) {
        h = web_data_route(id);
    }
    return h;
}
