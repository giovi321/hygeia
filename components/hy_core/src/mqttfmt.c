#include "hy/mqttfmt.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

/* ---- helpers ---- */

/* The segment [s, s + len) as an id: 1..4294967295, digits only, no leading zero. */
static bool parse_id(const char *s, size_t len, uint32_t *out)
{
    if (len == 0 || len > 10 || s[0] == '0') return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    if (v == 0 || v > 4294967295ULL) return false;
    *out = (uint32_t)v;
    return true;
}

static int fin(int n, size_t len)
{
    return (n < 0 || (size_t)n >= len) ? -1 : n;
}

/* Matches "<base>/med/" at the start of the topic; returns the length of the prefix or 0. */
static size_t med_prefix(const char *base, const char *topic, size_t n)
{
    size_t bl = strlen(base);
    if (n < bl + 5 || memcmp(topic, base, bl) != 0 || memcmp(topic + bl, "/med/", 5) != 0) return 0;
    return bl + 5;
}

/* ---- topics ---- */

int hy_mqtt_topic(const char *base, const char *leaf, char *out, size_t len)
{
    if (!base || !leaf || !out || len == 0) return -1;
    return fin(snprintf(out, len, "%s/%s", base, leaf), len);
}

int hy_mqtt_med_topic(const char *base, uint32_t id, const char *leaf, char *out, size_t len)
{
    if (!base || !leaf || !out || len == 0) return -1;
    return fin(snprintf(out, len, "%s/med/%lu/%s", base, (unsigned long)id, leaf), len);
}

bool hy_mqtt_parse_cmd(const char *base, const char *topic, size_t topic_len, uint32_t *id, hy_mqtt_cmd_t *cmd)
{
    if (!base || !topic || !id || !cmd) return false;
    size_t p = med_prefix(base, topic, topic_len);
    if (!p) return false;
    size_t i = p;
    while (i < topic_len && topic[i] != '/') i++;
    if (i >= topic_len) return false;
    uint32_t v;
    if (!parse_id(topic + p, i - p, &v)) return false;
    const char *leaf = topic + i + 1;
    size_t ll = topic_len - i - 1;
    hy_mqtt_cmd_t c;
    if (ll == 3 && memcmp(leaf, "log", 3) == 0) c = HY_MC_LOG;
    else if (ll == 4 && memcmp(leaf, "skip", 4) == 0) c = HY_MC_SKIP;
    else if (ll == 6 && memcmp(leaf, "snooze", 6) == 0) c = HY_MC_SNOOZE;
    else return false;
    *id = v;
    *cmd = c;
    return true;
}

bool hy_mqtt_cmd_accept(bool retained)
{
    return !retained;
}

/* ---- command payloads ---- */

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Trims ASCII whitespace from both ends. */
static void trim(const char **p, size_t *n)
{
    while (*n && is_ws((*p)[0])) { (*p)++; (*n)--; }
    while (*n && is_ws((*p)[*n - 1])) (*n)--;
}

bool hy_mqtt_parse_log(const char *p, size_t n, hy_mqtt_log_req_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!p) return true;
    trim(&p, &n);
    if (n == 0 || p[0] != '{') return true;
    char buf[256];
    if (n >= sizeof buf) return false;
    memcpy(buf, p, n);
    buf[n] = '\0';
    hy_api_logreq_t r;
    hy_api_err_t err;
    if (hy_api_logreq_from_json(buf, &r, &err) != HY_OK) return false;
    out->has_ts = r.has_ts;
    out->ts = r.has_ts ? r.ts : 0;
    out->has_note = r.has_note;
    if (r.has_note) memcpy(out->note, r.note, sizeof out->note);
    out->confirm = r.confirm;
    return true;
}

bool hy_mqtt_log_ts_ok(int64_t ts, int64_t now, int64_t build_epoch)
{
    return ts >= build_epoch && ts <= now + 60;
}

bool hy_mqtt_parse_snooze(const char *p, size_t n, uint16_t def, uint16_t *minutes)
{
    if (!minutes) return false;
    if (!p) { *minutes = def; return true; }
    trim(&p, &n);
    if (n == 0 || (n == 5 && memcmp(p, "PRESS", 5) == 0)) {
        *minutes = def;
        return true;
    }
    long v = -1;
    if (p[0] == '{') {
        char buf[64];
        if (n >= sizeof buf) return false;
        memcpy(buf, p, n);
        buf[n] = '\0';
        cJSON *o = cJSON_ParseWithOpts(buf, NULL, true);
        cJSON *m = o && cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, "minutes") : NULL;
        if (m && cJSON_IsNumber(m) && m->valuedouble >= 1 && m->valuedouble <= 120 &&
            m->valuedouble == (double)(long)m->valuedouble)
            v = (long)m->valuedouble;
        cJSON_Delete(o);
    } else if (n <= 3) {
        v = 0;
        for (size_t i = 0; i < n; i++) {
            if (p[i] < '0' || p[i] > '9') { v = -1; break; }
            v = v * 10 + (p[i] - '0');
        }
    }
    if (v < 1 || v > 120) return false;
    *minutes = (uint16_t)v;
    return true;
}

bool hy_mqtt_log_decide(const hy_med_t *m, const hy_mqtt_log_req_t *req, hy_err_t interval_check)
{
    if (!m || !req) return false;
    if (m->type == HY_SCHED_AS_NEEDED && m->min_interval_min > 0 && !req->has_ts && !req->confirm &&
        interval_check == HY_W_TOO_SOON)
        return false;
    return true;
}

/* ---- state JSON ---- */

/* JSON string body: escapes the quote, the backslash and control bytes. Returns the bytes written, -1 when full. */
static int esc(const char *s, char *out, size_t len)
{
    size_t o = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char tmp[8];
        size_t tl;
        if (c == '"' || c == '\\') { tmp[0] = '\\'; tmp[1] = (char)c; tl = 2; }
        else if (c < 0x20) tl = (size_t)snprintf(tmp, sizeof tmp, "\\u%04x", c);
        else { tmp[0] = (char)c; tl = 1; }
        if (o + tl >= len) return -1;
        memcpy(out + o, tmp, tl);
        o += tl;
    }
    out[o] = '\0';
    return (int)o;
}

/* "2025-11-06T07:02:00Z"; 24 bytes with the NUL cover any year of an int64 in range. */
static void iso_utc(int64_t ts, char out[24])
{
    int64_t days = ts >= 0 ? ts / 86400 : -((-ts + 86399) / 86400);
    int64_t sod = ts - days * 86400;
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    snprintf(out, 24, "%04d-%02d-%02dT%02d:%02d:%02dZ", (int)(y % 10000), (int)m, (int)d,
             (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60));
}

static const char *const TILE_KIND[] = { "as_needed", "no_dose", "since", "due", "late", "snoozed", "missed" };

int hy_mqtt_med_state(const hy_api_tile_t *t, char *buf, size_t len)
{
    if (!t || !buf || len == 0 || t->kind >= sizeof TILE_KIND / sizeof TILE_KIND[0]) return -1;
    char code[16], name[2 * 33 + 8], last[40], next[40], pills[16], ts[24];
    if (esc(t->code, code, sizeof code) < 0 || esc(t->name, name, sizeof name) < 0) return -1;
    if (t->last_taken > 0) { iso_utc(t->last_taken, ts); snprintf(last, sizeof last, "\"%s\"", ts); }
    else strcpy(last, "null");
    if (t->next_due > 0) { iso_utc(t->next_due, ts); snprintf(next, sizeof next, "\"%s\"", ts); }
    else strcpy(next, "null");
    if (t->pills) snprintf(pills, sizeof pills, "%d", (int)t->pills_left);
    else strcpy(pills, "null");
    bool due = t->kind == 3 || t->kind == 4 || t->kind == 5;
    return fin(snprintf(buf, len,
                        "{\"id\":%lu,\"code\":\"%s\",\"name\":\"%s\",\"status\":\"%s\",\"due\":%s,"
                        "\"last_taken\":%s,\"next_due\":%s,\"pills_left\":%s}",
                        (unsigned long)t->id, code, name, TILE_KIND[t->kind], due ? "true" : "false",
                        last, next, pills), len);
}

int hy_mqtt_device_state(const hy_mqtt_device_t *d, char *buf, size_t len)
{
    if (!d || !buf || len == 0) return -1;
    static const char *const MODE[] = { "off", "client", "ap" };
    char batt[8], rssi[8], src[48], adh[8], fw[80];
    if (d->batt_pct >= 0) snprintf(batt, sizeof batt, "%d", (int)d->batt_pct); else strcpy(batt, "null");
    if (d->rssi != 0) snprintf(rssi, sizeof rssi, "%d", (int)d->rssi); else strcpy(rssi, "null");
    if (d->adherence >= 0) snprintf(adh, sizeof adh, "%d", (int)d->adherence); else strcpy(adh, "null");
    if (strnlen(d->clock_src, sizeof d->clock_src) >= sizeof d->clock_src ||
        strnlen(d->fw, sizeof d->fw) >= sizeof d->fw) return -1;
    if (d->clock_src[0]) {
        char e[40];
        if (esc(d->clock_src, e, sizeof e) < 0) return -1;
        snprintf(src, sizeof src, "\"%s\"", e);
    } else {
        strcpy(src, "null");
    }
    if (esc(d->fw, fw, sizeof fw) < 0) return -1;
    return fin(snprintf(buf, len,
                        "{\"battery\":%s,\"charging\":%s,\"usb\":%s,\"wifi_mode\":\"%s\",\"rssi\":%s,"
                        "\"clock_source\":%s,\"clock_valid\":%s,\"adherence_30d\":%s,\"fw\":\"%s\"}",
                        batt, d->charging ? "true" : "false", d->usb ? "true" : "false",
                        MODE[d->net_mode > 2 ? 0 : d->net_mode], rssi, src,
                        d->clock_valid ? "true" : "false", adh, fw), len);
}

int16_t hy_mqtt_adherence_pct(uint32_t taken, uint32_t resolved)
{
    if (resolved == 0) return -1;
    uint64_t v = ((uint64_t)taken * 100 + resolved / 2) / resolved;
    return (int16_t)(v > 100 ? 100 : v);
}

/* ---- discovery ---- */

void hy_disc_node_id(const char *client_id, char out[16])
{
    size_t n = client_id ? strlen(client_id) : 0;
    const char *h = client_id ? client_id + (n >= 4 ? n - 4 : 0) : "";
    memcpy(out, "hygeia_", 7);
    size_t o = 7;
    for (; *h && o < 15; h++) out[o++] = (char)tolower((unsigned char)*h);
    out[o] = '\0';
}

static const char *const MED_OBJ[] = { "last_taken", "next_due", "due", "log", "pills" };
static const char *const MED_COMP[] = { "sensor", "sensor", "binary_sensor", "button", "sensor" };
static const char *const DEV_OBJ[] = { "battery", "charging", "adherence_30d", "event" };
static const char *const DEV_COMP[] = { "sensor", "binary_sensor", "sensor", "event" };

int hy_disc_med_topic(const char *prefix, const char *node, uint32_t id, hy_disc_med_obj_t o, char *out, size_t len)
{
    if (!prefix || !node || !out || len == 0 || (unsigned)o >= HY_DO_MED_COUNT) return -1;
    return fin(snprintf(out, len, "%s/%s/%s/med_%lu_%s/config", prefix, MED_COMP[o], node, (unsigned long)id,
                        MED_OBJ[o]), len);
}

int hy_disc_dev_topic(const char *prefix, const char *node, hy_disc_dev_obj_t o, char *out, size_t len)
{
    if (!prefix || !node || !out || len == 0 || (unsigned)o >= HY_DO_DEV_COUNT) return -1;
    return fin(snprintf(out, len, "%s/%s/%s/%s/config", prefix, DEV_COMP[o], node, DEV_OBJ[o]), len);
}

static bool add_str(cJSON *o, const char *k, const char *v)
{
    return cJSON_AddStringToObject(o, k, v) != NULL;
}

/* name, unique_id, availability_topic, device. */
static bool add_common(cJSON *o, const hy_disc_ctx_t *c, const char *name, const char *object)
{
    char uid[64], av[HY_MQTT_TOPIC_LEN];
    if (snprintf(uid, sizeof uid, "%s_%s", c->node, object) >= (int)sizeof uid) return false;
    if (hy_mqtt_topic(c->base, "status", av, sizeof av) < 0) return false;
    if (!add_str(o, "name", name) || !add_str(o, "unique_id", uid) || !add_str(o, "availability_topic", av))
        return false;
    cJSON *dev = cJSON_AddObjectToObject(o, "device");
    if (!dev) return false;
    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    if (!ids) return false;
    cJSON *id = cJSON_CreateString(c->node);
    if (!id) return false;
    cJSON_AddItemToArray(ids, id);
    return add_str(dev, "name", "Hygeia") && add_str(dev, "manufacturer", "Waveshare") &&
           add_str(dev, "model", "ESP32-S3-Touch-AMOLED-2.16") && add_str(dev, "sw_version", c->fw ? c->fw : "");
}

static char *print_free(cJSON *o, bool ok)
{
    char *s = ok ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    return s;
}

char *hy_disc_med_config(const hy_disc_ctx_t *c, uint32_t id, const char *name, hy_disc_med_obj_t o)
{
    if (!c || !c->base || !c->node || !name || (unsigned)o >= HY_DO_MED_COUNT) return NULL;
    static const char *const SUFFIX[] = { "last taken", "next due", "due", "log", "pills left" };
    char full[2 * 33 + 16], object[48], state[HY_MQTT_TOPIC_LEN];
    if (snprintf(full, sizeof full, "%s %s", name, SUFFIX[o]) >= (int)sizeof full) return NULL;
    snprintf(object, sizeof object, "med_%lu_%s", (unsigned long)id, MED_OBJ[o]);
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    bool ok = add_common(j, c, full, object);
    if (o == HY_DO_LOG) {
        char cmd[HY_MQTT_TOPIC_LEN];
        ok = ok && hy_mqtt_med_topic(c->base, id, "log", cmd, sizeof cmd) >= 0 &&
             add_str(j, "command_topic", cmd) && add_str(j, "payload_press", "PRESS");
        return print_free(j, ok);
    }
    ok = ok && hy_mqtt_med_topic(c->base, id, "state", state, sizeof state) >= 0 && add_str(j, "state_topic", state);
    switch (o) {
    case HY_DO_LAST_TAKEN:
        ok = ok && add_str(j, "value_template", "{{ value_json.last_taken }}") && add_str(j, "device_class", "timestamp");
        break;
    case HY_DO_NEXT_DUE:
        ok = ok && add_str(j, "value_template", "{{ value_json.next_due }}") && add_str(j, "device_class", "timestamp");
        break;
    case HY_DO_DUE:
        ok = ok && add_str(j, "value_template", "{{ 'ON' if value_json.due else 'OFF' }}");
        break;
    default:
        ok = ok && add_str(j, "value_template", "{{ value_json.pills_left }}") &&
             add_str(j, "unit_of_measurement", "pills");
        break;
    }
    return print_free(j, ok);
}

char *hy_disc_dev_config(const hy_disc_ctx_t *c, hy_disc_dev_obj_t o)
{
    if (!c || !c->base || !c->node || (unsigned)o >= HY_DO_DEV_COUNT) return NULL;
    static const char *const NAME[] = { "Battery", "Charging", "Adherence 30 days", "Event" };
    char topic[HY_MQTT_TOPIC_LEN];
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    bool ok = add_common(j, c, NAME[o], DEV_OBJ[o]);
    switch (o) {
    case HY_DO_BATTERY:
        ok = ok && hy_mqtt_topic(c->base, "device", topic, sizeof topic) >= 0 && add_str(j, "state_topic", topic) &&
             add_str(j, "device_class", "battery") && add_str(j, "unit_of_measurement", "%") &&
             add_str(j, "value_template", "{{ value_json.battery }}");
        break;
    case HY_DO_CHARGING:
        ok = ok && hy_mqtt_topic(c->base, "device", topic, sizeof topic) >= 0 && add_str(j, "state_topic", topic) &&
             add_str(j, "device_class", "battery_charging") &&
             add_str(j, "value_template", "{{ 'ON' if value_json.charging else 'OFF' }}");
        break;
    case HY_DO_ADHERENCE:
        ok = ok && hy_mqtt_topic(c->base, "device", topic, sizeof topic) >= 0 && add_str(j, "state_topic", topic) &&
             add_str(j, "unit_of_measurement", "%") && add_str(j, "value_template", "{{ value_json.adherence_30d }}");
        break;
    default: {
        cJSON *types = NULL;
        ok = ok && hy_mqtt_topic(c->base, "event", topic, sizeof topic) >= 0 && add_str(j, "state_topic", topic) &&
             (types = cJSON_AddArrayToObject(j, "event_types")) != NULL;
        for (int i = 0; ok && i < HY_IE_COUNT; i++) {
            cJSON *s = cJSON_CreateString(hy_iev_name((hy_iev_t)i));
            if (!s) ok = false; else cJSON_AddItemToArray(types, s);
        }
        ok = ok && add_str(j, "value_template", "{{ {'event_type': value_json.event} | to_json }}");
        break;
    }
    }
    return print_free(j, ok);
}

bool hy_disc_parse_topic(const char *prefix, const char *node, const char *topic, size_t n, char obj[32])
{
    if (!prefix || !node || !topic || !obj) return false;
    size_t pl = strlen(prefix), nl = strlen(node);
    if (n < pl + 1 || memcmp(topic, prefix, pl) != 0 || topic[pl] != '/') return false;
    size_t i = pl + 1;
    size_t cs = i;
    while (i < n && topic[i] != '/') i++;
    if (i == cs || i >= n) return false;                       /* component */
    i++;
    if (n - i < nl + 1 || memcmp(topic + i, node, nl) != 0 || topic[i + nl] != '/') return false;
    i += nl + 1;
    size_t os = i;
    while (i < n && topic[i] != '/') i++;
    size_t ol = i - os;
    if (ol == 0 || ol > 31 || i >= n) return false;
    if (n - i != 7 || memcmp(topic + i, "/config", 7) != 0) return false;
    memcpy(obj, topic + os, ol);
    obj[ol] = '\0';
    return true;
}

static bool med_active(uint32_t id, const hy_disc_med_t *meds, size_t n, bool *pills)
{
    for (size_t i = 0; meds && i < n; i++) {
        if (meds[i].id == id) {
            if (pills) *pills = meds[i].pills;
            return true;
        }
    }
    return false;
}

bool hy_disc_wanted(const char *obj, bool ha_on, const hy_disc_med_t *meds, size_t n)
{
    if (!obj || !ha_on) return false;
    for (size_t i = 0; i < sizeof DEV_OBJ / sizeof DEV_OBJ[0]; i++)
        if (strcmp(obj, DEV_OBJ[i]) == 0) return true;
    if (strncmp(obj, "med_", 4) != 0) return false;
    const char *p = obj + 4;
    const char *us = strchr(p, '_');
    uint32_t id;
    if (!us || !parse_id(p, (size_t)(us - p), &id)) return false;
    for (size_t i = 0; i < sizeof MED_OBJ / sizeof MED_OBJ[0]; i++) {
        if (strcmp(us + 1, MED_OBJ[i]) != 0) continue;
        bool pills = false;
        if (!med_active(id, meds, n, &pills)) return false;
        return i == HY_DO_PILLS ? pills : true;
    }
    return false;
}

bool hy_mqtt_state_stale(const char *base, const char *topic, size_t n, const hy_disc_med_t *meds, size_t nm)
{
    if (!base || !topic) return false;
    size_t p = med_prefix(base, topic, n);
    if (!p) return false;
    size_t i = p;
    while (i < n && topic[i] != '/') i++;
    uint32_t id;
    if (i >= n || !parse_id(topic + p, i - p, &id)) return false;
    if (n - i != 6 || memcmp(topic + i, "/state", 6) != 0) return false;
    return !med_active(id, meds, nm, NULL);
}
