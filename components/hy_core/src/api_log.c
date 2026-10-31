#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api_int.h"
#include "codec_obj.h"
#include "hy/api.h"

#define MAX_SAFE_INT 999999999999999.0
#define DEFAULT_SPAN_S (30LL * 86400)

/* ---- query ---- */

/* Digits only (an optional "-" when neg_ok), 1..15 of them. */
static bool parse_i64(const char *s, size_t len, bool neg_ok, int64_t *out)
{
    bool neg = false;
    if (len > 0 && s[0] == '-' && neg_ok) { neg = true; s++; len--; }
    if (len == 0 || len > 15) return false;
    int64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = neg ? -v : v;
    return true;
}

static bool parse_u32(const char *s, size_t len, uint32_t *out)
{
    if (len == 0 || len > 10) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    if (v > 4294967295ULL) return false;
    *out = (uint32_t)v;
    return true;
}

hy_err_t hy_api_logq_parse(const char *query, int64_t now, hy_api_logq_t *q, hy_api_err_t *err)
{
    if (!q) return hy_api_fail(err, HY_E_INVALID, NULL);
    hy_api_logq_t r;
    memset(&r, 0, sizeof r);
    r.limit = 200;
    bool has_from = false, has_to = false;
    const char *p = query ? query : "";
    if (*p == '?') p++;
    while (*p) {
        size_t klen = strcspn(p, "=&");
        const char *key = p;
        const char *val = "";
        size_t vlen = 0;
        const char *next = p + klen;
        if (*next == '=') {
            val = next + 1;
            vlen = strcspn(val, "&");
            next = val + vlen;
        }
        if (*next == '&') next++;
        bool ok = true;
        const char *bad = NULL;
        if (klen == 4 && strncmp(key, "from", 4) == 0) {
            ok = parse_i64(val, vlen, true, &r.from);
            has_from = true;
            bad = "from";
        } else if (klen == 2 && strncmp(key, "to", 2) == 0) {
            ok = parse_i64(val, vlen, true, &r.to);
            has_to = true;
            bad = "to";
        } else if (klen == 3 && strncmp(key, "med", 3) == 0) {
            ok = parse_u32(val, vlen, &r.med);
            bad = "med";
        } else if (klen == 5 && strncmp(key, "limit", 5) == 0) {
            int64_t v = 0;
            ok = parse_i64(val, vlen, false, &v) && v >= 1 && v <= 500;
            if (ok) r.limit = (uint16_t)v;
            bad = "limit";
        } else if (klen == 5 && strncmp(key, "after", 5) == 0) {
            const char *dot = memchr(val, '.', vlen);
            ok = dot != NULL && parse_i64(val, (size_t)(dot - val), false, &r.after_ts) &&
                 parse_u32(dot + 1, vlen - (size_t)(dot - val) - 1, &r.after_id);
            bad = "after";
        }
        if (!ok) return hy_api_fail(err, HY_E_INVALID, bad);
        p = next;
    }
    if (!has_to) r.to = now + 1;
    if (!has_from) r.from = r.to - DEFAULT_SPAN_S;
    if (r.from >= r.to) return hy_api_fail(err, HY_E_INVALID, "from");
    *q = r;
    return HY_OK;
}

size_t hy_api_page_filter(hy_log_t *v, size_t n, int64_t after_ts, uint32_t after_id)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i].ts < after_ts || (v[i].ts == after_ts && v[i].id <= after_id)) continue;
        if (k != i) v[k] = v[i];
        k++;
    }
    return k;
}

/* ---- entries ---- */

int hy_api_log_json(const hy_log_t *e, char *buf, size_t len)
{
    if (!e || !buf || len == 0) return -1;
    cJSON *o = hy_codec_log_obj(e);
    if (!o) return -1;
    char local[24];
    if (!hy_api_fmt_local(e->ts, e->tz, local, sizeof local)) local[0] = '\0';
    int n = cJSON_AddStringToObject(o, "local", local) ? hy_api_print(o, buf, len) : -1;
    cJSON_Delete(o);
    return n;
}

/* Reads an integer member in [lo, hi]. Absent or null leaves *has false. */
static hy_err_t req_int(const cJSON *o, const char *key, double lo, double hi, int64_t *out, bool *has,
                        hy_api_err_t *err)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it || cJSON_IsNull(it)) return HY_OK;
    if (!cJSON_IsNumber(it) || !(it->valuedouble >= lo && it->valuedouble <= hi) ||
        it->valuedouble != floor(it->valuedouble))
        return hy_api_fail(err, HY_E_INVALID, key);
    *out = (int64_t)it->valuedouble;
    if (has) *has = true;
    return HY_OK;
}

hy_err_t hy_api_logreq_from_json(const char *body, hy_api_logreq_t *out, hy_api_err_t *err)
{
    if (!body || !out) return hy_api_fail(err, HY_E_INVALID, NULL);
    cJSON *o = cJSON_ParseWithOpts(body, NULL, true);
    if (!o || !cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return hy_api_fail(err, HY_E_INVALID, NULL);
    }
    hy_api_logreq_t r;
    memset(&r, 0, sizeof r);
    int64_t v = 0;
    hy_err_t res = req_int(o, "med_id", 1, 4294967295.0, &v, &r.has_med, err);
    if (res == HY_OK && r.has_med) r.med_id = (uint32_t)v;
    if (res == HY_OK) res = req_int(o, "ts", 1, MAX_SAFE_INT, &r.ts, &r.has_ts, err);
    if (res == HY_OK) res = req_int(o, "orig_ts", 1, MAX_SAFE_INT, &r.orig_ts, NULL, err);

    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, "status");
    if (res == HY_OK && it && !cJSON_IsNull(it)) {
        static const char *const names[] = { "taken", "skipped", "missed" };
        res = hy_api_fail(err, HY_E_INVALID, "status");
        if (cJSON_IsString(it) && it->valuestring) {
            for (int i = 0; i < 3; i++) {
                if (strcmp(it->valuestring, names[i]) == 0) {
                    r.status = (hy_status_t)i;
                    r.has_status = true;
                    res = HY_OK;
                }
            }
        }
    }
    it = cJSON_GetObjectItemCaseSensitive(o, "note");
    if (res == HY_OK && it && !cJSON_IsNull(it)) {
        if (!cJSON_IsString(it) || !it->valuestring || strlen(it->valuestring) >= sizeof r.note ||
            !hy_api_utf8_valid(it->valuestring)) {
            res = hy_api_fail(err, HY_E_INVALID, "note");
        } else {
            strcpy(r.note, it->valuestring);
            r.has_note = true;
        }
    }
    it = cJSON_GetObjectItemCaseSensitive(o, "confirm");
    if (res == HY_OK && it && !cJSON_IsNull(it)) {
        if (!cJSON_IsBool(it)) res = hy_api_fail(err, HY_E_INVALID, "confirm");
        else r.confirm = cJSON_IsTrue(it);
    }
    cJSON_Delete(o);
    if (res == HY_OK) *out = r;
    return res;
}

/* ---- csv ---- */

int hy_csv_field(const char *in, char *out, size_t len)
{
    if (!in || !out || len == 0) return -1;
    bool prefix = in[0] == '=' || in[0] == '+' || in[0] == '-' || in[0] == '@' || in[0] == '\t' || in[0] == '\r';
    bool quote = prefix && in[0] == '\r';
    for (const char *p = in; *p; p++)
        if (*p == ',' || *p == '"' || *p == '\r' || *p == '\n') quote = true;
    size_t n = 0;
    bool fit = true;
#define PUT(c) do { if (n + 1 < len) out[n] = (c); else fit = false; n++; } while (0)
    if (quote) PUT('"');
    if (prefix) PUT('\'');
    for (const char *p = in; *p; p++) {
        if (quote && *p == '"') PUT('"');
        PUT(*p);
    }
    if (quote) PUT('"');
#undef PUT
    if (!fit || n > 0x7FFFFFFF) {
        out[0] = '\0';
        return -1;
    }
    out[n] = '\0';
    return (int)n;
}

static const char *const STATUS_TXT[] = { "taken", "skipped", "missed" };
static const char *const SOURCE_TXT[] = { "screen", "web", "mqtt", "auto" };

int hy_api_log_csv(const hy_log_t *e, const hy_med_t *m, char *buf, size_t len)
{
    if (!e || !buf || len == 0) return -1;
    if ((unsigned)e->status > HY_MISSED || (unsigned)e->source > HY_SRC_AUTO) return -1;
    if (strnlen(e->tz, sizeof e->tz) >= sizeof e->tz || strnlen(e->note, sizeof e->note) >= sizeof e->note) return -1;
    char local[24], slot[24], tz[2 * HY_TZ_LEN + 3], code[8], name[2 * 33 + 3], note[2 * 64 + 3];
    if (!hy_api_fmt_local(e->ts, e->tz, local, sizeof local)) local[0] = '\0';
    slot[0] = '\0';
    if (e->slot != 0 && !hy_api_fmt_local(e->slot, e->tz, slot, sizeof slot)) slot[0] = '\0';
    const char *c = m ? m->code : "?";
    const char *nm = m ? m->name : "";
    if (strnlen(c, 3) >= 3 || strnlen(nm, 33) >= 33) return -1;
    if (hy_csv_field(e->tz, tz, sizeof tz) < 0 || hy_csv_field(c, code, sizeof code) < 0 ||
        hy_csv_field(nm, name, sizeof name) < 0 || hy_csv_field(e->note, note, sizeof note) < 0)
        return -1;
    int n = snprintf(buf, len, "%lu,%s,%lld,%s,%s,%s,%s,%s,%ld,%s,%s,%s", (unsigned long)e->id, local,
                     (long long)e->ts, tz, code, name, STATUS_TXT[e->status], slot, (long)e->delta_min, note,
                     SOURCE_TXT[e->source], e->edited ? "true" : "false");
    if (n < 0 || (size_t)n >= len) return -1;
    return n;
}
