/* Log, history and schedule routes.
 *
 * Log writes and reads run as one short job each. History and schedule walk every active medicine, so they run one
 * job per chunk of medicines (sized by medicine-days, so each job stays near 300 ms) and the httpd task joins the pieces:
 * the LVGL task is never held for the whole walk. A medicine that disappears between two jobs is left out. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "hy/actions.h"
#include "hy/clock.h"
#include "hy/occ.h"
#include "hy/sched_run.h"
#include "hy/summary.h"
#include "web_internal.h"

#define HY_CHUNK 10          /* most medicines in one history job (7 day range) */
#define HIST_WORK 70         /* medicine-days in one history job: about 4 ms each, so a job stays near 300 ms */
#define SCHED_CHUNK 5        /* medicines in one schedule job */
#define LOG_ENTRY_JSON 640
#define MAX_RANGE_S (8 * 86400)
#define SLOTS_PER_MED 72       /* 8 times a day over 8 days, plus room for a clock change inside the range */
#define DEEP_ROWS 512
#define DEEP_WINDOW_S (32 * 86400)

/* ---------------------------------------------------------------- helpers */

static void put_entry(web_sb_t *b, const hy_log_t *e)
{
    char t[LOG_ENTRY_JSON];
    int n = hy_api_log_json(e, t, sizeof t);
    if (n < 0) {
        b->bad = true;
    } else {
        web_sb_add(b, t, (size_t)n);
    }
}

static esp_err_t send_entry(web_req_t *r, int status, const hy_log_t *e)
{
    web_sb_t b = {0};
    put_entry(&b, e);
    return web_sb_send(r, status, &b);
}

/* ---------------------------------------------------------------- GET /api/log */

typedef struct {
    const char *body;
    hy_api_logq_t q;
    size_t cap;
    size_t n;       /* rows kept after the cursor filter */
    bool more;
    hy_log_t rows[];
} lq_ctx_t;

static void job_log_query(void *arg)
{
    lq_ctx_t *c = arg;
    int64_t from = c->q.from > c->q.after_ts ? c->q.from : c->q.after_ts;
    size_t n = hy_store_port()->query_log(NULL, from, c->q.to, c->q.med, c->rows, c->cap);
    size_t kept = hy_api_page_filter(c->rows, n, c->q.after_ts, c->q.after_id);
    c->more = kept > c->q.limit || n == c->cap;
    c->n = kept > c->q.limit ? c->q.limit : kept;
}

/* ?from=&to=&med=&limit=&after=<ts>.<id>: ascending, {"log":[...],"next":"<ts>.<id>" or null} */
static esp_err_t h_log_get(web_req_t *r)
{
    hy_api_logq_t q;
    hy_api_err_t ae;
    if (hy_api_logq_parse(web_query(r), (int64_t)time(NULL), &q, &ae) != HY_OK) {
        return web_fail_api(r, &ae);
    }
    size_t cap = (size_t)q.limit + 1 + 8;   /* one to see whether more follow, a few for entries at the cursor's ts */
    hy_job_t *j = web_job_new(r, job_log_query, sizeof(lq_ctx_t) + cap * sizeof(hy_log_t));
    if (!j) {
        return web_oom(r);
    }
    lq_ctx_t *c = hy_job_ctx(j);
    c->q = q;
    c->cap = cap;
    esp_err_t sent;
    c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!c) {
        return sent;
    }
    web_sb_t b = {0};
    web_sb_str(&b, "{\"log\":[");
    for (size_t i = 0; i < c->n; i++) {
        if (i) {
            web_sb_str(&b, ",");
        }
        put_entry(&b, &c->rows[i]);
    }
    if (c->more && c->n > 0) {
        web_sb_fmt(&b, "],\"next\":\"%lld.%u\"}", (long long)c->rows[c->n - 1].ts, (unsigned)c->rows[c->n - 1].id);
    } else {
        web_sb_str(&b, "],\"next\":null}");
    }
    hy_job_put(j);
    return web_sb_send(r, 200, &b);
}

/* ---------------------------------------------------------------- entry writes */

typedef struct {
    const char *body;
    uint32_t id;
    hy_api_logreq_t req;
    int64_t ts;          /* delete: the entry's ts */
    hy_err_t err;
    bool has_entry;
    hy_log_t entry;
} lw_ctx_t;

static lw_ctx_t *lw_begin(web_req_t *r, void (*fn)(void *), hy_job_t **j)
{
    *j = web_job_new(r, fn, sizeof(lw_ctx_t));
    if (!*j) {
        return NULL;
    }
    lw_ctx_t *c = hy_job_ctx(*j);
    c->id = r->id;
    return c;
}

/* Reads the entry with this id back from the store (it sits at ts) */
static void fetch_entry(lw_ctx_t *c, uint32_t id, int64_t ts)
{
    hy_log_t rows[8];
    size_t n = hy_store_port()->query_log(NULL, ts, ts + 1, 0, rows, 8);
    for (size_t i = 0; i < n; i++) {
        if (rows[i].id == id) {
            c->entry = rows[i];
            c->has_entry = true;
            return;
        }
    }
}

static esp_err_t finish_write(web_req_t *r, hy_job_t *j, int status)
{
    esp_err_t sent;
    lw_ctx_t *c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!c) {
        return sent;
    }
    esp_err_t e;
    if (c->err != HY_OK) {
        e = web_fail(r, c->err);
    } else if (c->has_entry) {
        e = send_entry(r, status, &c->entry);
    } else {
        e = web_send_json(r, status, "{\"ok\":true}");
    }
    hy_job_put(j);
    return e;
}

/* POST /api/log: a dose taken at a past time */
static void job_log_new(void *arg)
{
    lw_ctx_t *c = arg;
    uint32_t id = 0;
    c->err = hy_act_log_taken(c->req.med_id, c->req.ts, c->req.has_note ? c->req.note : NULL, HY_SRC_WEB, &id);
    if (c->err == HY_OK) {
        fetch_entry(c, id, c->req.ts);
    }
}

static esp_err_t h_log_new(web_req_t *r)
{
    hy_api_logreq_t q;
    hy_api_err_t ae;
    if (hy_api_logreq_from_json(web_body_or_empty(r), &q, &ae) != HY_OK) {
        return web_fail_api(r, &ae);
    }
    if (!q.has_med) {
        return web_send_err(r, 400, "invalid", "med_id", "med_id is required");
    }
    if (!q.has_ts) {
        return web_send_err(r, 400, "invalid", "ts", "ts is required");
    }
    if (q.has_status && q.status != HY_TAKEN) {
        return web_send_err(r, 400, "invalid", "status", "Only taken doses can be added");
    }
    if (q.ts > (int64_t)time(NULL) + 60 || q.ts < hy_clock_build_epoch()) {
        return web_send_err(r, 400, "invalid", "ts", "The time is not valid");
    }
    hy_job_t *j;
    lw_ctx_t *c = lw_begin(r, job_log_new, &j);
    if (!c) {
        return web_oom(r);
    }
    c->req = q;
    return finish_write(r, j, 201);
}

/* PUT /api/log/{id}: orig_ts (the ts the entry has now) locates it */
static void job_log_put(void *arg)
{
    lw_ctx_t *c = arg;
    c->err = hy_act_log_update(c->id, &c->req);
    if (c->err == HY_OK) {
        fetch_entry(c, c->id, c->req.has_ts ? c->req.ts : c->req.orig_ts);
    }
}

static esp_err_t h_log_put(web_req_t *r)
{
    hy_api_logreq_t q;
    hy_api_err_t ae;
    if (hy_api_logreq_from_json(web_body_or_empty(r), &q, &ae) != HY_OK) {
        return web_fail_api(r, &ae);
    }
    if (q.orig_ts == 0) {
        return web_send_err(r, 400, "invalid", "orig_ts", "orig_ts is required");
    }
    if (q.has_ts && (q.ts > (int64_t)time(NULL) + 60 || q.ts < hy_clock_build_epoch())) {
        return web_send_err(r, 400, "invalid", "ts", "The time is not valid");
    }
    hy_job_t *j;
    lw_ctx_t *c = lw_begin(r, job_log_put, &j);
    if (!c) {
        return web_oom(r);
    }
    c->req = q;
    return finish_write(r, j, 200);
}

/* DELETE /api/log/{id}?ts=<ts of the entry> */
static void job_log_del(void *arg)
{
    lw_ctx_t *c = arg;
    c->err = hy_act_log_delete(c->id, c->ts);
}

static esp_err_t h_log_del(web_req_t *r)
{
    char v[24];
    char *end = NULL;
    long long ts = 0;
    if (web_query_get(r, "ts", v, sizeof v)) {
        ts = strtoll(v, &end, 10);
    }
    if (!end || end == v || *end || ts <= 0) {
        return web_send_err(r, 400, "invalid", "ts", "Send ?ts= with the entry's time");
    }
    hy_job_t *j;
    lw_ctx_t *c = lw_begin(r, job_log_del, &j);
    if (!c) {
        return web_oom(r);
    }
    c->ts = ts;
    esp_err_t sent;
    c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!c) {
        return sent;
    }
    esp_err_t e = c->err == HY_OK ? web_send_json(r, 200, "{\"deleted\":true}") : web_fail(r, c->err);
    hy_job_put(j);
    return e;
}

/* POST /api/log/{id}/undo */
static void job_undo(void *arg)
{
    lw_ctx_t *c = arg;
    c->err = hy_act_undo(c->id);
}

static esp_err_t h_log_undo(web_req_t *r)
{
    hy_job_t *j;
    lw_ctx_t *c = lw_begin(r, job_undo, &j);
    if (!c) {
        return web_oom(r);
    }
    esp_err_t sent;
    c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!c) {
        return sent;
    }
    esp_err_t e = c->err == HY_OK ? web_send_json(r, 200, "{\"undone\":true}") : web_fail(r, c->err);
    hy_job_put(j);
    return e;
}

/* ---------------------------------------------------------------- GET /api/history?days=7|30 */

static const char *const MARK_NAME[] = {"none", "on_time", "late", "missed", "skipped", "untracked", "as_needed_taken"};

typedef struct {
    const char *body;
    int days;
    bool no_tracking;
    int32_t today, first;
    size_t n;
    struct { uint32_t id; char code[3]; char name[33]; uint8_t color; } m[HY_MAX_MEDS];
} hmeta_t;

static void job_hist_meta(void *arg)
{
    hmeta_t *c = arg;
    const hy_snapshot_t *sn = hy_sched_snapshot();
    if (hy_sched_generation() == 0 || !sn->tracking) {
        c->no_tracking = true;
        return;
    }
    c->today = hy_local_date(sn->in.now, sn->in.tz);
    c->first = hy_date_add(c->today, -(c->days - 1));
    for (size_t i = 0; i < sn->in.n_meds && c->n < HY_MAX_MEDS; i++) {
        const hy_med_t *m = &sn->in.meds[i];
        if (m->archived) {
            continue;
        }
        c->m[c->n].id = m->id;
        strlcpy(c->m[c->n].code, m->code, sizeof c->m[c->n].code);
        strlcpy(c->m[c->n].name, m->name, sizeof c->m[c->n].name);
        c->m[c->n].color = m->color;
        c->n++;
    }
}

typedef struct {
    const char *body;
    int days;
    int32_t first;
    bool nomem;
    size_t n;
    uint32_t ids[HY_CHUNK];
    bool gone[HY_CHUNK];
    bool prog[HY_CHUNK];
    uint8_t marks[HY_CHUNK][30];
    hy_adherence_t ad[HY_CHUNK];
} hchunk_t;

/* The marks of up to HY_CHUNK medicines. The 30 day range reads each medicine's last 32 days from the store, as the
 * device's History page does (the scheduler snapshot keeps 8 days); the 7 day range uses the snapshot. */
static void job_hist_chunk(void *arg)
{
    hchunk_t *c = arg;
    const hy_snapshot_t *sn = hy_sched_snapshot();
    hy_log_t *buf = c->days == 30 ? heap_caps_malloc(DEEP_ROWS * sizeof *buf, MALLOC_CAP_SPIRAM) : NULL;
    if (c->days == 30 && !buf) {   /* the 8 day snapshot would read older days as missed: fail instead */
        c->nomem = true;
        return;
    }
    for (size_t i = 0; i < c->n; i++) {
        const hy_med_t *m = NULL;
        for (size_t k = 0; k < sn->in.n_meds; k++) {
            if (sn->in.meds[k].id == c->ids[i] && !sn->in.meds[k].archived) {
                m = &sn->in.meds[k];
            }
        }
        if (!m || !sn->tracking) {
            c->gone[i] = true;
            continue;
        }
        hy_sched_in_t din = sn->in;
        const hy_sched_in_t *in = &sn->in;
        if (buf) {
            din.logs = buf;
            din.n_logs = web_log_window(m->id, sn->in.now - DEEP_WINDOW_S, sn->in.now + 1, buf, DEEP_ROWS);
            in = &din;
        }
        for (int d = 0; d < c->days; d++) {
            bool prog = false;
            c->marks[i][d] = (uint8_t)hy_day_mark(m, hy_date_add(c->first, d), in, sn->sched,
                                                  d == c->days - 1 ? &prog : NULL);
            if (d == c->days - 1) {
                c->prog[i] = prog;
            }
        }
        if (buf) {   /* the same function the integrations use, so the 30 day numbers cannot drift */
            if (!hy_sched_med_adherence(m->id, buf, DEEP_ROWS, &c->ad[i])) {
                c->gone[i] = true;
            }
        } else {
            hy_adherence(m->id, c->first, c->days, in, sn->sched, &c->ad[i]);
        }
    }
    heap_caps_free(buf);
}

typedef struct {
    bool gone, prog;
    uint8_t marks[30];
    hy_adherence_t ad;
} hres_t;

static esp_err_t h_history(web_req_t *r)
{
    char v[8];
    int days = 7;
    if (web_query_get(r, "days", v, sizeof v)) {
        days = strcmp(v, "7") == 0 ? 7 : strcmp(v, "30") == 0 ? 30 : 0;
    }
    if (days == 0) {
        return web_send_err(r, 400, "invalid", "days", "days is 7 or 30");
    }
    hy_job_t *j = web_job_new(r, job_hist_meta, sizeof(hmeta_t));
    if (!j) {
        return web_oom(r);
    }
    ((hmeta_t *)hy_job_ctx(j))->days = days;
    esp_err_t sent;
    hmeta_t *mc = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!mc) {
        return sent;
    }
    hmeta_t *meta = heap_caps_malloc(sizeof *meta, MALLOC_CAP_SPIRAM);
    hres_t *res = heap_caps_calloc(HY_MAX_MEDS, sizeof *res, MALLOC_CAP_SPIRAM);
    if (!meta || !res) {
        heap_caps_free(meta);
        heap_caps_free(res);
        hy_job_put(j);
        return web_oom(r);
    }
    *meta = *mc;
    hy_job_put(j);
    if (meta->no_tracking) {
        heap_caps_free(meta);
        heap_caps_free(res);
        return web_fail(r, HY_E_CLOCK);
    }

    size_t per = HIST_WORK / (size_t)days;   /* 10 medicines for 7 days, 2 for 30 */
    if (per > HY_CHUNK) {
        per = HY_CHUNK;
    }
    for (size_t at = 0; at < meta->n; at += per) {
        j = web_job_new(r, job_hist_chunk, sizeof(hchunk_t));
        if (!j) {
            heap_caps_free(meta);
            heap_caps_free(res);
            return web_oom(r);
        }
        hchunk_t *c = hy_job_ctx(j);
        c->days = days;
        c->first = meta->first;
        c->n = meta->n - at < per ? meta->n - at : per;
        for (size_t i = 0; i < c->n; i++) {
            c->ids[i] = meta->m[at + i].id;
        }
        c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
        if (!c) {
            heap_caps_free(meta);
            heap_caps_free(res);
            return sent;
        }
        if (c->nomem) {
            hy_job_put(j);
            heap_caps_free(meta);
            heap_caps_free(res);
            return web_oom(r);
        }
        for (size_t i = 0; i < c->n; i++) {
            res[at + i].gone = c->gone[i];
            res[at + i].prog = c->prog[i];
            memcpy(res[at + i].marks, c->marks[i], sizeof res[at + i].marks);
            res[at + i].ad = c->ad[i];
        }
        hy_job_put(j);
    }

    uint32_t tt = 0, tr = 0;
    for (size_t i = 0; i < meta->n; i++) {
        if (!res[i].gone) {
            tt += res[i].ad.taken;
            tr += res[i].ad.resolved;
        }
    }
    char d1[11], d2[11];
    web_date_str(meta->first, d1);
    web_date_str(meta->today, d2);
    web_sb_t b = {0};
    web_sb_fmt(&b, "{\"days\":%d,\"first\":\"%s\",\"today\":\"%s\",\"adherence\":{\"taken\":%u,\"resolved\":%u},\"meds\":[",
               days, d1, d2, (unsigned)(tt > 65535 ? 65535 : tt), (unsigned)(tr > 65535 ? 65535 : tr));
    bool first = true;
    for (size_t i = 0; i < meta->n; i++) {
        if (res[i].gone) {
            continue;
        }
        web_sb_fmt(&b, "%s{\"id\":%u,\"code\":", first ? "" : ",", (unsigned)meta->m[i].id);
        first = false;
        web_sb_jstr(&b, meta->m[i].code);
        web_sb_str(&b, ",\"name\":");
        web_sb_jstr(&b, meta->m[i].name);
        web_sb_fmt(&b, ",\"color\":%u,\"in_progress\":%s,\"adherence\":{\"taken\":%u,\"resolved\":%u},\"marks\":[",
                   (unsigned)meta->m[i].color, res[i].prog ? "true" : "false", (unsigned)res[i].ad.taken,
                   (unsigned)res[i].ad.resolved);
        for (int d = 0; d < days; d++) {
            uint8_t mk = res[i].marks[d];
            web_sb_fmt(&b, "%s\"%s\"", d ? "," : "", MARK_NAME[mk < sizeof MARK_NAME / sizeof MARK_NAME[0] ? mk : 0]);
        }
        web_sb_str(&b, "]}");
    }
    web_sb_str(&b, "]}");
    heap_caps_free(meta);
    heap_caps_free(res);
    return web_sb_send(r, 200, &b);
}

/* ---------------------------------------------------------------- GET /api/schedule?from=&to= */

static const char *const OCC_NAME[] = {"upcoming", "due", "late", "missed", "taken", "skipped", "untracked"};

typedef struct { uint32_t med_id; int64_t slot; uint8_t state; } sslot_t;

typedef struct {
    const char *body;
    bool no_tracking;
    size_t n;
    uint32_t ids[HY_MAX_MEDS];
} smeta_t;

static void job_sched_meta(void *arg)
{
    smeta_t *c = arg;
    const hy_snapshot_t *sn = hy_sched_snapshot();
    if (hy_sched_generation() == 0 || !sn->tracking) {
        c->no_tracking = true;
        return;
    }
    for (size_t i = 0; i < sn->in.n_meds && c->n < HY_MAX_MEDS; i++) {
        if (!sn->in.meds[i].archived && sn->in.meds[i].type == HY_SCHED_FIXED) {
            c->ids[c->n++] = sn->in.meds[i].id;
        }
    }
}

typedef struct {
    const char *body;
    int64_t from, to;
    size_t n_ids;
    uint32_t ids[SCHED_CHUNK];
    bool nomem;
    size_t n;
    sslot_t slots[SCHED_CHUNK * SLOTS_PER_MED];
} schunk_t;

static void job_sched_chunk(void *arg)
{
    schunk_t *c = arg;
    const hy_snapshot_t *sn = hy_sched_snapshot();
    if (!sn->tracking) {
        return;
    }
    int64_t now = (int64_t)time(NULL);
    hy_log_t *buf = heap_caps_malloc(DEEP_ROWS * sizeof *buf, MALLOC_CAP_SPIRAM);
    if (!buf) {   /* states without the logs would show taken doses as missed: fail instead */
        c->nomem = true;
        return;
    }
    for (size_t i = 0; i < c->n_ids; i++) {
        const hy_med_t *m = NULL;
        for (size_t k = 0; k < sn->in.n_meds; k++) {
            if (sn->in.meds[k].id == c->ids[i] && !sn->in.meds[k].archived) {
                m = &sn->in.meds[k];
            }
        }
        if (!m) {
            continue;
        }
        int64_t occ[SLOTS_PER_MED];
        size_t no = hy_occurrences(m, c->from, c->to, sn->in.tz, occ, SLOTS_PER_MED);
        /* logs that can decide these slots: from a day before the range to a day after it, but none from the future */
        size_t nl = 0;
        int64_t lo = c->from - 86400, hi = c->to + 86400 < now + 1 ? c->to + 86400 : now + 1;
        if (no > 0 && hi > lo) {
            nl = web_log_window(m->id, lo, hi, buf, DEEP_ROWS);
        }
        hy_gap_t gap = {0, hy_sched_tracked_since(sn->sched, m->id)};
        for (size_t k = 0; k < no; k++) {
            sslot_t *s = &c->slots[c->n++];
            s->med_id = m->id;
            s->slot = occ[k];
            s->state = (uint8_t)hy_occ_state(m, occ[k], 0, now, sn->in.tz, buf, nl, &gap, 1, sn->in.last_change);
        }
    }
    heap_caps_free(buf);
}

static int slot_cmp(const void *a, const void *b)
{
    const sslot_t *x = a, *y = b;
    if (x->slot != y->slot) {
        return x->slot < y->slot ? -1 : 1;
    }
    return x->med_id < y->med_id ? -1 : x->med_id > y->med_id;
}

static bool parse_ts(const char *s, int64_t *out)
{
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end || v <= 0 || v > 99999999999LL) {
        return false;
    }
    *out = v;
    return true;
}

/* ?from=&to= (epoch seconds, at most 8 days apart; default now and 7 days on):
 * {"from":..,"to":..,"slots":[{"med_id":..,"slot":..,"state":".."}]} ascending by slot; as-needed medicines have none */
static esp_err_t h_schedule(web_req_t *r)
{
    char v[24];
    int64_t now = (int64_t)time(NULL), from = now, to;
    bool has_to = false;
    if (web_query_get(r, "from", v, sizeof v) && !parse_ts(v, &from)) {
        return web_send_err(r, 400, "invalid", "from", "from is a time in seconds");
    }
    to = from + 7 * 86400;
    if (web_query_get(r, "to", v, sizeof v)) {
        if (!parse_ts(v, &to)) {
            return web_send_err(r, 400, "invalid", "to", "to is a time in seconds");
        }
        has_to = true;
    }
    if (to <= from || to - from > MAX_RANGE_S) {
        return web_send_err(r, 400, "invalid", has_to ? "to" : "from", "The range is between 1 second and 8 days");
    }

    hy_job_t *j = web_job_new(r, job_sched_meta, sizeof(smeta_t));
    if (!j) {
        return web_oom(r);
    }
    esp_err_t sent;
    smeta_t *mc = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
    if (!mc) {
        return sent;
    }
    smeta_t *meta = heap_caps_malloc(sizeof *meta, MALLOC_CAP_SPIRAM);
    sslot_t *all = heap_caps_malloc(HY_MAX_MEDS * SLOTS_PER_MED * sizeof *all, MALLOC_CAP_SPIRAM);
    if (!meta || !all) {
        heap_caps_free(meta);
        heap_caps_free(all);
        hy_job_put(j);
        return web_oom(r);
    }
    *meta = *mc;
    hy_job_put(j);
    if (meta->no_tracking) {
        heap_caps_free(meta);
        heap_caps_free(all);
        return web_fail(r, HY_E_CLOCK);
    }

    size_t total = 0;
    for (size_t at = 0; at < meta->n; at += SCHED_CHUNK) {
        j = web_job_new(r, job_sched_chunk, sizeof(schunk_t));
        if (!j) {
            heap_caps_free(meta);
            heap_caps_free(all);
            return web_oom(r);
        }
        schunk_t *c = hy_job_ctx(j);
        c->from = from;
        c->to = to;
        c->n_ids = meta->n - at < SCHED_CHUNK ? meta->n - at : SCHED_CHUNK;
        for (size_t i = 0; i < c->n_ids; i++) {
            c->ids[i] = meta->ids[at + i];
        }
        c = web_job_go(r, j, HY_JOB_TIMEOUT_MS, &sent);
        if (!c) {
            heap_caps_free(meta);
            heap_caps_free(all);
            return sent;
        }
        if (c->nomem) {
            hy_job_put(j);
            heap_caps_free(meta);
            heap_caps_free(all);
            return web_oom(r);
        }
        memcpy(all + total, c->slots, c->n * sizeof(sslot_t));
        total += c->n;
        hy_job_put(j);
    }
    heap_caps_free(meta);
    qsort(all, total, sizeof *all, slot_cmp);

    web_sb_t b = {0};
    web_sb_fmt(&b, "{\"from\":%lld,\"to\":%lld,\"slots\":[", (long long)from, (long long)to);
    for (size_t i = 0; i < total; i++) {
        uint8_t st = all[i].state;
        web_sb_fmt(&b, "%s{\"med_id\":%u,\"slot\":%lld,\"state\":\"%s\"}", i ? "," : "", (unsigned)all[i].med_id,
                   (long long)all[i].slot, OCC_NAME[st < sizeof OCC_NAME / sizeof OCC_NAME[0] ? st : 0]);
    }
    web_sb_str(&b, "]}");
    heap_caps_free(all);
    return web_sb_send(r, 200, &b);
}

/* ---------------------------------------------------------------- route table */

web_handler_t web_log_route(hy_route_id_t id)
{
    switch (id) {
    case HY_R_LOG: return h_log_get;
    case HY_R_LOG_NEW: return h_log_new;
    case HY_R_LOG_PUT: return h_log_put;
    case HY_R_LOG_DEL: return h_log_del;
    case HY_R_LOG_UNDO: return h_log_undo;
    case HY_R_HISTORY: return h_history;
    case HY_R_SCHEDULE: return h_schedule;
    default: return NULL;
    }
}
