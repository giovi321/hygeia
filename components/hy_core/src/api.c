#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "api_int.h"
#include "cJSON.h"
#include "hy/api.h"
#include "hy/tzdb.h"
#include "hy/tzlock.h"


static const struct { hy_err_t e; int http; const char *code; const char *field; } ERR_MAP[] = {
    { HY_E_INVALID,      400, "invalid",       "" },
    { HY_E_CODE,         422, "code",          "code" },
    { HY_E_NAME,         422, "name",          "name" },
    { HY_E_DOSE,         422, "dose",          "dose" },
    { HY_E_TIMES,        422, "times",         "times" },
    { HY_E_INTERVAL,     422, "interval",      "min_interval_min" },
    { HY_E_CODE_DUP,     409, "code_dup",      "code" },
    { HY_E_NOT_FOUND,    404, "not_found",     "" },
    { HY_E_CLOCK,        409, "clock_invalid", "" },
    { HY_E_UNDO_EXPIRED, 410, "undo_expired",  "" },
    { HY_E_FULL,         409, "full",          "" },
    { HY_E_IO,           503, "storage",       "" },
    { HY_W_TOO_SOON,     409, "too_soon",      "" },
};

void hy_api_err_from(hy_err_t e, hy_api_err_t *out)
{
    if (!out) return;
    out->err = e;
    out->http = 500;
    out->code = "internal";
    out->field[0] = '\0';
    for (size_t i = 0; i < sizeof ERR_MAP / sizeof ERR_MAP[0]; i++) {
        if (ERR_MAP[i].e == e) {
            out->http = ERR_MAP[i].http;
            out->code = ERR_MAP[i].code;
            snprintf(out->field, sizeof out->field, "%s", ERR_MAP[i].field);
            return;
        }
    }
}

hy_err_t hy_api_fail(hy_api_err_t *err, hy_err_t e, const char *field)
{
    if (!err) return e;
    hy_api_err_from(e, err);
    if (field) snprintf(err->field, sizeof err->field, "%s", field);
    return e;
}

int hy_api_print(const cJSON *o, char *buf, size_t len)
{
    if (!o || !buf || len == 0) return -1;
    char *s = cJSON_PrintUnformatted(o);
    if (!s) return -1;
    size_t n = strlen(s);
    int ret = -1;
    if (n < len && n <= 0x7FFFFFFF) {
        memcpy(buf, s, n + 1);
        ret = (int)n;
    }
    free(s);
    return ret;
}

int hy_api_err_json(const hy_api_err_t *e, const char *message, char *buf, size_t len)
{
    if (!e || !buf || len == 0) return -1;
    cJSON *o = cJSON_CreateObject();
    if (!o) return -1;
    bool ok = cJSON_AddStringToObject(o, "error", e->code ? e->code : "internal") != NULL;
    if (ok && e->field[0]) ok = cJSON_AddStringToObject(o, "field", e->field) != NULL;
    if (ok) ok = cJSON_AddStringToObject(o, "message", message ? message : "") != NULL;
    int n = ok ? hy_api_print(o, buf, len) : -1;
    cJSON_Delete(o);
    return n;
}

bool hy_api_utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p;
        if (c < 0x80) { p++; continue; }
        int extra;
        unsigned min;
        unsigned cp;
        if (c >= 0xC2 && c <= 0xDF) { extra = 1; min = 0x80; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { extra = 2; min = 0x800; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { extra = 3; min = 0x10000; cp = c & 0x07; }
        else return false;
        for (int i = 1; i <= extra; i++) {
            if ((p[i] & 0xC0) != 0x80) return false; /* also catches the terminating NUL */
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        p += extra + 1;
    }
    return true;
}

bool hy_api_fmt_local(int64_t ts, const char *posix_tz, char *buf, size_t len)
{
    if (!buf || len == 0) return false;
    buf[0] = '\0';
    hy_saved_tz_t saved;
    if (!hy_tz_enter(&saved, (posix_tz && posix_tz[0]) ? posix_tz : NULL)) return false;
    time_t t = (time_t)ts;
    struct tm tm;
    bool ok = localtime_r(&t, &tm) != NULL;
    hy_tz_leave(&saved);
    if (!ok) return false;
    int n = snprintf(buf, len, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min);
    if (n < 0 || (size_t)n >= len) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

/* ---- time ---- */

hy_err_t hy_api_time_from_json(const char *body, int64_t build_epoch, hy_api_time_t *out, hy_api_err_t *err)
{
    if (!body || !out) return hy_api_fail(err, HY_E_INVALID, NULL);
    cJSON *o = cJSON_ParseWithOpts(body, NULL, true);
    if (!o || !cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return hy_api_fail(err, HY_E_INVALID, NULL);
    }
    hy_api_time_t t;
    memset(&t, 0, sizeof t);
    hy_err_t r = HY_OK;
    const char *field = NULL;

    cJSON *ms = cJSON_GetObjectItemCaseSensitive(o, "utc_ms");
    if (!cJSON_IsNumber(ms) || !(ms->valuedouble >= 0 && ms->valuedouble <= MAX_SAFE_INT * 1000.0) ||
        ms->valuedouble != floor(ms->valuedouble)) {
        r = HY_E_INVALID;
        field = "utc_ms";
    } else {
        t.utc_ms = (int64_t)ms->valuedouble;
        if (t.utc_ms < build_epoch * 1000) {
            r = HY_E_INVALID;
            field = "utc_ms";
        }
    }

    cJSON *au = cJSON_GetObjectItemCaseSensitive(o, "auto");
    if (r == HY_OK && au && !cJSON_IsNull(au)) {
        if (!cJSON_IsBool(au)) {
            r = HY_E_INVALID;
            field = "auto";
        } else {
            t.is_auto = cJSON_IsTrue(au);
        }
    }

    cJSON *zone = cJSON_GetObjectItemCaseSensitive(o, "zone");
    if (r == HY_OK && !t.is_auto && zone && !cJSON_IsNull(zone)) {
        if (!cJSON_IsString(zone) || zone->valuestring == NULL || strlen(zone->valuestring) >= sizeof t.zone ||
            hy_tzdb_posix(zone->valuestring) == NULL) {
            r = HY_E_INVALID;
            field = "zone";
        } else {
            strcpy(t.zone, zone->valuestring);
            t.has_zone = true;
        }
    }
    cJSON_Delete(o);
    if (r != HY_OK) return hy_api_fail(err, r, field);
    *out = t;
    return HY_OK;
}

bool hy_api_time_should_set(const hy_api_time_t *t, int64_t device_utc_ms, bool clock_valid)
{
    if (!t) return false;
    if (!t->is_auto || !clock_valid) return true;
    int64_t d = t->utc_ms - device_utc_ms;
    if (d < 0) d = -d;
    return d > 2000;
}
