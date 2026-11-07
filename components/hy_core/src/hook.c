#include "hy/hook.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "hy/codec.h"

/* Bounded writer: once a write does not fit, `bad` stays set and nothing more is written. */
typedef struct { char *buf; size_t len, pos; bool bad; } wr_t;

static void w_raw(wr_t *w, const char *s, size_t n)
{
    if (w->bad) return;
    if (w->pos + n + 1 > w->len) { w->bad = true; return; }
    memcpy(w->buf + w->pos, s, n);
    w->pos += n;
    w->buf[w->pos] = '\0';
}

static void w_str(wr_t *w, const char *s) { w_raw(w, s, strlen(s)); }

static void w_i64(wr_t *w, int64_t v)
{
    char t[24];
    int n = snprintf(t, sizeof t, "%" PRId64, v);
    w_raw(w, t, (size_t)n);
}

/* JSON string with the escapes cJSON prints: quote, backslash, \b \f \n \r \t, other control bytes as \u00xx.
 * Bytes from 0x20 up (UTF-8 included) go through unchanged. */
static void w_jstr(wr_t *w, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    w_raw(w, "\"", 1);
    for (; *s && !w->bad; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  w_raw(w, "\\\"", 2); break;
        case '\\': w_raw(w, "\\\\", 2); break;
        case '\b': w_raw(w, "\\b", 2); break;
        case '\f': w_raw(w, "\\f", 2); break;
        case '\n': w_raw(w, "\\n", 2); break;
        case '\r': w_raw(w, "\\r", 2); break;
        case '\t': w_raw(w, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char t[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
                w_raw(w, t, sizeof t);
            } else {
                w_raw(w, s, 1);
            }
        }
    }
    w_raw(w, "\"", 1);
}

static void w_ts_tz_device(wr_t *w, int64_t ts, const char *zone, const char *device)
{
    w_str(w, ",\"ts\":");
    if (ts > 0) w_i64(w, ts); else w_str(w, "null");
    w_str(w, ",\"tz\":");
    if (zone && zone[0]) w_jstr(w, zone); else w_str(w, "null");
    w_str(w, ",\"device\":");
    w_jstr(w, device ? device : "");
}

static void w_battery(wr_t *w, int32_t pct)
{
    w_str(w, ",\"battery\":");
    if (pct >= 0) w_i64(w, pct); else w_str(w, "null");
}

static int finish(const wr_t *w) { return w->bad ? -1 : (int)w->pos; }

int hy_hook_payload(const hy_hook_ev_t *e, uint32_t seq, char *buf, size_t len)
{
    if (!e || !buf || len == 0) return -1;
    const char *name = hy_iev_name(e->type);
    if (!name) return -1;
    wr_t w = { buf, len, 0, false };
    buf[0] = '\0';

    w_str(&w, "{\"id\":");
    w_i64(&w, seq);
    w_str(&w, ",\"event\":");
    w_jstr(&w, name);
    w_ts_tz_device(&w, e->ts, e->zone, e->device);

    w_str(&w, ",\"medicine\":");
    if (e->has_med) {
        w_str(&w, "{\"id\":");
        w_i64(&w, e->med_id);
        w_str(&w, ",\"code\":");
        w_jstr(&w, e->code);
        w_str(&w, ",\"name\":");
        w_jstr(&w, e->name);
        w_str(&w, ",\"dose\":");
        w_jstr(&w, e->dose);
        w_str(&w, "}");
    } else {
        w_str(&w, "null");
    }

    w_str(&w, ",\"slot\":");
    if (e->slot != 0) w_i64(&w, e->slot); else w_str(&w, "null");

    w_str(&w, ",\"log\":");
    if (e->type == HY_IE_UNDONE) {
        /* An undo takes the dose back: only the id of the undone entry, also when has_log is set (after an undo
         * the last written entry can be the restored missed entry, which the receiver must not store). */
        w_str(&w, "{\"id\":");
        w_i64(&w, e->log_id);
        w_str(&w, "}");
    } else if (e->has_log) {
        if (w.bad) return -1;
        int n = hy_log_to_jsonl(&e->log, buf + w.pos, len - w.pos);
        if (n < 0) return -1;
        w.pos += (size_t)n;
    } else {
        w_str(&w, "null");
    }

    bool batt_ev = e->type == HY_IE_BATTERY_LOW || e->type == HY_IE_BATTERY_CRITICAL;
    w_battery(&w, batt_ev ? e->value : e->batt_pct);
    w_str(&w, "}");
    return finish(&w);
}

int hy_hook_test_payload(int64_t ts, const char *zone, const char *device, int8_t batt_pct, char *buf, size_t len)
{
    if (!buf || len == 0) return -1;
    wr_t w = { buf, len, 0, false };
    buf[0] = '\0';
    w_str(&w, "{\"id\":0,\"event\":\"test\"");
    w_ts_tz_device(&w, ts, zone, device);
    w_str(&w, ",\"medicine\":null,\"slot\":null,\"log\":null");
    w_battery(&w, batt_pct);
    w_str(&w, "}");
    return finish(&w);
}

int hy_hook_sig_header(const uint8_t mac[32], char out[72])
{
    static const char hex[] = "0123456789abcdef";
    memcpy(out, "sha256=", 7);
    for (int i = 0; i < 32; i++) {
        out[7 + 2 * i] = hex[mac[i] >> 4];
        out[8 + 2 * i] = hex[mac[i] & 15];
    }
    out[71] = '\0';
    return 71;
}

hy_hook_verdict_t hy_hook_verdict(int status)
{
    if (status <= 0) return HY_HV_RETRY;
    if (status >= 200 && status <= 299) return HY_HV_DELIVERED;
    if (status == 401 || status == 403) return HY_HV_PAUSE;
    if (status == 408 || status == 429 || (status >= 500 && status <= 599)) return HY_HV_RETRY;
    return HY_HV_REJECT;
}

uint32_t hy_hook_retry_s(uint32_t failures)
{
    if (failures == 0) return 0;
    if (failures == 1) return 60;
    if (failures == 2) return 300;
    return 1800;
}

bool hy_ob_expired(int64_t at, int64_t now, bool clock_valid)
{
    return at > 0 && clock_valid && now - at > HY_OB_MAX_AGE_S;
}
