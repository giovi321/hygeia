#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api_int.h"
#include "cJSON.h"
#include "hy/integ.h"

/* ---- event names ---- */

static const char *const IEV_NAME[HY_IE_COUNT] = { "taken", "skipped", "missed", "due", "snoozed", "undone",
                                                   "battery_low", "battery_critical", "clock_invalid" };

const char *hy_iev_name(hy_iev_t t)
{
    return (unsigned)t < HY_IE_COUNT ? IEV_NAME[t] : NULL;
}

bool hy_iev_from_name(const char *s, hy_iev_t *out)
{
    if (!s || !out) return false;
    for (int i = 0; i < HY_IE_COUNT; i++) {
        if (strcmp(s, IEV_NAME[i]) == 0) {
            *out = (hy_iev_t)i;
            return true;
        }
    }
    return false;
}

bool hy_iev_from_bus(hy_ev_type_t t, hy_iev_t *out)
{
    hy_iev_t v;
    switch (t) {
    case HY_EV_TAKEN: v = HY_IE_TAKEN; break;
    case HY_EV_SKIPPED: v = HY_IE_SKIPPED; break;
    case HY_EV_MISSED: v = HY_IE_MISSED; break;
    case HY_EV_DUE: v = HY_IE_DUE; break;
    case HY_EV_SNOOZED: v = HY_IE_SNOOZED; break;
    case HY_EV_UNDONE: v = HY_IE_UNDONE; break;
    case HY_EV_BATTERY_LOW: v = HY_IE_BATTERY_LOW; break;
    case HY_EV_BATTERY_CRITICAL: v = HY_IE_BATTERY_CRITICAL; break;
    case HY_EV_CLOCK_INVALID: v = HY_IE_CLOCK_INVALID; break;
    default: return false;
    }
    if (out) *out = v;
    return true;
}

/* ---- defaults ---- */

void hy_integ_defaults(hy_integ_t *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->mqtt.base, sizeof c->mqtt.base, "hygeia");
    snprintf(c->mqtt.ha_prefix, sizeof c->mqtt.ha_prefix, "homeassistant");
}

/* ---- url, topic, credentials ---- */

static bool prefix_ci(const char *s, const char *lit)
{
    for (; *lit; s++, lit++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        if (c != *lit) return false;
    }
    return true;
}

static bool host_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

hy_err_t hy_url_parse(const char *url, bool mqtt, hy_url_t *out)
{
    if (!url || !out) return HY_E_INVALID;
    size_t len = strnlen(url, HY_INTEG_URL_MAX + 1);
    if (len == 0 || len > HY_INTEG_URL_MAX) return HY_E_INVALID;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 0x20 || c == 0x7F || c == '#') return HY_E_INVALID;
    }
    hy_url_t u;
    memset(&u, 0, sizeof u);
    const char *p;
    if (mqtt) {
        if (prefix_ci(url, "mqtts://")) { u.tls = true; u.port = 8883; p = url + 8; }
        else if (prefix_ci(url, "mqtt://")) { u.port = 1883; p = url + 7; }
        else return HY_E_INVALID;
    } else {
        if (prefix_ci(url, "https://")) { u.tls = true; u.port = 443; p = url + 8; }
        else if (prefix_ci(url, "http://")) { u.port = 80; p = url + 7; }
        else return HY_E_INVALID;
    }
    size_t n = 0;
    while (host_char(p[n])) n++;
    if (n < 1 || n > sizeof u.host - 1) return HY_E_INVALID;
    if (p[0] == '.' || p[0] == '-' || p[n - 1] == '.' || p[n - 1] == '-') return HY_E_INVALID;
    memcpy(u.host, p, n);
    p += n;
    if (*p == ':') {
        p++;
        unsigned port = 0;
        size_t digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 5) return HY_E_INVALID;
            port = port * 10 + (unsigned)(*p - '0');
            p++;
        }
        if (digits == 0 || port < 1 || port > 65535) return HY_E_INVALID;
        u.port = (uint16_t)port;
    }
    if (*p == '\0') {
        u.path = p;
    } else if (*p == '/') {
        if (mqtt) {
            if (p[1] != '\0') return HY_E_INVALID;
            u.path = p + 1;   /* "" */
        } else {
            u.path = p;
        }
    } else {
        return HY_E_INVALID;   /* userinfo, query without a path, or a stray byte */
    }
    *out = u;
    return HY_OK;
}

hy_err_t hy_integ_check_topic(const char *t)
{
    if (!t) return HY_E_INVALID;
    size_t n = strnlen(t, HY_INTEG_TOPIC_MAX + 1);
    if (n < 1 || n > HY_INTEG_TOPIC_MAX) return HY_E_INVALID;
    if (t[0] == '/' || t[0] == '$' || t[n - 1] == '/') return HY_E_INVALID;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == '-' || c == '/';
        if (!ok || (c == '/' && i + 1 < n && t[i + 1] == '/')) return HY_E_INVALID;
    }
    return HY_OK;
}

hy_err_t hy_integ_check_cred(const char *s)
{
    if (!s) return HY_E_INVALID;
    size_t n = strnlen(s, HY_INTEG_CRED_MAX + 1);
    if (n > HY_INTEG_CRED_MAX) return HY_E_INVALID;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7F) return HY_E_INVALID;
    return hy_api_utf8_valid(s) ? HY_OK : HY_E_INVALID;
}

/* Equal after lower-casing the scheme and the host (the part before the first '/' after "://"). Both urls
 * passed hy_url_parse. */
static bool url_same(const char *a, const char *b)
{
    const char *ra = strstr(a, "://") + 3, *rb = strstr(b, "://") + 3;
    size_t ea = (size_t)(ra - a), eb = (size_t)(rb - b);
    while (a[ea] != '\0' && a[ea] != '/') ea++;
    while (b[eb] != '\0' && b[eb] != '/') eb++;
    if (ea != eb) return false;
    for (size_t i = 0; i < ea; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (char)(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return strcmp(a + ea, b + eb) == 0;
}

/* "webhooks.<i>" or "webhooks.<i>.<tail>" into f. The index is a slot number below HY_HOOK_MAX. */
static void hook_field(char f[24], unsigned i, const char *tail)
{
    snprintf(f, 24, "webhooks.%u%s%s", i & 7u, tail[0] ? "." : "", tail);
}

hy_err_t hy_integ_validate(const hy_integ_t *c, hy_api_err_t *err)
{
    if (!c) return hy_api_fail(err, HY_E_INVALID, NULL);
    hy_url_t u;
    if ((c->mqtt.on || c->mqtt.url[0] != '\0') && hy_url_parse(c->mqtt.url, true, &u) != HY_OK)
        return hy_api_fail(err, HY_E_INVALID, "mqtt.url");
    if (hy_integ_check_cred(c->mqtt.user) != HY_OK) return hy_api_fail(err, HY_E_INVALID, "mqtt.user");
    if (hy_integ_check_cred(c->mqtt.pass) != HY_OK) return hy_api_fail(err, HY_E_INVALID, "mqtt.password");
    if (hy_integ_check_topic(c->mqtt.base) != HY_OK) return hy_api_fail(err, HY_E_INVALID, "mqtt.base");
    if (hy_integ_check_topic(c->mqtt.ha_prefix) != HY_OK) return hy_api_fail(err, HY_E_INVALID, "mqtt.ha_prefix");
    if (c->n_hooks > HY_HOOK_MAX) return hy_api_fail(err, HY_E_INVALID, "webhooks");
    for (unsigned i = 0; i < c->n_hooks; i++) {
        char f[24];
        const hy_hook_cfg_t *h = &c->hook[i];
        hook_field(f, i, "url");
        if (hy_url_parse(h->url, false, &u) != HY_OK) return hy_api_fail(err, HY_E_INVALID, f);
        for (unsigned j = 0; j < i; j++)
            if (url_same(c->hook[j].url, h->url)) return hy_api_fail(err, HY_E_INVALID, f);
        if ((h->events & ~HY_IE_ALL) != 0) {
            hook_field(f, i, "events");
            return hy_api_fail(err, HY_E_INVALID, f);
        }
        if (hy_integ_check_cred(h->secret) != HY_OK) {
            hook_field(f, i, "secret");
            return hy_api_fail(err, HY_E_INVALID, f);
        }
    }
    return HY_OK;
}

/* ---- json out ---- */

bool hy_api_integ_add(cJSON *obj, const hy_integ_t *c, bool secrets)
{
    if (!obj || !c) return false;
    cJSON *m = cJSON_AddObjectToObject(obj, "mqtt");
    bool ok = m && cJSON_AddBoolToObject(m, "on", c->mqtt.on) && cJSON_AddStringToObject(m, "url", c->mqtt.url) &&
              cJSON_AddStringToObject(m, "user", c->mqtt.user) &&
              cJSON_AddBoolToObject(m, "password_set", c->mqtt.pass[0] != '\0') &&
              (!secrets || cJSON_AddStringToObject(m, "password", c->mqtt.pass)) &&
              cJSON_AddStringToObject(m, "base", c->mqtt.base) && cJSON_AddBoolToObject(m, "ha", c->mqtt.ha) &&
              cJSON_AddStringToObject(m, "ha_prefix", c->mqtt.ha_prefix);
    cJSON *arr = ok ? cJSON_AddArrayToObject(obj, "webhooks") : NULL;
    ok = arr != NULL;
    for (unsigned i = 0; ok && i < c->n_hooks && i < HY_HOOK_MAX; i++) {
        const hy_hook_cfg_t *h = &c->hook[i];
        cJSON *o = cJSON_CreateObject();
        if (!o) { ok = false; break; }
        cJSON_AddItemToArray(arr, o);
        cJSON *ev = NULL;
        ok = cJSON_AddBoolToObject(o, "on", h->on) && cJSON_AddStringToObject(o, "url", h->url) &&
             (ev = cJSON_AddArrayToObject(o, "events")) != NULL;
        for (int t = 0; ok && t < HY_IE_COUNT; t++) {
            if (!(h->events & (1u << t))) continue;
            cJSON *s = cJSON_CreateString(IEV_NAME[t]);
            if (!s) { ok = false; break; }
            cJSON_AddItemToArray(ev, s);
        }
        ok = ok && cJSON_AddBoolToObject(o, "secret_set", h->secret[0] != '\0') &&
             (!secrets || cJSON_AddStringToObject(o, "secret", h->secret));
    }
    return ok;
}

/* ---- json in ---- */

/* Absent or null: *has false, dst kept. A string must fit in cap bytes with its NUL, never cut. */
static hy_err_t get_str(const cJSON *o, const char *key, char *dst, size_t cap, bool *has, const char *field,
                        hy_api_err_t *err)
{
    *has = false;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it || cJSON_IsNull(it)) return HY_OK;
    if (!cJSON_IsString(it) || !it->valuestring || strnlen(it->valuestring, cap) >= cap)
        return hy_api_fail(err, HY_E_INVALID, field);
    memcpy(dst, it->valuestring, strlen(it->valuestring) + 1);
    *has = true;
    return HY_OK;
}

static hy_err_t get_flag(const cJSON *o, const char *key, bool *dst, const char *field, hy_api_err_t *err)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it || cJSON_IsNull(it)) return HY_OK;
    if (!cJSON_IsBool(it)) return hy_api_fail(err, HY_E_INVALID, field);
    *dst = cJSON_IsTrue(it);
    return HY_OK;
}

static hy_err_t mqtt_from_json(const cJSON *m, hy_mqtt_cfg_t *c, hy_api_err_t *err)
{
    if (!cJSON_IsObject(m) || hy_api_reject_dups(m, NULL) != HY_OK) return hy_api_fail(err, HY_E_INVALID, "mqtt");
    bool has;
    hy_err_t r = get_flag(m, "on", &c->on, "mqtt.on", err);
    if (r == HY_OK) r = get_str(m, "url", c->url, sizeof c->url, &has, "mqtt.url", err);
    if (r == HY_OK) r = get_str(m, "user", c->user, sizeof c->user, &has, "mqtt.user", err);
    if (r == HY_OK) r = get_str(m, "password", c->pass, sizeof c->pass, &has, "mqtt.password", err);
    if (r == HY_OK) r = get_str(m, "base", c->base, sizeof c->base, &has, "mqtt.base", err);
    if (r == HY_OK) r = get_flag(m, "ha", &c->ha, "mqtt.ha", err);
    if (r == HY_OK) r = get_str(m, "ha_prefix", c->ha_prefix, sizeof c->ha_prefix, &has, "mqtt.ha_prefix", err);
    return r;
}

static hy_err_t hook_from_json(const cJSON *it, unsigned i, hy_hook_cfg_t *h, hy_api_err_t *err)
{
    char f[24];
    hook_field(f, i, "");
    if (!cJSON_IsObject(it) || hy_api_reject_dups(it, NULL) != HY_OK) return hy_api_fail(err, HY_E_INVALID, f);

    const cJSON *u = cJSON_GetObjectItemCaseSensitive(it, "url");
    bool has;
    hook_field(f, i, "url");
    if (!u || cJSON_IsNull(u)) return hy_api_fail(err, HY_E_INVALID, f);
    hy_err_t r = get_str(it, "url", h->url, sizeof h->url, &has, f, err);
    if (r != HY_OK) return r;

    hook_field(f, i, "on");
    h->on = true;
    r = get_flag(it, "on", &h->on, f, err);
    if (r != HY_OK) return r;

    hook_field(f, i, "events");
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(it, "events");
    h->events = HY_IE_ALL;
    if (ev && !cJSON_IsNull(ev)) {
        if (!cJSON_IsArray(ev)) return hy_api_fail(err, HY_E_INVALID, f);
        h->events = 0;
        for (const cJSON *e = ev->child; e; e = e->next) {
            hy_iev_t t;
            if (!cJSON_IsString(e) || !hy_iev_from_name(e->valuestring, &t)) return hy_api_fail(err, HY_E_INVALID, f);
            h->events = (uint16_t)(h->events | (1u << t));
        }
    }

    hook_field(f, i, "secret");
    return get_str(it, "secret", h->secret, sizeof h->secret, &has, f, err);   /* absent keeps the slot's secret */
}

/* Overwrites a buffer that held a secret; the volatile pointer keeps the stores from being optimised away. */
static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

static void wipe_strings(cJSON *obj, const char *key)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) wipe(it->valuestring, strlen(it->valuestring));
}

/* Overwrites the secret strings of a parsed settings body (see hy/integ.h). */
void hy_api_integ_wipe_tree(struct cJSON *o)
{
    if (!o) return;
    cJSON *m = cJSON_GetObjectItemCaseSensitive(o, "mqtt");
    if (cJSON_IsObject(m)) wipe_strings(m, "password");
    cJSON *w = cJSON_GetObjectItemCaseSensitive(o, "webhooks");
    if (cJSON_IsArray(w))
        for (cJSON *h = w->child; h; h = h->next)
            if (cJSON_IsObject(h)) wipe_strings(h, "secret");
}

/* Working copy of the merge (about 1.8 KB, in PSRAM, off every stack). Only the LVGL task calls
 * hy_api_integ_from_json, so one copy is enough. */
static HY_EXT_BSS hy_integ_t merge_scratch;

hy_err_t hy_api_integ_from_json(const char *body, const hy_integ_t *cur, hy_integ_t *out, hy_api_err_t *err)
{
    if (!body || !cur || !out) return hy_api_fail(err, HY_E_INVALID, NULL);
    cJSON *o = cJSON_ParseWithOpts(body, NULL, true);
    if (!o || !cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return hy_api_fail(err, HY_E_INVALID, NULL);
    }
    hy_integ_t *w = &merge_scratch;
    *w = *cur;
    hy_err_t r = hy_api_reject_dups(o, err);

    const cJSON *m = cJSON_GetObjectItemCaseSensitive(o, "mqtt");
    if (r == HY_OK && m && !cJSON_IsNull(m)) r = mqtt_from_json(m, &w->mqtt, err);

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, "webhooks");
    if (r == HY_OK && arr && !cJSON_IsNull(arr)) {
        if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) > HY_HOOK_MAX) {
            r = hy_api_fail(err, HY_E_INVALID, "webhooks");
        } else {
            memset(w->hook, 0, sizeof w->hook);
            w->n_hooks = 0;
            unsigned i = 0;
            for (const cJSON *it = arr->child; r == HY_OK && it; it = it->next, i++) {
                /* The secret stays with the slot: start from the stored hook at the same position. */
                if (i < cur->n_hooks) memcpy(w->hook[i].secret, cur->hook[i].secret, sizeof w->hook[i].secret);
                r = hook_from_json(it, i, &w->hook[i], err);
                w->n_hooks = (uint8_t)(i + 1);
            }
        }
    }
    if (r == HY_OK) r = hy_integ_validate(w, err);
    if (r == HY_OK) *out = *w;
    wipe(w, sizeof *w);
    hy_api_integ_wipe_tree(o);
    cJSON_Delete(o);
    return r;
}

/* ---- status json ---- */

static const char *const ERR_NAME[HY_HE_COUNT] = { "", "offline", "connect", "tls", "timeout", "http", "memory",
                                                   "auth", "busy", "storage" };
static const char *const MQS_NAME[] = { "off", "waiting", "connecting", "connected", "error" };

const char *hy_integ_err_name(hy_integ_err_t e)
{
    return (unsigned)e < HY_HE_COUNT ? ERR_NAME[e] : NULL;
}

static bool add_err(cJSON *o, const char *key, unsigned e)
{
    if (e == HY_HE_NONE || e >= HY_HE_COUNT) return cJSON_AddNullToObject(o, key) != NULL;
    return cJSON_AddStringToObject(o, key, ERR_NAME[e]) != NULL;
}

static bool add_pos(cJSON *o, const char *key, int64_t v)
{
    if (v <= 0) return cJSON_AddNullToObject(o, key) != NULL;
    return cJSON_AddNumberToObject(o, key, (double)v) != NULL;
}

char *hy_api_integ_status_json(const hy_integ_status_t *s)
{
    if (!s) return NULL;
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    unsigned st = (unsigned)s->mqtt < sizeof MQS_NAME / sizeof MQS_NAME[0] ? (unsigned)s->mqtt : (unsigned)HY_MQS_OFF;
    cJSON *m = cJSON_AddObjectToObject(o, "mqtt");
    bool ok = m && cJSON_AddStringToObject(m, "state", MQS_NAME[st]) && add_err(m, "error", s->mqtt_err) &&
              add_pos(m, "since", s->mqtt_since) && cJSON_AddBoolToObject(m, "ha", s->ha) &&
              cJSON_AddNumberToObject(m, "pending", s->mqtt_pending);
    cJSON *arr = ok ? cJSON_AddArrayToObject(o, "webhooks") : NULL;
    ok = arr != NULL;
    for (unsigned i = 0; ok && i < s->n_hooks && i < HY_HOOK_MAX; i++) {
        const hy_hook_status_t *h = &s->hook[i];
        cJSON *w = cJSON_CreateObject();
        if (!w) { ok = false; break; }
        cJSON_AddItemToArray(arr, w);
        cJSON *last = NULL;
        ok = cJSON_AddNumberToObject(w, "index", i) && (last = cJSON_AddObjectToObject(w, "last")) != NULL &&
             add_pos(last, "at", h->at) && add_pos(last, "status", h->http) && add_err(last, "error", h->err) &&
             cJSON_AddNumberToObject(w, "pending", h->pending) &&
             (h->retry_in_s < 0 ? cJSON_AddNullToObject(w, "retry_in_s") != NULL
                                : cJSON_AddNumberToObject(w, "retry_in_s", h->retry_in_s) != NULL) &&
             cJSON_AddBoolToObject(w, "paused", h->paused);
    }
    cJSON *ob = ok ? cJSON_AddObjectToObject(o, "outbox") : NULL;
    ok = ob && cJSON_AddNumberToObject(ob, "pending", s->pending) && cJSON_AddNumberToObject(ob, "capacity", 1000) &&
         cJSON_AddNumberToObject(ob, "dropped", s->dropped) && cJSON_AddNumberToObject(ob, "expired", s->expired) &&
         cJSON_AddNumberToObject(ob, "rejected", s->rejected) && cJSON_AddNumberToObject(ob, "lost", s->lost) &&
         add_err(ob, "error", s->outbox_err);
    cJSON *t = ok ? cJSON_AddObjectToObject(o, "test") : NULL;
    ok = t && cJSON_AddBoolToObject(t, "running", s->test_running) &&
         (s->test_index < 0 ? cJSON_AddNullToObject(t, "index") : cJSON_AddNumberToObject(t, "index", s->test_index)) &&
         add_pos(t, "status", s->test_http) && add_err(t, "error", s->test_err);
    char *js = ok ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    return js;
}
