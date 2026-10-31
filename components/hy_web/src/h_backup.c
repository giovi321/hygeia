/* Export and import.
 *
 * Both run in one worker task, detached from the httpd task with httpd_req_async_handler_begin, so every other
 * request (and the 409 "busy" answers) is served meanwhile. One backup operation at a time: a second one answers
 * 409 busy.
 *
 * Export: chunked response; each chunk is one LVGL job that reads at most 128 log entries after the cursor, then the
 * worker encodes and sends them. The JSON export writes "log" last.
 *
 * Import: the body (up to 4 MB) goes into PSRAM. hy_import_parse checks the whole file; for a merge, jobs collect the
 * stored entries of the file's date range into a PSRAM index (month by month, 128 entries per job) and the stored
 * gaps, and hy_import_plan_parsed compares against them without touching the store. Preview answers the counts.
 * The commit runs as jobs:
 *   merge:   upserts (10 medicines per job), hy_act_log_import (32 entries per job, one event each), gaps;
 *   replace: hy_store_import_begin, the staged medicines, entries (128 per job) and gaps, then one commit job that
 *            refuses with 409 "changed" when a dose was logged (next log id) or a medicine changed since the begin,
 *            else commits, reloads the actions and invalidates the scheduler.
 * Every job owns a copy of its input, so a job that runs late after a timeout never reads freed memory. While a
 * commit runs, other web writes answer 409 busy. */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hy/actions.h"
#include "hy/audio.h"
#include "hy/clock.h"
#include "hy/codec.h"
#include "hy/events.h"
#include "hy/import.h"
#include "hy/net.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "web_internal.h"

static const char *TAG = "web_backup";

#define IMPORT_MAX        (4 * 1024 * 1024)
#define PSRAM_MARGIN      (1024 * 1024)
#define WORKER_STACK      4096
#define WORKER_PRIO       4            /* below the httpd task */
#define EXPORT_ROWS       128
#define INDEX_ROWS        128
#define STAGE_ROWS        128
#define MERGE_ROWS        32           /* each append is one fsync: keeps a job near half a second */
#define MEDS_PER_JOB      10
#define GAPS_PER_JOB      128
#define INDEX_MAX         65536        /* stored entries compared against in one merge (16 bytes each) */
#define READ_JOB_MS       5000
#define WRITE_JOB_MS      15000
#define COMMIT_JOB_MS     30000
#define BODY_DEADLINE_US  (120 * 1000000LL)
#define LOG_FROM          (-62167219200LL)   /* 0000-01-01 UTC */
#define LOG_TO            253402300800LL      /* 10000-01-01 UTC */

typedef enum { W_CSV = 1, W_JSON, W_IMPORT } wkind_t;

typedef struct {
    wkind_t kind;
    httpd_req_t *req;          /* the async copy */
    bool secrets;
    hy_import_mode_t mode;
    bool preview, settings;
    size_t len;
} wjob_t;

static atomic_int s_state;           /* 0 idle, else the wkind_t running */
static atomic_bool s_committing;     /* an import writes: other writes answer 409 busy */
static atomic_bool s_abort;          /* the server is stopping: the worker ends at its next step */
static atomic_bool s_stopping;       /* no new export or import until web_backup_allow */

bool web_backup_busy(void)
{
    return atomic_load(&s_committing);
}

/* The worker holds an async request of the server, so httpd_stop must wait for it however long its last job takes.
 * start_worker claims s_state before it reads s_stopping, and this sets s_stopping before it reads s_state, so
 * either the new worker sees the stop and gives up, or the wait below sees the worker. */
void web_backup_stop(void)
{
    atomic_store(&s_stopping, true);
    if (!atomic_load(&s_state)) {
        return;
    }
    atomic_store(&s_abort, true);
    int64_t t0 = esp_timer_get_time();
    int64_t next_log = t0 + 10000000;
    while (atomic_load(&s_state)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (esp_timer_get_time() >= next_log) {
            ESP_LOGW(TAG, "server stop waits for the backup worker (%lld s)",
                     (long long)((esp_timer_get_time() - t0) / 1000000));
            next_log += 10000000;
        }
    }
}

void web_backup_allow(void)
{
    atomic_store(&s_stopping, false);
}

/* A wipe the compiler cannot drop before the following free. */
static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) {
        *v++ = 0;
    }
}

/* ---------------------------------------------------------------- jobs */

static void *psram_calloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}

/* Runs fn on a private copy of io (size bytes) and copies the context back on success. A job that times out may run
 * later on its own copy; the caller's buffer is never touched after the return. */
static esp_err_t run_job(void (*fn)(void *), void *io, size_t size, uint32_t timeout_ms)
{
    hy_job_t *j = hy_job_new(fn, size);
    if (!j) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(hy_job_ctx(j), io, size);
    esp_err_t e = hy_job_run(j, timeout_ms);
    if (e == ESP_OK) {
        memcpy(io, hy_job_ctx(j), size);
    }
    hy_job_put(j);
    return e;
}

/* ---------------------------------------------------------------- answers from the worker */

static esp_err_t send_err(httpd_req_t *req, int status, const char *code, const char *field, const char *msg)
{
    web_req_t r = {.req = req};
    if (status == 503) {
        httpd_resp_set_hdr(req, "Retry-After", "2");
    }
    return web_send_err(&r, status, code, field, msg);
}

static esp_err_t send_api_err(httpd_req_t *req, const hy_api_err_t *e)
{
    web_req_t r = {.req = req};
    return web_fail_api(&r, e);
}

static void file_name(char *out, size_t len, const char *ext)
{
    time_t now = time(NULL);
    struct tm tm;
    if (hy_clock_valid() && localtime_r(&now, &tm)) {
        snprintf(out, len, "attachment; filename=\"hygeia-%04d%02d%02d.%s\"", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, ext);
    } else {
        snprintf(out, len, "attachment; filename=\"hygeia.%s\"", ext);
    }
}

/* ---------------------------------------------------------------- export */

typedef struct {
    hy_med_t meds[HY_MAX_STORED];
    size_t n_meds;
    uint32_t next_id;
    hy_gap_t gaps[HY_IMPORT_GAPS_MAX];
    uint8_t reasons[HY_IMPORT_GAPS_MAX];
    size_t n_gaps;
    hy_settings_t settings;
} xhead_t;

static void job_export_head(void *arg)
{
    xhead_t *c = arg;
    const hy_med_t *m;
    size_t n = hy_meds(&m);
    if (n > HY_MAX_STORED) {
        n = HY_MAX_STORED;
    }
    memcpy(c->meds, m, n * sizeof *m);
    c->n_meds = n;
    const hy_store_port_t *st = hy_store_port();
    c->next_id = st->load_next_id ? st->load_next_id(NULL) : 0;
    c->n_gaps = st->query_gaps ? st->query_gaps(NULL, LOG_FROM, LOG_TO, c->gaps, c->reasons, HY_IMPORT_GAPS_MAX) : 0;
    c->settings = *hy_settings();
}

typedef struct {
    int64_t after_ts;
    uint32_t after_id;
    bool started;      /* the cursor is set */
    bool stuck;        /* a full page that the cursor filtered away: more than a page share one second */
    size_t n;
    hy_log_t rows[EXPORT_ROWS];
} xpage_t;

static void job_export_page(void *arg)
{
    xpage_t *c = arg;
    size_t n = hy_store_port()->query_log(NULL, c->started ? c->after_ts : LOG_FROM, LOG_TO, 0, c->rows, EXPORT_ROWS);
    size_t kept = c->started ? hy_api_page_filter(c->rows, n, c->after_ts, c->after_id) : n;
    c->stuck = n == EXPORT_ROWS && kept == 0;
    c->n = kept;
}

/* Next page into *pg; false when the job failed (busy or no memory). */
static bool export_next(xpage_t *pg)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        if (atomic_load(&s_abort)) {
            return false;
        }
        esp_err_t e = run_job(job_export_page, pg, sizeof *pg, READ_JOB_MS);
        if (e == ESP_OK) {
            if (pg->stuck) {   /* skip the crowded second rather than loop on it */
                ESP_LOGW(TAG, "more than %d entries at %lld: export skips the rest of that second", EXPORT_ROWS,
                         (long long)pg->after_ts);
                pg->after_ts++;
                pg->after_id = 0;
                pg->n = 0;
                continue;
            }
            if (pg->n) {
                pg->after_ts = pg->rows[pg->n - 1].ts;
                pg->after_id = pg->rows[pg->n - 1].id;
                pg->started = true;
            }
            return true;
        }
        if (e == ESP_ERR_NO_MEM) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}

static const hy_med_t *med_in(const xhead_t *h, uint32_t id)
{
    for (size_t i = 0; i < h->n_meds; i++) {
        if (h->meds[i].id == id) {
            return &h->meds[i];
        }
    }
    return NULL;
}

static void do_export(wjob_t *w)
{
    httpd_req_t *req = w->req;
    xhead_t *h = psram_calloc(sizeof *h);
    xpage_t *pg = psram_calloc(sizeof *pg);
    char *line = psram_calloc(HY_IMPORT_ENTRY_MAX + 2);
    web_sb_t sb = {0};
    bool ok = h && pg && line;
    if (!ok) {
        send_err(req, 503, "busy", NULL, "The device is busy, try again");
        goto out;
    }
    if (run_job(job_export_head, h, sizeof *h, READ_JOB_MS) != ESP_OK) {
        send_err(req, 503, "busy", NULL, "The device is busy, try again");
        ok = false;
        goto out;
    }

    char disp[64];
    bool json = w->kind == W_JSON;
    char *head = NULL;
    if (json) {
        hy_backup_wifi_t *wifi = NULL;
        if (w->secrets) {
            wifi = psram_calloc(sizeof *wifi);
            if (!wifi) {   /* never a file without the passwords the user asked for */
                send_err(req, 503, "busy", NULL, "The device is busy, try again");
                goto out;
            }
            hy_known_net_t k[HY_NET_MAX_KNOWN];
            size_t n = hy_net_known(k);
            for (size_t i = 0; i < n && i < HY_NET_MAX_KNOWN; i++) {
                if (hy_net_pass_get(k[i].ssid, wifi->pass[wifi->n]) == ESP_OK) {
                    strlcpy(wifi->ssid[wifi->n], k[i].ssid, sizeof wifi->ssid[0]);
                    wifi->n++;
                }
            }
            hy_net_ap_pass_get(wifi->ap_pass);
        }
        hy_export_head_t eh = {
            .exported = hy_clock_valid() ? (int64_t)time(NULL) : 0,
            .meds = h->meds, .n_meds = h->n_meds, .next_id = h->next_id,
            .gaps = h->gaps, .gap_reasons = h->reasons, .n_gaps = h->n_gaps,
            .settings = &h->settings, .wifi = wifi,
        };
        head = hy_export_head_json(&eh);
        if (wifi) {
            wipe(wifi, sizeof *wifi);
            heap_caps_free(wifi);
        }
        if (!head) {
            send_err(req, 503, "busy", NULL, "The device is busy, try again");
            goto out;
        }
    }

    /* the download headers only once the head exists: an error before this is a plain JSON answer */
    file_name(disp, sizeof disp, json ? "json" : "csv");
    httpd_resp_set_type(req, json ? "application/json" : "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_status(req, "200 OK");
    if (json) {
        ok = httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN) == ESP_OK;
        wipe(head, strlen(head));   /* it can hold Wi-Fi passwords */
        free(head);
    } else {
        ok = httpd_resp_send_chunk(req, HY_EXPORT_CSV_HEADER "\r\n", HTTPD_RESP_USE_STRLEN) == ESP_OK;
    }

    size_t total = 0, written = 0;
    while (ok) {
        if (!export_next(pg)) {
            ok = false;
            break;
        }
        if (pg->n == 0) {
            break;
        }
        sb.len = 0;
        for (size_t i = 0; i < pg->n; i++) {
            int n = json ? hy_log_to_jsonl(&pg->rows[i], line, HY_IMPORT_ENTRY_MAX)
                         : hy_api_log_csv(&pg->rows[i], med_in(h, pg->rows[i].med_id), line, HY_IMPORT_ENTRY_MAX);
            if (n < 0) {
                continue;   /* a record the encoder refuses is left out, as the store skips a bad line */
            }
            if (json && written > 0) {
                web_sb_add(&sb, ",", 1);
            }
            written++;
            web_sb_add(&sb, line, (size_t)n);
            if (!json) {
                web_sb_add(&sb, "\r\n", 2);
            }
        }
        total += pg->n;
        if (sb.bad) {
            ok = false;
            break;
        }
        if (sb.len && httpd_resp_send_chunk(req, sb.p, sb.len) != ESP_OK) {
            ok = false;
        }
    }
    if (ok && json) {
        ok = httpd_resp_send_chunk(req, HY_EXPORT_TAIL, HTTPD_RESP_USE_STRLEN) == ESP_OK;
    }
    if (ok) {
        ok = httpd_resp_send_chunk(req, NULL, 0) == ESP_OK;
    }
    ESP_LOGI(TAG, "export %s: %u entries%s", json ? "json" : "csv", (unsigned)total, ok ? "" : ", cut short");
out:
    if (!ok) {
        /* the status line may be out already: closing the socket is the only way to tell the client */
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    }
    web_sb_free(&sb);
    heap_caps_free(line);
    heap_caps_free(pg);
    heap_caps_free(h);
}

/* ---------------------------------------------------------------- import: shared pieces */

typedef struct {
    int64_t ts;
    uint32_t med;
    uint8_t st;
} ikey_t;

typedef struct {
    ikey_t *v;
    size_t n, cap;
} index_t;

static int ikey_cmp(const void *a, const void *b)
{
    const ikey_t *x = a, *y = b;
    if (x->med != y->med) {
        return x->med < y->med ? -1 : 1;
    }
    if (x->ts != y->ts) {
        return x->ts < y->ts ? -1 : 1;
    }
    return (int)x->st - (int)y->st;
}

static bool index_exists(void *ctx, uint32_t med, int64_t ts, hy_status_t st)
{
    const index_t *ix = ctx;
    ikey_t k = {.ts = ts, .med = med, .st = (uint8_t)st};
    return ix->n && bsearch(&k, ix->v, ix->n, sizeof k, ikey_cmp) != NULL;
}

typedef struct {
    hy_med_t meds[HY_MAX_STORED];
    size_t n;
    uint32_t next_log_id;
    uint32_t fingerprint;
} icur_t;

/* Ids and etags of every medicine: changes when the device edits, adds or removes one. */
static uint32_t meds_fingerprint(const hy_med_t *m, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        uint32_t parts[2] = {m[i].id, hy_api_med_etag(&m[i])};
        const uint8_t *b = (const uint8_t *)parts;
        for (size_t k = 0; k < sizeof parts; k++) {
            h = (h ^ b[k]) * 16777619u;
        }
    }
    return h;
}

static void job_current(void *arg)
{
    icur_t *c = arg;
    const hy_med_t *m;
    size_t n = hy_meds(&m);
    if (n > HY_MAX_STORED) {
        n = HY_MAX_STORED;
    }
    memcpy(c->meds, m, n * sizeof *m);
    c->n = n;
    c->next_log_id = hy_store_next_log_id();
    c->fingerprint = meds_fingerprint(m, n);
}

typedef struct {
    int64_t from, to;
    int64_t last_ts;
    uint32_t last_id;
    bool have;
    size_t n_raw, n;
    hy_log_t rows[INDEX_ROWS];
} ipage_t;

static void job_index_page(void *arg)
{
    ipage_t *c = arg;
    c->n_raw = hy_store_port()->query_log(NULL, c->from, c->to, 0, c->rows, INDEX_ROWS);
    c->n = c->have ? hy_api_page_filter(c->rows, c->n_raw, c->last_ts, c->last_id) : c->n_raw;
}

/* Stored entries in [from, to) into the index, 128 per job. */
static hy_err_t build_index(index_t *ix, int64_t from, int64_t to, ipage_t *pg)
{
    memset(pg, 0, sizeof *pg);
    pg->from = from;
    pg->to = to;
    for (;;) {
        if (atomic_load(&s_abort)) {
            return HY_E_IO;
        }
        if (run_job(job_index_page, pg, sizeof *pg, READ_JOB_MS) != ESP_OK) {
            return HY_E_IO;
        }
        if (ix->n + pg->n > INDEX_MAX) {
            return HY_E_FULL;
        }
        if (ix->n + pg->n > ix->cap) {
            size_t cap = ix->cap ? ix->cap * 2 : 1024;
            while (cap < ix->n + pg->n) {
                cap *= 2;
            }
            ikey_t *v = heap_caps_realloc(ix->v, cap * sizeof *v, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!v) {
                return HY_E_IO;
            }
            ix->v = v;
            ix->cap = cap;
        }
        for (size_t i = 0; i < pg->n; i++) {
            ix->v[ix->n++] = (ikey_t){.ts = pg->rows[i].ts, .med = pg->rows[i].med_id, .st = (uint8_t)pg->rows[i].status};
        }
        if (pg->n_raw < INDEX_ROWS) {
            break;
        }
        if (pg->n == 0) {   /* a whole page at one second: step past it */
            pg->from = pg->last_ts + 1;
            pg->have = false;
            continue;
        }
        pg->last_ts = pg->rows[pg->n - 1].ts;
        pg->last_id = pg->rows[pg->n - 1].id;
        pg->have = true;
        pg->from = pg->last_ts;
    }
    qsort(ix->v, ix->n, sizeof *ix->v, ikey_cmp);
    return HY_OK;
}

typedef struct {
    hy_gap_t gaps[HY_IMPORT_GAPS_MAX];
    size_t n;
} igaps_t;

static void job_stored_gaps(void *arg)
{
    igaps_t *c = arg;
    const hy_store_port_t *st = hy_store_port();
    c->n = st->query_gaps ? st->query_gaps(NULL, LOG_FROM, LOG_TO, c->gaps, NULL, HY_IMPORT_GAPS_MAX) : 0;
}

/* Settings from the file merged onto the live ones, as PUT /api/settings does. */
typedef struct {
    char json[1024];
    bool failed;
    esp_err_t save;
} iset_t;

static void job_settings(void *arg)
{
    iset_t *c = arg;
    hy_settings_t old = *hy_settings();
    hy_settings_t next;
    hy_api_err_t ae;
    if (hy_api_settings_from_json(c->json, &old, &next, &ae) != HY_OK) {
        c->failed = true;
        return;
    }
    next.rot = old.rot;
    c->save = hy_settings_update(&next);
    hy_settings_t now = *hy_settings();
    hy_audio_set_volume(now.volume);
    hy_net_set_keep_batt(now.wifi_keep_batt);
    const hy_web_ui_t *ui = web_ui();
    if (ui && ui->settings_applied) {
        ui->settings_applied(&old, &now);
    }
}

/* ---------------------------------------------------------------- import: merge commit */

typedef struct {
    hy_med_t m[MEDS_PER_JOB];
    uint8_t kind[MEDS_PER_JOB];
    size_t n;
    size_t done;             /* records written */
    uint32_t new_id[MEDS_PER_JOB];
    hy_err_t err;
} imeds_t;

static void job_merge_meds(void *arg)
{
    imeds_t *c = arg;
    c->err = HY_OK;
    for (c->done = 0; c->done < c->n; c->done++) {
        hy_med_t m = c->m[c->done];
        if (c->kind[c->done] == HY_IMP_ADD) {
            m.id = 0;   /* a fresh id: upsert refuses an unknown nonzero id */
        }
        c->err = hy_act_med_upsert(&m);
        if (c->err != HY_OK) {
            return;
        }
        c->new_id[c->done] = m.id;
    }
}

typedef struct {
    hy_log_t rows[STAGE_ROWS];
    size_t n;
    size_t done;
    hy_err_t err;
} ilog_t;

static void job_merge_log(void *arg)
{
    ilog_t *c = arg;
    c->err = HY_OK;
    for (c->done = 0; c->done < c->n; c->done++) {
        c->err = hy_act_log_import(&c->rows[c->done]);
        if (c->err != HY_OK) {
            break;
        }
    }
    if (c->done) {
        hy_act_log_import_end();
    }
}

typedef struct {
    hy_gap_t g[GAPS_PER_JOB];
    uint8_t r[GAPS_PER_JOB];
    size_t n;
    bool staged;             /* replace: into the staging area; merge: hy_act_record_gap */
    hy_err_t err;
} igapw_t;

static void job_gaps(void *arg)
{
    igapw_t *c = arg;
    c->err = HY_OK;
    for (size_t i = 0; i < c->n && c->err == HY_OK; i++) {
        c->err = c->staged ? hy_store_import_gap(&c->g[i], (hy_gap_reason_t)c->r[i])
                           : hy_act_record_gap(&c->g[i], (hy_gap_reason_t)c->r[i]);
    }
}

static void job_invalidate(void *arg)
{
    (void)arg;
    hy_sched_run_invalidate();
}

/* ---------------------------------------------------------------- import: replace commit */

typedef struct {
    size_t bytes;
    uint32_t next_log_id;
    uint32_t fingerprint;
    hy_err_t err;
    bool pending;      /* an earlier commit is unfinished and could not be completed now */
} ibegin_t;

/* The device reloads after a store change it did not make through the actions. */
static hy_err_t reload_after_store_change(void)
{
    hy_err_t r = hy_actions_init(hy_store_port(), NULL, hy_clock_port(), NULL);
    if (r != HY_OK) {
        ESP_LOGE(TAG, "actions reload: %s", hy_err_str(r));
    }
    hy_sched_run_invalidate();
    int64_t now = hy_clock_valid() ? (int64_t)time(NULL) : 0;
    hy_event_t ev = {.type = HY_EV_MEDS_CHANGED, .ts = now};
    hy_ev_publish(&ev);
    ev.type = HY_EV_LOG_EDITED;
    hy_ev_publish(&ev);
    return r;
}

static void job_stage_begin(void *arg)
{
    ibegin_t *c = arg;
    bool was_pending = hy_store_import_pending();
    c->err = hy_store_import_begin(c->bytes);
    c->pending = hy_store_import_pending();
    if (was_pending && !c->pending) {
        ESP_LOGW(TAG, "an earlier unfinished import commit was completed");
        (void)reload_after_store_change();
    }
    /* read after the begin: a completed earlier commit changes both */
    const hy_med_t *m;
    size_t n = hy_meds(&m);
    c->next_log_id = hy_store_next_log_id();
    c->fingerprint = meds_fingerprint(m, n);
}

typedef struct {
    hy_med_t m[HY_MAX_STORED];
    size_t n;
    uint32_t next_id;
    hy_err_t err;
} istmeds_t;

static void job_stage_meds(void *arg)
{
    istmeds_t *c = arg;
    c->err = hy_store_import_meds(c->m, c->n, c->next_id);
}

static void job_stage_log(void *arg)
{
    ilog_t *c = arg;
    c->err = hy_store_import_log(c->rows, c->n);
    c->done = c->err == HY_OK ? c->n : 0;
}

static void job_stage_abort(void *arg)
{
    (void)arg;
    hy_store_import_abort();
}

typedef struct {
    uint32_t next_log_id;
    uint32_t fingerprint;
    bool changed;
    bool untouched;    /* the commit failed before its marker: the live data is as it was */
    bool pending;      /* the commit stopped after its marker: a restart or the next import completes it */
    hy_err_t err;
    hy_err_t reload;
} icommit_t;

static void job_stage_commit(void *arg)
{
    icommit_t *c = arg;
    const hy_med_t *m;
    size_t n = hy_meds(&m);
    if (hy_store_next_log_id() != c->next_log_id || meds_fingerprint(m, n) != c->fingerprint) {
        c->changed = true;   /* a dose or an edit on the device since the begin would be lost */
        hy_store_import_abort();
        return;
    }
    c->err = hy_store_import_commit();   /* re-runs the store's open logic */
    if (c->err != HY_OK && hy_store_import_staging()) {
        /* failed before its marker: nothing was replaced */
        c->untouched = true;
        hy_store_import_abort();
        return;
    }
    c->pending = hy_store_import_pending();
    /* The store was reopened: the actions must reload what is on disk now. */
    c->reload = reload_after_store_change();
}

/* ---------------------------------------------------------------- import: worker */

typedef struct {
    hy_import_t imp;
    hy_import_diff_t diff;
    hy_import_cursor_t cur;
    icur_t now;
    index_t ix;
    ipage_t page;
    bool gap_new[HY_IMPORT_GAPS_MAX];
    union {
        imeds_t meds;
        ilog_t log;
        igapw_t gaps;
        istmeds_t stmeds;
        igaps_t stored;
        iset_t set;
    } u;
} iwork_t;

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t len)
{
    size_t got = 0;
    int timeouts = 0;
    int64_t deadline = esp_timer_get_time() + BODY_DEADLINE_US;
    while (got < len) {
        if (atomic_load(&s_abort) || esp_timer_get_time() > deadline) {
            return ESP_ERR_TIMEOUT;
        }
        int n = httpd_req_recv(req, buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= 2) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        timeouts = 0;
        got += (size_t)n;
    }
    buf[len] = 0;
    return ESP_OK;
}

static const char *mode_name(hy_import_mode_t m)
{
    return m == HY_IMPORT_REPLACE ? "replace" : "merge";
}

static esp_err_t send_counts(httpd_req_t *req, const wjob_t *w, const hy_import_diff_t *d, int64_t exported,
                             bool settings_applied, bool secrets_applied)
{
    char buf[400];
    int n = snprintf(buf, sizeof buf,
                     "{\"%s\":true,\"mode\":\"%s\",\"exported\":%lld,"
                     "\"meds\":{\"add\":%u,\"change\":%u,\"same\":%u},"
                     "\"log\":{\"add\":%u,\"dup\":%u,\"skip\":%u},\"gaps\":{\"add\":%u},"
                     "\"settings\":%s,\"secrets\":%s,\"settings_applied\":%s,\"secrets_applied\":%s}",
                     w->preview ? "preview" : "imported", mode_name(w->mode), (long long)exported,
                     (unsigned)d->meds_add, (unsigned)d->meds_change, (unsigned)d->meds_same, (unsigned)d->log_add,
                     (unsigned)d->log_dup, (unsigned)d->log_skip, (unsigned)d->gaps_add, d->settings ? "true" : "false",
                     d->secrets ? "true" : "false", settings_applied ? "true" : "false",
                     secrets_applied ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof buf) {
        return send_err(req, 500, "internal", NULL, "Answer too long");
    }
    web_req_t r = {.req = req};
    return web_send_json(&r, 200, buf);
}

/* A merge that stops partway keeps what it wrote: the answer says so, because the same file then reports those
 * medicines as clashes and those entries as duplicates. */
#define PARTIAL_MSG                                                                                              \
    "The import stopped partway: some medicines or entries were already added. Importing the same file again " \
    "reports them as duplicates; check the medicines and the log"

static esp_err_t send_partial(httpd_req_t *req, int status, const char *code, const char *msg)
{
    return send_err(req, status, code, NULL, msg);
}

/* An action error partway through a merge: its code and status, with the partial message */
static esp_err_t send_partial_err(httpd_req_t *req, hy_err_t e)
{
    hy_api_err_t ae;
    hy_api_err_from(e, &ae);
    return send_err(req, ae.http, ae.code, ae.field[0] ? ae.field : NULL, PARTIAL_MSG);
}

/* Merge commit. Returns ESP_OK when everything was written; otherwise the answer is already sent. */
static esp_err_t merge_commit(httpd_req_t *req, iwork_t *k)
{
    hy_import_t *imp = &k->imp;
    /* medicines */
    for (size_t i = 0; i < imp->n_meds;) {
        imeds_t *c = &k->u.meds;
        memset(c, 0, sizeof *c);
        size_t idx[MEDS_PER_JOB];
        while (i < imp->n_meds && c->n < MEDS_PER_JOB) {
            if (imp->map[i].kind != HY_IMP_SAME) {
                c->m[c->n] = imp->meds[i];
                c->kind[c->n] = imp->map[i].kind;
                idx[c->n++] = i;
            }
            i++;
        }
        if (!c->n) {
            continue;
        }
        if (atomic_load(&s_abort) || run_job(job_merge_meds, c, sizeof *c, WRITE_JOB_MS) != ESP_OK) {
            return send_partial(req, 503, "busy", PARTIAL_MSG);
        }
        for (size_t j = 0; j < c->done; j++) {
            if (c->kind[j] == HY_IMP_ADD) {
                hy_import_set_dev_id(imp, imp->meds[idx[j]].id, c->new_id[j]);
            }
        }
        if (c->err != HY_OK) {
            ESP_LOGW(TAG, "merge: medicine %s refused: %s", c->m[c->done].code, hy_err_str(c->err));
            return send_partial_err(req, c->err);
        }
    }
    /* log */
    hy_import_log_begin(imp, &k->cur);
    hy_log_t e;
    int got;
    bool more = true;
    while (more) {
        ilog_t *c = &k->u.log;
        c->n = 0;
        while (c->n < MERGE_ROWS && (got = hy_import_log_next(&k->cur, &e)) == 1) {
            uint32_t dev;
            uint8_t kind;
            if (!hy_import_target(imp, e.med_id, &dev, &kind)) {
                continue;   /* not in the file's medicines, or an ADD the upsert refused */
            }
            if (kind != HY_IMP_ADD && index_exists(&k->ix, dev, e.ts, e.status)) {
                continue;   /* duplicate */
            }
            e.med_id = dev;
            c->rows[c->n++] = e;
        }
        more = c->n == MERGE_ROWS;
        if (!c->n) {
            break;
        }
        if (atomic_load(&s_abort) || run_job(job_merge_log, c, sizeof *c, WRITE_JOB_MS) != ESP_OK) {
            return send_partial(req, 503, "busy", PARTIAL_MSG);
        }
        if (c->err != HY_OK) {
            return send_partial_err(req, c->err);
        }
    }
    /* gaps not stored yet */
    for (size_t i = 0; i < imp->n_gaps;) {
        igapw_t *c = &k->u.gaps;
        memset(c, 0, sizeof *c);
        while (i < imp->n_gaps && c->n < GAPS_PER_JOB) {
            if (k->gap_new[i]) {
                c->g[c->n] = imp->gaps[i];
                c->r[c->n++] = imp->gap_reasons[i];
            }
            i++;
        }
        if (!c->n) {
            continue;
        }
        if (atomic_load(&s_abort) || run_job(job_gaps, c, sizeof *c, WRITE_JOB_MS) != ESP_OK || c->err != HY_OK) {
            return send_partial(req, 503, "storage", PARTIAL_MSG);
        }
    }
    uint8_t none = 0;
    run_job(job_invalidate, &none, sizeof none, READ_JOB_MS);
    return ESP_OK;
}

static void stage_abort(void)
{
    uint8_t none = 0;
    for (int i = 0; i < 3; i++) {
        if (run_job(job_stage_abort, &none, sizeof none, WRITE_JOB_MS) == ESP_OK) {
            return;
        }
    }
    ESP_LOGW(TAG, "staging area left behind: the next import or boot removes it");
}

/* Replace commit. ESP_OK when committed; otherwise the answer is already sent and the live data is untouched. */
static esp_err_t replace_commit(httpd_req_t *req, iwork_t *k, size_t bytes)
{
    hy_import_t *imp = &k->imp;
    ibegin_t b = {.bytes = bytes};
    if (atomic_load(&s_abort) || run_job(job_stage_begin, &b, sizeof b, WRITE_JOB_MS) != ESP_OK) {
        stage_abort();
        return send_err(req, 503, "busy", NULL, "The device is busy, try again");
    }
    if (b.pending) {   /* the staging area of that commit is kept as it is: stage_abort would not remove it either */
        return send_err(req, 503, "storage", NULL, "An earlier import is unfinished. Restart the device");
    }
    if (b.err == HY_E_FULL) {
        return send_err(req, 409, "full", NULL, "Not enough free storage for this file");
    }
    if (b.err != HY_OK) {
        stage_abort();
        return send_err(req, 503, "storage", NULL, "The device could not prepare the import");
    }
    istmeds_t *sm = &k->u.stmeds;
    memcpy(sm->m, imp->meds, imp->n_meds * sizeof imp->meds[0]);
    sm->n = imp->n_meds;
    sm->next_id = imp->next_id;
    if (atomic_load(&s_abort) || run_job(job_stage_meds, sm, sizeof *sm, WRITE_JOB_MS) != ESP_OK || sm->err != HY_OK) {
        stage_abort();
        return send_err(req, 503, "storage", NULL, "The medicines could not be written");
    }
    hy_import_log_begin(imp, &k->cur);
    hy_log_t e;
    bool more = true;
    while (more) {
        ilog_t *c = &k->u.log;
        c->n = 0;
        while (c->n < STAGE_ROWS && hy_import_log_next(&k->cur, &e) == 1) {
            if (hy_import_target(imp, e.med_id, NULL, NULL)) {
                c->rows[c->n++] = e;
            }
        }
        more = c->n == STAGE_ROWS;
        if (!c->n) {
            break;
        }
        if (atomic_load(&s_abort) || run_job(job_stage_log, c, sizeof *c, WRITE_JOB_MS) != ESP_OK ||
            c->err != HY_OK) {
            stage_abort();
            return send_err(req, 503, "storage", NULL, "The log could not be written");
        }
    }
    for (size_t i = 0; i < imp->n_gaps;) {
        igapw_t *c = &k->u.gaps;
        memset(c, 0, sizeof *c);
        c->staged = true;
        while (i < imp->n_gaps && c->n < GAPS_PER_JOB) {
            c->g[c->n] = imp->gaps[i];
            c->r[c->n++] = imp->gap_reasons[i];
            i++;
        }
        if (atomic_load(&s_abort) || run_job(job_gaps, c, sizeof *c, WRITE_JOB_MS) != ESP_OK || c->err != HY_OK) {
            stage_abort();
            return send_err(req, 503, "storage", NULL, "The untracked periods could not be written");
        }
    }
    if (atomic_load(&s_abort)) {
        stage_abort();
        return ESP_FAIL;
    }
    icommit_t cm = {.next_log_id = b.next_log_id, .fingerprint = b.fingerprint};
    if (run_job(job_stage_commit, &cm, sizeof cm, COMMIT_JOB_MS) != ESP_OK) {
        /* The commit job may still run: it either commits or finds a change and aborts; never both halves. */
        return send_err(req, 503, "busy", NULL, "The device is busy: check the medicines and the log before retrying");
    }
    if (cm.changed) {
        return send_err(req, 409, "changed", NULL,
                        "Something was logged or edited on the device during the import: nothing was replaced");
    }
    if (cm.untouched) {
        ESP_LOGE(TAG, "import commit: %s before any change", hy_err_str(cm.err));
        return send_err(req, 503, "storage", NULL, "The import could not be completed: nothing was replaced");
    }
    if (cm.err != HY_OK || cm.reload != HY_OK) {
        ESP_LOGE(TAG, "import commit: %s, reload: %s, unfinished: %d", hy_err_str(cm.err), hy_err_str(cm.reload),
                 (int)cm.pending);
        return send_err(req, 503, "storage", NULL,
                        cm.pending ? "The import did not finish. Restart the device to complete it; the previous "
                                     "data is kept in pre-import"
                                   : "The import was written but the device could not reload it. Restart the device");
    }
    return ESP_OK;
}

/* Settings and secrets of the file, only with settings=1. */
static void apply_settings(iwork_t *k, bool *settings_ok, bool *secrets_ok)
{
    hy_import_t *imp = &k->imp;
    if (imp->settings && strlen(imp->settings) < sizeof k->u.set.json) {
        iset_t *c = &k->u.set;
        memset(c, 0, sizeof *c);
        strcpy(c->json, imp->settings);
        *settings_ok = run_job(job_settings, c, sizeof *c, WRITE_JOB_MS) == ESP_OK && !c->failed && c->save == ESP_OK;
    }
    if (imp->has_wifi) {
        bool ok = true;
        for (size_t i = 0; i < imp->wifi.n; i++) {
            if (hy_net_add(imp->wifi.ssid[i], imp->wifi.pass[i]) != ESP_OK) {
                ok = false;
            }
        }
        if (imp->wifi.ap_pass[0] && hy_net_ap_pass_set(imp->wifi.ap_pass) != ESP_OK) {
            ok = false;
        }
        *secrets_ok = ok;
    }
}

static void do_import(wjob_t *w)
{
    httpd_req_t *req = w->req;
    char *body = heap_caps_malloc(w->len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    iwork_t *k = psram_calloc(sizeof *k);
    if (!body || !k) {
        send_err(req, 413, "too_large", NULL, "Not enough memory for this file");
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
        goto out;
    }
    esp_err_t rb = read_body(req, body, w->len);
    if (rb != ESP_OK) {
        if (rb == ESP_ERR_TIMEOUT) {
            send_err(req, 408, "timeout", NULL, "The file did not arrive in time");
        }
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
        goto out;
    }

    hy_api_err_t ae;
    if (hy_import_parse(body, &k->imp, &ae) != HY_OK) {
        send_api_err(req, &ae);
        goto out;
    }
    if (run_job(job_current, &k->now, sizeof k->now, READ_JOB_MS) != ESP_OK) {
        send_err(req, 503, "busy", NULL, "The device is busy, try again");
        goto out;
    }

    if (w->mode == HY_IMPORT_MERGE) {
        /* stored entries over the file's date range, and the stored gaps */
        int64_t lo = 0, hi = 0;
        bool any = false;
        hy_log_t e;
        hy_import_log_begin(&k->imp, &k->cur);
        while (hy_import_log_next(&k->cur, &e) == 1) {
            if (!any || e.ts < lo) lo = e.ts;
            if (!any || e.ts > hi) hi = e.ts;
            any = true;
        }
        if (any) {
            hy_err_t ir = build_index(&k->ix, lo, hi + 1, &k->page);
            if (ir == HY_E_FULL) {
                send_err(req, 413, "too_large", NULL, "Too many stored entries in the file's date range to compare");
                goto out;
            }
            if (ir != HY_OK) {
                send_err(req, 503, "busy", NULL, "The device is busy, try again");
                goto out;
            }
        }
        if (k->imp.n_gaps) {
            if (run_job(job_stored_gaps, &k->u.stored, sizeof k->u.stored, READ_JOB_MS) != ESP_OK) {
                send_err(req, 503, "busy", NULL, "The device is busy, try again");
                goto out;
            }
        }
    }

    if (hy_import_plan_parsed(&k->imp, w->mode, k->now.meds, k->now.n, index_exists, &k->ix, &k->diff, &ae) !=
        HY_OK) {
        send_api_err(req, &ae);
        goto out;
    }
    if (w->mode == HY_IMPORT_MERGE) {
        size_t add = 0;
        for (size_t i = 0; i < k->imp.n_gaps; i++) {
            bool stored = false;
            for (size_t j = 0; j < k->u.stored.n && !stored; j++) {
                stored = k->u.stored.gaps[j].start == k->imp.gaps[i].start &&
                         k->u.stored.gaps[j].end == k->imp.gaps[i].end;
            }
            k->gap_new[i] = !stored;
            add += !stored;
        }
        k->diff.gaps_add = add;
    }
    if (w->settings && k->imp.settings && strlen(k->imp.settings) >= sizeof k->u.set.json) {
        send_err(req, 413, "too_large", "settings", "The settings in this file are too large to apply");
        goto out;
    }
    if (w->preview) {
        send_counts(req, w, &k->diff, k->imp.exported, false, false);
        goto out;
    }

    atomic_store(&s_committing, true);
    web_ui_toast_info("Importing");
    ESP_LOGI(TAG, "import %s: %u medicines, %u entries, %u gaps", mode_name(w->mode), (unsigned)k->imp.n_meds,
             (unsigned)k->imp.n_log, (unsigned)k->imp.n_gaps);
    esp_err_t c = w->mode == HY_IMPORT_REPLACE ? replace_commit(req, k, w->len) : merge_commit(req, k);
    bool set_ok = false, sec_ok = false;
    if (c == ESP_OK && w->settings) {
        apply_settings(k, &set_ok, &sec_ok);
    }
    atomic_store(&s_committing, false);
    if (c == ESP_OK) {
        web_ui_toast_info("Import done");
        send_counts(req, w, &k->diff, k->imp.exported, set_ok, sec_ok);
        ESP_LOGI(TAG, "import done");
    } else {
        web_ui_toast_info("Import failed");
        if (c == ESP_FAIL) {
            httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
        }
    }
out:
    if (k) {
        hy_import_free(&k->imp);
        heap_caps_free(k->ix.v);
        wipe(&k->u, sizeof k->u);
        heap_caps_free(k);
    }
    if (body) {
        wipe(body, w->len);   /* the file can hold Wi-Fi passwords */
        heap_caps_free(body);
    }
}

static void worker(void *arg)
{
    wjob_t *w = arg;
    if (w->kind == W_IMPORT) {
        do_import(w);
    } else {
        do_export(w);
    }
    httpd_req_async_handler_complete(w->req);
    free(w);
    atomic_store(&s_abort, false);
    atomic_store(&s_state, 0);
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------- handlers (httpd task) */

static esp_err_t start_worker(web_req_t *r, wjob_t *proto)
{
    int idle = 0;
    if (!atomic_compare_exchange_strong(&s_state, &idle, (int)proto->kind)) {
        web_send_err(r, 409, "busy", NULL, "An export or import is running");
        return r->req->content_len > WEB_BODY_MAX ? ESP_FAIL : ESP_OK;
    }
    atomic_store(&s_abort, false);
    if (atomic_load(&s_stopping)) {   /* after the claim: see web_backup_stop */
        atomic_store(&s_state, 0);
        web_send_busy(r);
        return r->req->content_len > WEB_BODY_MAX ? ESP_FAIL : ESP_OK;
    }
    wjob_t *w = malloc(sizeof *w);
    httpd_req_t *async = NULL;
    if (!w || httpd_req_async_handler_begin(r->req, &async) != ESP_OK) {
        free(w);
        atomic_store(&s_state, 0);
        web_send_busy(r);
        return r->req->content_len > WEB_BODY_MAX ? ESP_FAIL : ESP_OK;
    }
    *w = *proto;
    w->req = async;
    r->status = 202;   /* for the request log: the worker answers */
    if (xTaskCreate(worker, "web_bak", WORKER_STACK, w, WORKER_PRIO, NULL) != pdPASS) {
        web_req_t ar = {.req = async};
        web_send_busy(&ar);
        httpd_sess_trigger_close(async->handle, httpd_req_to_sockfd(async));
        httpd_req_async_handler_complete(async);
        free(w);
        atomic_store(&s_state, 0);
    }
    return ESP_OK;
}

static esp_err_t h_export(web_req_t *r, wkind_t kind)
{
    char v[4] = "";
    wjob_t w = {.kind = kind};
    if (kind == W_JSON && web_query_get(r, "secrets", v, sizeof v)) {
        if (strcmp(v, "1") != 0 && strcmp(v, "0") != 0) {
            return web_send_err(r, 400, "invalid", "secrets", "secrets is 0 or 1");
        }
        w.secrets = v[0] == '1';
    }
    return start_worker(r, &w);
}

static esp_err_t h_export_csv(web_req_t *r)
{
    return h_export(r, W_CSV);
}

static esp_err_t h_export_json(web_req_t *r)
{
    return h_export(r, W_JSON);
}

static esp_err_t refuse(web_req_t *r, int status, const char *code, const char *field, const char *msg)
{
    web_send_err(r, status, code, field, msg);
    return r->req->content_len > WEB_BODY_MAX ? ESP_FAIL : ESP_OK;   /* a large unread body: close */
}

static esp_err_t h_import(web_req_t *r)
{
    char v[12] = "";
    wjob_t w = {.kind = W_IMPORT, .mode = HY_IMPORT_MERGE};
    if (web_query_get(r, "mode", v, sizeof v)) {
        if (strcmp(v, "replace") == 0) {
            w.mode = HY_IMPORT_REPLACE;
        } else if (strcmp(v, "merge") != 0) {
            return refuse(r, 400, "invalid", "mode", "mode is merge or replace");
        }
    }
    w.preview = web_query_get(r, "preview", v, sizeof v) && strcmp(v, "1") == 0;
    w.settings = web_query_get(r, "settings", v, sizeof v) && strcmp(v, "1") == 0;
    size_t len = r->req->content_len;
    if (len == 0) {
        return refuse(r, 400, "invalid", "body", "Send the backup file as the request body");
    }
    if (len > IMPORT_MAX) {
        return refuse(r, 413, "too_large", NULL, "The file is above 4 MB");
    }
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < len + PSRAM_MARGIN) {
        return refuse(r, 413, "too_large", NULL, "Not enough memory for this file");
    }
    w.len = len;
    return start_worker(r, &w);
}

web_handler_t web_data_route(hy_route_id_t id)
{
    switch (id) {
    case HY_R_EXPORT_CSV: return h_export_csv;
    case HY_R_EXPORT_JSON: return h_export_json;
    case HY_R_IMPORT: return h_import;
    default: return NULL;
    }
}
