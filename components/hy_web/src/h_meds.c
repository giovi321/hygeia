/* Medicine routes: list, create, edit, archive, delete, order, pills and the dose actions.
 *
 * The httpd task parses and encodes. Every store, action and settings call runs in one job in the LVGL task. A job that
 * times out while it runs still completes after the 503, so a write can land after its "busy" answer; the web UI
 * reloads after a 503 for that reason. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "hy/actions.h"
#include "hy/clock.h"
#include "hy/occ.h"
#include "web_internal.h"

#define STALE_MSG "This medicine was changed on the device. Reload to see the changes"
#define LONG_JOB_MS 15000   /* a delete rewrites every month file of the medicine */
#define TOO_SOON_ROWS 32

typedef enum { F_OK = 0, F_ERR, F_API, F_STALE, F_CONFIRM, F_TOO_SOON, F_NOMEM } fail_t;

typedef struct {
    const char *body;       /* first: set by web_job_new */
    uint32_t id;
    fail_t fail;
    hy_err_t err;
    hy_api_err_t aerr;
    char etag[12];          /* If-Match without quotes */
    bool confirm;
    hy_med_t med;           /* in: the parsed record (create); out: the stored record */
    uint32_t log_id;
    size_t count;           /* delete: log entries counted or removed */
    int32_t since_min;
    int64_t ts, until;
    char note[64];
    bool has_ts, has_note;
    int pills_mode;         /* 0 set, 1 add */
    int32_t pills_val;
    uint32_t ids[HY_MAX_STORED];
    size_t n_ids;
} mctx_t;

typedef struct {
    const char *body;
    hy_med_t list[HY_MAX_STORED];
    size_t n;
} lctx_t;

static const hy_med_t *find_med(uint32_t id)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    for (size_t i = 0; i < n; i++) {
        if (ms[i].id == id) {
            return &ms[i];
        }
    }
    return NULL;
}

static void set_err(mctx_t *c, hy_err_t e)
{
    c->fail = F_ERR;
    c->err = e;
}

/* Copies the stored record of id into c->med; false (with a not-found error) when it is gone */
static bool load_med(mctx_t *c, uint32_t id)
{
    const hy_med_t *m = find_med(id);
    if (!m) {
        set_err(c, HY_E_NOT_FOUND);
        return false;
    }
    c->med = *m;
    return true;
}

/* Sends the failure answer of a job and returns true; false when the job succeeded */
static bool sent_failure(web_req_t *r, const mctx_t *c, esp_err_t *sent)
{
    switch (c->fail) {
    case F_OK:
        return false;
    case F_API:
        *sent = web_fail_api(r, &c->aerr);
        break;
    case F_NOMEM:
        *sent = web_oom(r);
        break;
    case F_STALE:
        *sent = web_send_err(r, 412, "stale", NULL, STALE_MSG);
        break;
    case F_CONFIRM: {
        cJSON *o = cJSON_CreateObject();
        if (o) {
            char m[96];
            snprintf(m, sizeof m, "This also deletes %u log %s", (unsigned)c->count,
                     c->count == 1 ? "entry" : "entries");
            cJSON_AddStringToObject(o, "error", "confirm");
            cJSON_AddStringToObject(o, "message", m);
            cJSON_AddNumberToObject(o, "log_entries", (double)c->count);
        }
        *sent = web_send_cjson(r, 409, o);
        break;
    }
    case F_TOO_SOON: {
        cJSON *o = cJSON_CreateObject();
        if (o) {
            char m[128];
            snprintf(m, sizeof m, "The last dose was %ld min ago. The minimum gap is %u min.", (long)c->since_min,
                     (unsigned)c->med.min_interval_min);
            cJSON_AddStringToObject(o, "error", "too_soon");
            cJSON_AddStringToObject(o, "message", m);
            cJSON_AddNumberToObject(o, "since_min", c->since_min);
            cJSON_AddNumberToObject(o, "min_interval_min", c->med.min_interval_min);
        }
        *sent = web_send_cjson(r, 409, o);
        break;
    }
    default:
        *sent = web_fail(r, c->err);
        break;
    }
    return true;
}

/* The record as the answer, with its ETag header (quoted; the body keeps the bare "etag") */
static esp_err_t send_med(web_req_t *r, int status, const hy_med_t *m)
{
    char *json = hy_api_med_json(m);
    if (!json) {
        return web_oom(r);
    }
    char etag[16];
    snprintf(etag, sizeof etag, "\"%08x\"", (unsigned)hy_api_med_etag(m));
    httpd_resp_set_hdr(r->req, "ETag", etag);
    esp_err_t e = web_send_json(r, status, json);
    free(json);
    return e;
}

/* A job with a medicine context; the {id} segment is already in it */
static mctx_t *begin(web_req_t *r, void (*fn)(void *), hy_job_t **j)
{
    *j = web_job_new(r, fn, sizeof(mctx_t));
    if (!*j) {
        return NULL;
    }
    mctx_t *c = hy_job_ctx(*j);
    c->id = r->id;
    return c;
}

/* Runs the job and answers with the stored record, or with the failure */
static esp_err_t run_med(web_req_t *r, hy_job_t *j, uint32_t timeout_ms, int status)
{
    esp_err_t sent;
    mctx_t *c = web_job_go(r, j, timeout_ms, &sent);
    if (!c) {
        return sent;
    }
    esp_err_t e = sent_failure(r, c, &sent) ? sent : send_med(r, status, &c->med);
    hy_job_put(j);
    return e;
}

/* ---------------------------------------------------------------- GET /api/medicines, GET /api/medicines/{id} */

static void job_list(void *arg)
{
    lctx_t *c = arg;
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    if (n > HY_MAX_STORED) {
        n = HY_MAX_STORED;
    }
    memcpy(c->list, ms, n * sizeof ms[0]);
    c->n = n;
}

static esp_err_t h_list(web_req_t *r)
{
    hy_job_t *j = web_job_new(r, job_list, sizeof(lctx_t));
    esp_err_t sent;
    if (!j) {
        return web_oom(r);
    }
    lctx_t *c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!c) {
        return sent;
    }
    char *json = hy_api_meds_json(c->list, c->n);
    esp_err_t e = json ? web_send_json(r, 200, json) : web_oom(r);
    free(json);
    hy_job_put(j);
    return e;
}

static void job_get(void *arg)
{
    mctx_t *c = arg;
    load_med(c, c->id);
}

static esp_err_t h_get(web_req_t *r)
{
    hy_job_t *j;
    return begin(r, job_get, &j) ? run_med(r, j, HY_JOB_TIMEOUT_MS, 200) : web_oom(r);
}

/* ---------------------------------------------------------------- POST /api/medicines */

static void job_create(void *arg)
{
    mctx_t *c = arg;
    hy_med_t m = c->med;
    m.id = 0;
    m.archived = false;
    hy_err_t e = hy_act_med_upsert(&m);
    if (e != HY_OK) {
        set_err(c, e);
        return;
    }
    load_med(c, m.id);
}

static esp_err_t h_create(web_req_t *r)
{
    hy_med_t m;
    hy_api_err_t ae;
    if (hy_api_med_from_json(web_body_or_empty(r), NULL, &m, &ae) != HY_OK) {
        return web_fail_api(r, &ae);
    }
    hy_job_t *j;
    mctx_t *c = begin(r, job_create, &j);
    if (!c) {
        return web_oom(r);
    }
    c->med = m;   /* the body's pill count is the starting count of a new medicine */
    return run_med(r, j, HY_JOB_TIMEOUT_MS, 201);
}

/* ---------------------------------------------------------------- PUT /api/medicines/{id} */

static void job_edit(void *arg)
{
    mctx_t *c = arg;
    const hy_med_t *live = find_med(c->id);
    if (!live) {
        set_err(c, HY_E_NOT_FOUND);
        return;
    }
    char cur[12];
    snprintf(cur, sizeof cur, "%08x", (unsigned)hy_api_med_etag(live));
    if (strcasecmp(cur, c->etag) != 0) {   /* the record changed since the browser read it: nothing is written */
        c->fail = F_STALE;
        return;
    }
    hy_med_t m;
    if (hy_api_med_from_json(c->body && c->body[0] ? c->body : "{}", live, &m, &c->aerr) != HY_OK) {
        c->fail = F_API;
        return;
    }
    m.id = live->id;
    m.archived = live->archived;
    m.order = live->order;
    /* The remaining count changes through /pills. The one exception is switching pill counting on: the form then
     * gives the starting count and hy_act_med_upsert takes it. */
    if (live->pills_enabled || !m.pills_enabled) {
        m.pills_left = live->pills_left;
    }
    hy_err_t e = hy_act_med_upsert(&m);
    if (e != HY_OK) {
        set_err(c, e);
        return;
    }
    load_med(c, c->id);
}

/* If-Match without quotes (or the weak prefix) into out; false when the header is absent */
static bool if_match(web_req_t *r, char out[12])
{
    char h[40];
    if (httpd_req_get_hdr_value_str(r->req, "If-Match", h, sizeof h) != ESP_OK) {
        return false;
    }
    const char *p = h;
    if (p[0] == 'W' && p[1] == '/') {
        p += 2;
    }
    if (*p == '"') {
        p++;
    }
    size_t n = strcspn(p, "\"");
    if (n >= 12) {
        n = 11;   /* too long to be an etag: the compare in the job fails */
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static esp_err_t h_put(web_req_t *r)
{
    char etag[12];
    if (!if_match(r, etag)) {
        return web_send_err(r, 428, "precondition_required", NULL, "Send If-Match with the etag from the last read");
    }
    hy_job_t *j;
    mctx_t *c = begin(r, job_edit, &j);
    if (!c) {
        return web_oom(r);
    }
    memcpy(c->etag, etag, sizeof c->etag);
    return run_med(r, j, HY_JOB_TIMEOUT_MS, 200);
}

/* ---------------------------------------------------------------- archive, unarchive */

static void job_archive(void *arg)
{
    mctx_t *c = arg;
    hy_err_t e = hy_act_med_archive(c->id, c->confirm);   /* confirm carries the target archived flag */
    if (e != HY_OK) {
        set_err(c, e);
        return;
    }
    load_med(c, c->id);
}

static esp_err_t archive_to(web_req_t *r, bool archived)
{
    hy_job_t *j;
    mctx_t *c = begin(r, job_archive, &j);
    if (!c) {
        return web_oom(r);
    }
    c->confirm = archived;
    return run_med(r, j, HY_JOB_TIMEOUT_MS, 200);
}

static esp_err_t h_archive(web_req_t *r) { return archive_to(r, true); }
static esp_err_t h_unarchive(web_req_t *r) { return archive_to(r, false); }

/* ---------------------------------------------------------------- DELETE /api/medicines/{id}[?confirm=1] */

static void job_delete(void *arg)
{
    mctx_t *c = arg;
    const hy_med_t *m = find_med(c->id);
    if (!m) {
        set_err(c, HY_E_NOT_FOUND);
        return;
    }
    if (!m->archived) {   /* only archived medicines can be deleted */
        set_err(c, HY_E_INVALID);
        return;
    }
    if (!c->confirm) {
        c->count = hy_act_med_log_count(c->id);
        c->fail = F_CONFIRM;
        return;
    }
    size_t n = 0;
    hy_err_t e = hy_act_med_delete(c->id, &n);
    c->count = n;
    if (e != HY_OK) {
        set_err(c, e);
    }
}

static esp_err_t h_delete(web_req_t *r)
{
    char v[4];
    hy_job_t *j;
    mctx_t *c = begin(r, job_delete, &j);
    if (!c) {
        return web_oom(r);
    }
    c->confirm = web_query_get(r, "confirm", v, sizeof v) && strcmp(v, "1") == 0;
    esp_err_t sent;
    c = web_job_go(r, j, LONG_JOB_MS, &sent);
    if (!c) {
        return sent;
    }
    esp_err_t e;
    if (sent_failure(r, c, &sent)) {
        e = sent;
    } else {
        char buf[48];
        snprintf(buf, sizeof buf, "{\"deleted\":true,\"log_entries\":%u}", (unsigned)c->count);
        e = web_send_json(r, 200, buf);
    }
    hy_job_put(j);
    return e;
}

/* ---------------------------------------------------------------- PUT /api/medicines/order */

static void job_order(void *arg)
{
    mctx_t *c = arg;
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    if (hy_api_order_from_json(c->body && c->body[0] ? c->body : "{}", ms, n, c->ids, &c->n_ids, &c->aerr) != HY_OK) {
        c->fail = F_API;
        return;
    }
    hy_err_t e = hy_act_med_reorder(c->ids, c->n_ids);
    if (e != HY_OK) {
        set_err(c, e);
    }
}

static esp_err_t h_order(web_req_t *r)
{
    hy_job_t *j;
    esp_err_t sent;
    if (!begin(r, job_order, &j)) {
        return web_oom(r);
    }
    mctx_t *c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!c) {
        return sent;
    }
    esp_err_t e;
    if (sent_failure(r, c, &sent)) {
        e = sent;
    } else {
        cJSON *o = cJSON_CreateObject();
        cJSON *a = o ? cJSON_AddArrayToObject(o, "ids") : NULL;
        for (size_t i = 0; a && i < c->n_ids; i++) {
            cJSON_AddItemToArray(a, cJSON_CreateNumber(c->ids[i]));
        }
        e = web_send_cjson(r, 200, o);
    }
    hy_job_put(j);
    return e;
}

/* ---------------------------------------------------------------- POST /api/medicines/{id}/pills */

static void job_pills(void *arg)
{
    mctx_t *c = arg;
    const hy_med_t *m = find_med(c->id);
    if (!m) {
        set_err(c, HY_E_NOT_FOUND);
        return;
    }
    int32_t v = c->pills_val;
    if (c->pills_mode == 1) {
        v += m->pills_left;
        v = v < 0 ? 0 : v > 9999 ? 9999 : v;
    }
    hy_err_t e = hy_act_pills_set(c->id, (int16_t)v);
    if (e != HY_OK) {
        set_err(c, e);
        return;
    }
    load_med(c, c->id);
}

/* {"left":n} sets the count; {"add":n} adds n (negative takes pills away) to the live count, clamped to 0..9999 */
static esp_err_t h_pills(web_req_t *r)
{
    cJSON *o = cJSON_Parse(web_body_or_empty(r));
    const cJSON *left = o ? cJSON_GetObjectItemCaseSensitive(o, "left") : NULL;
    const cJSON *add = o ? cJSON_GetObjectItemCaseSensitive(o, "add") : NULL;
    int mode = -1;
    double v = 0;
    if (cJSON_IsNumber(left) && !add) {
        mode = 0;
        v = left->valuedouble;
    } else if (cJSON_IsNumber(add) && !left) {
        mode = 1;
        v = add->valuedouble;
    }
    cJSON_Delete(o);
    if (mode < 0 || v < (mode ? -9999 : 0) || v > 9999 || v != (double)(int32_t)v) {
        return web_send_err(r, 400, "invalid", mode == 1 ? "add" : "left",
                            "Send {\"left\":n} or {\"add\":n} with n up to 9999");
    }
    hy_job_t *j;
    mctx_t *c = begin(r, job_pills, &j);
    if (!c) {
        return web_oom(r);
    }
    c->pills_mode = mode;
    c->pills_val = (int32_t)v;
    return run_med(r, j, HY_JOB_TIMEOUT_MS, 200);
}

/* ---------------------------------------------------------------- dose actions */

static esp_err_t run_dose(web_req_t *r, hy_job_t *j)
{
    esp_err_t sent;
    mctx_t *c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!c) {
        return sent;
    }
    esp_err_t e;
    if (sent_failure(r, c, &sent)) {
        e = sent;
    } else {
        char buf[80];
        snprintf(buf, sizeof buf, "{\"log_id\":%u,\"undo_until\":%lld}", (unsigned)c->log_id, (long long)c->until);
        e = web_send_json(r, 200, buf);
    }
    hy_job_put(j);
    return e;
}

static void job_log(void *arg)
{
    mctx_t *c = arg;
    const hy_med_t *m = find_med(c->id);
    if (!m) {
        set_err(c, HY_E_NOT_FOUND);
        return;
    }
    int64_t now = (int64_t)time(NULL);
    /* A live dose of an as-needed medicine inside its minimum interval needs a confirmation. Only the interval's own
     * window is read: any entry in it means too soon, even when the buffer fills. */
    if (!c->confirm && !c->has_ts && m->type == HY_SCHED_AS_NEEDED && m->min_interval_min > 0 && hy_clock_valid()) {
        hy_log_t *rows = heap_caps_malloc(TOO_SOON_ROWS * sizeof *rows, MALLOC_CAP_SPIRAM);
        if (!rows) {   /* no degraded answer: the request fails and can be repeated */
            c->fail = F_NOMEM;
            return;
        }
        {
            size_t n = hy_store_port()->query_log(NULL, now - (int64_t)m->min_interval_min * 60, now + 1, m->id, rows,
                                                  TOO_SOON_ROWS);
            int32_t since = -1;
            hy_err_t chk = hy_min_interval_check(m, now, rows, n, &since);
            heap_caps_free(rows);
            if (chk == HY_W_TOO_SOON) {
                c->med = *m;
                c->since_min = since;
                c->fail = F_TOO_SOON;
                return;
            }
        }
    }
    uint32_t id = 0;
    hy_err_t e = hy_act_log_taken(c->id, c->has_ts ? c->ts : 0, c->has_note ? c->note : NULL, HY_SRC_WEB, &id);
    if (e != HY_OK) {
        set_err(c, e);
        return;
    }
    c->log_id = id;
    c->until = now + HY_UNDO_WINDOW_S;
}

/* {"confirm":true} accepts a dose inside the minimum interval; "ts" backdates it, "note" adds a note */
static esp_err_t h_log_dose(web_req_t *r)
{
    hy_api_logreq_t q;
    hy_api_err_t ae;
    if (hy_api_logreq_from_json(web_body_or_empty(r), &q, &ae) != HY_OK) {
        return web_fail_api(r, &ae);
    }
    if (q.has_ts && (q.ts > (int64_t)time(NULL) + 60 || q.ts < hy_clock_build_epoch())) {
        return web_send_err(r, 400, "invalid", "ts", "The time is not valid");
    }
    hy_job_t *j;
    mctx_t *c = begin(r, job_log, &j);
    if (!c) {
        return web_oom(r);
    }
    c->confirm = q.confirm;
    c->has_ts = q.has_ts;
    c->ts = q.ts;
    c->has_note = q.has_note;
    memcpy(c->note, q.note, sizeof c->note);
    return run_dose(r, j);
}

static void job_skip(void *arg)
{
    mctx_t *c = arg;
    uint32_t id = 0;
    hy_err_t e = hy_act_skip(c->id, HY_SRC_WEB, &id);
    if (e != HY_OK) {
        set_err(c, e);
        return;
    }
    c->log_id = id;
    c->until = (int64_t)time(NULL) + HY_UNDO_WINDOW_S;
}

static esp_err_t h_skip(web_req_t *r)
{
    hy_job_t *j;
    return begin(r, job_skip, &j) ? run_dose(r, j) : web_oom(r);
}

static void job_snooze(void *arg)
{
    mctx_t *c = arg;
    int64_t until = 0;
    hy_err_t e = hy_act_snooze(c->id, hy_settings()->snooze_min, HY_SRC_WEB, &until);
    if (e != HY_OK) {
        set_err(c, e);
        return;
    }
    c->until = until;
}

static esp_err_t h_snooze(web_req_t *r)
{
    hy_job_t *j;
    esp_err_t sent;
    if (!begin(r, job_snooze, &j)) {
        return web_oom(r);
    }
    mctx_t *c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!c) {
        return sent;
    }
    esp_err_t e;
    if (sent_failure(r, c, &sent)) {
        e = sent;
    } else {
        char buf[40];
        snprintf(buf, sizeof buf, "{\"until\":%lld}", (long long)c->until);
        e = web_send_json(r, 200, buf);
    }
    hy_job_put(j);
    return e;
}

/* ---------------------------------------------------------------- route table */

web_handler_t web_meds_route(hy_route_id_t id)
{
    switch (id) {
    case HY_R_MEDS: return h_list;
    case HY_R_MED_NEW: return h_create;
    case HY_R_MED: return h_get;
    case HY_R_MED_PUT: return h_put;
    case HY_R_MED_DEL: return h_delete;
    case HY_R_MED_ARCHIVE: return h_archive;
    case HY_R_MED_UNARCHIVE: return h_unarchive;
    case HY_R_MED_ORDER: return h_order;
    case HY_R_MED_PILLS: return h_pills;
    case HY_R_MED_LOG: return h_log_dose;
    case HY_R_MED_SKIP: return h_skip;
    case HY_R_MED_SNOOZE: return h_snooze;
    default: return NULL;
    }
}
