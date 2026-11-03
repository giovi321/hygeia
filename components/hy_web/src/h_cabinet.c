/* Cabinet open: GET /api/cabinet, POST /api/cabinet/test, GET /api/cabinet/samples.csv, DELETE /api/cabinet/teach.
 *
 * The runner's setters and getters are callable from any task, so the handlers run in the httpd task and call
 * hy_cab_run_* directly. The CSV is streamed from one snapshot of the template takes and the recent captures, one
 * sequence per chunk. Each test result reaches the open browsers as a live event (hy_web_cabinet_event). */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hy/cabinet.h"
#include "hy/cabinet_run.h"
#include "hy/web.h"
#include "web_internal.h"

static const char *TAG = "web_cabinet";

#define CSV_CHUNK_MAX 16384   /* one sequence: a rest row and up to HY_CAB_MAX_N rows of under 70 bytes */

static esp_err_t h_cabinet_get(web_req_t *r)
{
    hy_cab_status_t st;
    hy_cab_recent_t *rec = heap_caps_calloc(HY_CAB_RECENT_N, sizeof *rec, MALLOC_CAP_SPIRAM);
    if (!rec) {
        return web_oom(r);
    }
    hy_cab_run_status(&st);
    size_t n = hy_cab_run_snapshot(NULL, rec, HY_CAB_RECENT_N);
    cJSON *o = hy_api_cabinet_json(&st, rec, n);
    heap_caps_free(rec);
    return web_send_cjson(r, 200, o);
}

static esp_err_t h_cabinet_test(web_req_t *r)
{
    cJSON *o = cJSON_Parse(web_body_or_empty(r));
    const cJSON *on = o ? cJSON_GetObjectItemCaseSensitive(o, "on") : NULL;
    if (!cJSON_IsBool(on)) {
        cJSON_Delete(o);
        return web_send_err(r, 400, "invalid", "on", "Send {\"on\":true} or {\"on\":false}");
    }
    bool want = cJSON_IsTrue(on);
    cJSON_Delete(o);
    esp_err_t e = hy_cab_run_test(want);
    if (e != ESP_OK) {
        hy_cab_status_t st;
        hy_cab_run_status(&st);
        if (st.teaching) {
            return web_send_err(r, 409, "teaching", NULL, "The movement is being taught on the device");
        }
        if (st.mode == HY_CAB_TAUGHT && !st.taught) {
            return web_send_err(r, 409, "not_taught", NULL, "Teach the movement on the device first");
        }
        return web_send_err(r, 409, "screen_off", NULL, "Wake the screen on the device to run the test");
    }
    hy_cab_status_t st;
    hy_cab_run_status(&st);
    /* the counters of the live event, so the page starts from the device's values */
    cJSON *a = cJSON_CreateObject();
    cJSON *t = a ? cJSON_AddObjectToObject(a, "test") : NULL;
    if (!t || !cJSON_AddBoolToObject(a, "testing", st.testing) ||
        !cJSON_AddNumberToObject(t, "matches", st.test_matches) ||
        !cJSON_AddNumberToObject(t, "others", st.test_others)) {
        cJSON_Delete(a);
        return web_oom(r);
    }
    return web_send_cjson(r, 200, a);
}

static esp_err_t h_cabinet_forget(web_req_t *r)
{
    esp_err_t e = hy_cab_run_forget();
    if (e != ESP_OK) {
        return web_send_hy_err(r, HY_E_IO, "The taught movement could not be removed");
    }
    r->status = 204;
    httpd_resp_set_status(r->req, web_status_line(204));
    httpd_resp_set_hdr(r->req, "Cache-Control", "no-store");
    return httpd_resp_send(r->req, NULL, 0);
}

/* Sends the rest row and the rows of one sequence as one chunk. The rest row is keyed by (kind, take) like the
 * sample rows, so a teach take and a live capture with the same number keep their own rests. */
static bool send_seq(httpd_req_t *req, char *buf, const hy_cab_seq_t *q, const char *kind, int take)
{
    char rest_kind[16];
    snprintf(rest_kind, sizeof rest_kind, "rest_%s", kind);
    int h = snprintf(buf, CSV_CHUNK_MAX, "%s,%d,0,%d,%d,%d,0,0,0\n", rest_kind, take, q->rest.x, q->rest.y, q->rest.z);
    if (h < 0 || h >= CSV_CHUNK_MAX) {
        return false;
    }
    size_t n = hy_cab_csv_rows(q, kind, take, buf + h, CSV_CHUNK_MAX - (size_t)h);
    if (n == 0 && q->n > 0) {
        return false;
    }
    return httpd_resp_send_chunk(req, buf, (ssize_t)((size_t)h + n)) == ESP_OK;
}

static esp_err_t h_cabinet_samples(web_req_t *r)
{
    hy_cab_model_t *m = heap_caps_calloc(1, sizeof *m, MALLOC_CAP_SPIRAM);
    hy_cab_recent_t *rec = heap_caps_calloc(HY_CAB_RECENT_N, sizeof *rec, MALLOC_CAP_SPIRAM);
    char *buf = heap_caps_malloc(CSV_CHUNK_MAX, MALLOC_CAP_SPIRAM);
    if (!m || !rec || !buf) {
        heap_caps_free(m);
        heap_caps_free(rec);
        heap_caps_free(buf);
        return web_oom(r);
    }
    size_t nrec = hy_cab_run_snapshot(m, rec, HY_CAB_RECENT_N);
    httpd_req_t *req = r->req;
    r->status = 200;
    httpd_resp_set_status(req, web_status_line(200));
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"cabinet-samples.csv\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    bool ok = httpd_resp_send_chunk(req, HY_CAB_CSV_HEADER, HTTPD_RESP_USE_STRLEN) == ESP_OK;
    if (m->valid) {
        for (int i = 0; ok && i < HY_CAB_TAKES; i++) {
            ok = send_seq(req, buf, &m->take[i], "teach", i);
        }
    }
    for (size_t i = 0; ok && i < nrec; i++) {
        ok = send_seq(req, buf, &rec[i].seq, rec[i].kind == HY_CAB_REC_TEST ? "test" : "live", (int)i);
    }
    if (ok) {
        ok = httpd_resp_send_chunk(req, NULL, 0) == ESP_OK;
    }
    if (!ok) {
        ESP_LOGW(TAG, "samples download cut short");
    }
    heap_caps_free(m);
    heap_caps_free(rec);
    heap_caps_free(buf);
    return ok ? ESP_OK : ESP_FAIL;   /* the status line may be out already: closing the socket tells the client */
}

/* LVGL task, after each test result */
void hy_web_cabinet_event(float dist, float thr, bool match)
{
    hy_cab_status_t st;
    hy_cab_run_status(&st);
    if (!st.testing) {
        return;
    }
    char msg[160];
    if (hy_api_cabinet_event_json(&st, dist, thr, match, msg, sizeof msg) > 0) {
        web_ws_push(msg);
    }
}

web_handler_t web_cabinet_route(hy_route_id_t id)
{
    switch (id) {
    case HY_R_CABINET: return h_cabinet_get;
    case HY_R_CABINET_TEST: return h_cabinet_test;
    case HY_R_CABINET_SAMPLES: return h_cabinet_samples;
    case HY_R_CABINET_DEL: return h_cabinet_forget;
    default: return NULL;
    }
}
