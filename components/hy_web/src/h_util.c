/* Helpers shared by the REST handler files. */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "web_internal.h"

static const char *TAG = "web_rest";

/* ---------------------------------------------------------------- jobs */

hy_job_t *web_job_new(web_req_t *r, void (*fn)(void *), size_t ctx_size)
{
    size_t off = (ctx_size + 7) & ~(size_t)7;
    size_t n = r->body_len;
    hy_job_t *j = hy_job_new(fn, off + n + 1);
    if (!j) {
        return NULL;
    }
    char *ctx = hy_job_ctx(j);
    char *copy = ctx + off;
    if (n && r->body) {
        memcpy(copy, r->body, n);
    }
    copy[n] = '\0';
    *(const char **)ctx = copy;
    return j;
}

void *web_job_go(web_req_t *r, hy_job_t *j, uint32_t timeout_ms, esp_err_t *sent)
{
    if (hy_job_run(j, timeout_ms) != ESP_OK) {
        *sent = web_send_busy(r);
        hy_job_put(j);
        return NULL;
    }
    return hy_job_ctx(j);
}

esp_err_t web_oom(web_req_t *r)
{
    return web_send_busy(r);
}

/* ---------------------------------------------------------------- answers */

static const char *message_for(const char *code)
{
    static const struct { const char *code, *msg; } T[] = {
        {"invalid", "That request is not valid"},
        {"code", "The code is 1 or 2 letters or digits"},
        {"name", "The name is empty or too long"},
        {"dose", "The dose is too long"},
        {"times", "Give 1 to 8 different times and pick at least one day"},
        {"interval", "Check the minimum interval between doses"},
        {"code_dup", "Another active medicine already uses this code"},
        {"not_found", "Not found"},
        {"clock_invalid", "The device clock is not set"},
        {"undo_expired", "Too late to undo"},
        {"full", "Limit reached"},
        {"storage", "The device could not save the change"},
        {"too_soon", "The last dose was too recent"},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (strcmp(T[i].code, code) == 0) {
            return T[i].msg;
        }
    }
    return "Something went wrong on the device";
}

esp_err_t web_fail_api(web_req_t *r, const hy_api_err_t *e)
{
    char buf[256];
    if (hy_api_err_json(e, message_for(e->code), buf, sizeof buf) < 0) {
        snprintf(buf, sizeof buf, "{\"error\":\"%s\"}", e->code);
    }
    return web_send_json(r, e->http, buf);
}

esp_err_t web_fail(web_req_t *r, hy_err_t err)
{
    hy_api_err_t e;
    hy_api_err_from(err, &e);
    return web_fail_api(r, &e);
}

esp_err_t web_send_cjson(web_req_t *r, int status, cJSON *o)
{
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    if (!s) {
        return web_oom(r);
    }
    esp_err_t e = web_send_json(r, status, s);
    cJSON_free(s);
    return e;
}

const char *web_body_or_empty(const web_req_t *r)
{
    return r->body && r->body[0] ? r->body : "{}";
}

/* ---------------------------------------------------------------- query string */

const char *web_query(const web_req_t *r)
{
    const char *q = strchr(r->req->uri, '?');
    return q ? q + 1 : "";
}

bool web_query_get(const web_req_t *r, const char *key, char *out, size_t len)
{
    size_t kl = strlen(key);
    const char *p = web_query(r);
    while (*p) {
        size_t seg = strcspn(p, "&");
        if (seg > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            size_t vl = seg - kl - 1;
            if (vl >= len) {
                return false;
            }
            memcpy(out, p + kl + 1, vl);
            out[vl] = '\0';
            return true;
        }
        p += seg;
        if (*p == '&') {
            p++;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- store */

size_t web_log_window(uint32_t med_id, int64_t from, int64_t to, hy_log_t *out, size_t cap)
{
    const hy_store_port_t *st = hy_store_port();
    for (;;) {
        size_t n = st->query_log(NULL, from, to, med_id, out, cap);
        if (n < cap || to - from <= 3600) {
            return n;
        }
        hy_log_t probe[2];   /* exactly cap: full only when something follows the last entry */
        if (st->query_log(NULL, out[n - 1].ts, to, med_id, probe, 2) < 2) {
            return n;
        }
        ESP_LOGW(TAG, "log window of med %u filled %u entries: keeping the newest half", (unsigned)med_id,
                 (unsigned)cap);
        from += (to - from) / 2;
    }
}

/* ---------------------------------------------------------------- text buffer */

void web_sb_add(web_sb_t *b, const char *s, size_t n)
{
    if (b->bad) {
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        char *p = heap_caps_realloc(b->p, cap, MALLOC_CAP_SPIRAM);
        if (!p) {
            p = heap_caps_realloc(b->p, cap, MALLOC_CAP_8BIT);
        }
        if (!p) {
            b->bad = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void web_sb_str(web_sb_t *b, const char *s)
{
    web_sb_add(b, s, strlen(s));
}

void web_sb_fmt(web_sb_t *b, const char *fmt, ...)
{
    char t[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof t) {
        web_sb_add(b, t, (size_t)n);
    } else {
        b->bad = true;
    }
}

void web_sb_jstr(web_sb_t *b, const char *s)
{
    web_sb_add(b, "\"", 1);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            char e[2] = {'\\', (char)ch};
            web_sb_add(b, e, 2);
        } else if (ch < 0x20) {
            char e[8];
            int n = snprintf(e, sizeof e, "\\u%04x", ch);
            web_sb_add(b, e, (size_t)n);
        } else {
            web_sb_add(b, (const char *)&ch, 1);
        }
    }
    web_sb_add(b, "\"", 1);
}

void web_sb_free(web_sb_t *b)
{
    heap_caps_free(b->p);
    memset(b, 0, sizeof *b);
}

esp_err_t web_sb_send(web_req_t *r, int status, web_sb_t *b)
{
    esp_err_t e;
    if (b->bad || !b->p) {
        e = web_oom(r);
    } else {
        e = web_send_json(r, status, b->p);
    }
    web_sb_free(b);
    return e;
}

void web_date_str(int32_t ymd, char out[11])
{
    unsigned u = (unsigned)ymd;
    snprintf(out, 11, "%04u-%02u-%02u", (u / 10000) % 10000, (u / 100) % 100, u % 100);
}
