#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api_int.h"
#include "cJSON.h"
#include "hy/api.h"
#include "hy/events.h"
#include "hy/occ.h"
#include "hy/settings.h"

/* ---- settings ---- */

static bool fmt_hhmm(uint16_t minutes, char out[6])
{
    if (minutes > 1439) return false;
    snprintf(out, 6, "%02u:%02u", (unsigned)(minutes / 60), (unsigned)(minutes % 60));
    return true;
}

static bool parse_hhmm(const cJSON *it, uint16_t *out)
{
    if (!cJSON_IsString(it) || it->valuestring == NULL) return false;
    const char *s = it->valuestring;
    if (strlen(s) != 5 || s[2] != ':') return false;
    for (int i = 0; i < 5; i++)
        if (i != 2 && (s[i] < '0' || s[i] > '9')) return false;
    int h = (s[0] - '0') * 10 + (s[1] - '0');
    int m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h > 23 || m > 59) return false;
    *out = (uint16_t)(h * 60 + m);
    return true;
}

char *hy_api_settings_json(const hy_settings_t *s)
{
    if (!s) return NULL;
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    bool ok = cJSON_AddBoolToObject(o, "sound", s->sound) && cJSON_AddBoolToObject(o, "dnd", s->dnd) &&
              cJSON_AddNumberToObject(o, "volume", s->volume) &&
              cJSON_AddNumberToObject(o, "brightness", s->brightness) &&
              cJSON_AddNumberToObject(o, "snooze_min", s->snooze_min) &&
              cJSON_AddNumberToObject(o, "dim_s", s->dim_s) &&
              cJSON_AddNumberToObject(o, "off_bat_s", s->off_bat_s) &&
              cJSON_AddNumberToObject(o, "off_usb_s", s->off_usb_s) &&
              cJSON_AddBoolToObject(o, "rot_lock", s->rot_lock) &&
              cJSON_AddNumberToObject(o, "rot", s->rot);
    char from[6], to[6];
    if (ok) {
        if (s->quiet_from != s->quiet_to && fmt_hhmm(s->quiet_from, from) && fmt_hhmm(s->quiet_to, to)) {
            cJSON *q = cJSON_AddObjectToObject(o, "quiet");
            ok = q && cJSON_AddStringToObject(q, "from", from) && cJSON_AddStringToObject(q, "to", to);
        } else {
            ok = cJSON_AddNullToObject(o, "quiet") != NULL;
        }
    }
    ok = ok && cJSON_AddBoolToObject(o, "wifi_keep_batt", s->wifi_keep_batt);
    char *js = ok ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    return js;
}

/* An absent or null member leaves *has false. Whole numbers only; above cap is cap (the clamp narrows it). */
static hy_err_t get_uint(const cJSON *o, const char *key, double cap, double *out, bool *has, hy_api_err_t *err)
{
    *has = false;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it || cJSON_IsNull(it)) return HY_OK;
    if (!cJSON_IsNumber(it) || !(it->valuedouble >= 0) || it->valuedouble != floor(it->valuedouble))
        return hy_api_fail(err, HY_E_INVALID, key);
    *out = it->valuedouble > cap ? cap : it->valuedouble;
    *has = true;
    return HY_OK;
}

static hy_err_t get_bool(const cJSON *o, const char *key, bool *out, hy_api_err_t *err)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it || cJSON_IsNull(it)) return HY_OK;
    if (!cJSON_IsBool(it)) return hy_api_fail(err, HY_E_INVALID, key);
    *out = cJSON_IsTrue(it);
    return HY_OK;
}

hy_err_t hy_api_settings_from_json(const char *body, const hy_settings_t *cur, hy_settings_t *out, hy_api_err_t *err)
{
    if (!body || !cur || !out) return hy_api_fail(err, HY_E_INVALID, NULL);
    cJSON *o = cJSON_ParseWithOpts(body, NULL, true);
    if (!o || !cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return hy_api_fail(err, HY_E_INVALID, NULL);
    }
    hy_settings_t s = *cur;
    hy_err_t r = hy_api_reject_dups(o, err);
    double d = 0;
    bool has = false;
    static const struct { const char *key; double cap; } NUM[] = {
        { "volume", 255 }, { "brightness", 255 }, { "snooze_min", 65535 }, { "dim_s", 65535 },
        { "off_bat_s", 65535 }, { "off_usb_s", 4294967295.0 },
    };
    for (size_t i = 0; r == HY_OK && i < sizeof NUM / sizeof NUM[0]; i++) {
        r = get_uint(o, NUM[i].key, NUM[i].cap, &d, &has, err);
        if (r != HY_OK || !has) continue;
        switch (i) {
        case 0: s.volume = (uint8_t)d; break;
        case 1: s.brightness = (uint8_t)d; break;
        case 2: s.snooze_min = (uint16_t)d; break;
        case 3: s.dim_s = (uint16_t)d; break;
        case 4: s.off_bat_s = (uint16_t)d; break;
        default: s.off_usb_s = (uint32_t)d; break;
        }
    }
    if (r == HY_OK) r = get_bool(o, "sound", &s.sound, err);
    if (r == HY_OK) r = get_bool(o, "dnd", &s.dnd, err);
    if (r == HY_OK) r = get_bool(o, "rot_lock", &s.rot_lock, err);
    if (r == HY_OK) r = get_bool(o, "wifi_keep_batt", &s.wifi_keep_batt, err);
    if (r == HY_OK) {
        const cJSON *q = cJSON_GetObjectItemCaseSensitive(o, "quiet");
        if (q && cJSON_IsNull(q)) {
            s.quiet_from = s.quiet_to = 0;
        } else if (q) {
            uint16_t from = 0, to = 0;
            if (!cJSON_IsObject(q) || hy_api_reject_dups(q, NULL) != HY_OK ||
                !parse_hhmm(cJSON_GetObjectItemCaseSensitive(q, "from"), &from) ||
                !parse_hhmm(cJSON_GetObjectItemCaseSensitive(q, "to"), &to)) {
                r = hy_api_fail(err, HY_E_INVALID, "quiet");
            } else {
                s.quiet_from = from;
                s.quiet_to = to;
            }
        }
    }
    cJSON_Delete(o);
    if (r != HY_OK) return r;
    hy_settings_clamp(&s);
    *out = s;
    return HY_OK;
}

/* ---- state ---- */

static const char *const TILE_KIND[] = { "as_needed", "no_dose", "since", "due", "late", "snoozed", "missed" };
static const char *const OCC_STATE[] = { "upcoming", "due", "late", "missed", "taken", "skipped", "untracked" };
static const char *const NET_MODE[] = { "off", "client", "ap" };
static const char *const NET_STATE[] = { "off", "paused", "connecting", "connected", "failing", "ap" };

#define N_OF(a) (sizeof(a) / sizeof((a)[0]))

static const char *name_of(const char *const *tab, size_t n, unsigned v)
{
    return v < n ? tab[v] : "unknown";
}

/* Text of a fixed buffer: null when empty or not valid UTF-8 (an SSID is arbitrary bytes). */
static bool add_text(cJSON *o, const char *key, const char *s, size_t cap)
{
    char tmp[64];
    size_t n = strnlen(s, cap);
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    if (n == 0 || !hy_api_utf8_valid(tmp)) return cJSON_AddNullToObject(o, key) != NULL;
    return cJSON_AddStringToObject(o, key, tmp) != NULL;
}

static bool add_ts(cJSON *o, const char *key, int64_t v)
{
    if (v == 0) return cJSON_AddNullToObject(o, key) != NULL;
    return cJSON_AddNumberToObject(o, key, (double)v) != NULL;
}

static cJSON *tile_obj(const hy_api_tile_t *t)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    char code[sizeof t->code + 1], name[sizeof t->name + 1];
    snprintf(code, sizeof code, "%.*s", (int)sizeof t->code, t->code);
    snprintf(name, sizeof name, "%.*s", (int)sizeof t->name, t->name);
    bool ok = cJSON_AddNumberToObject(o, "id", t->id) && cJSON_AddStringToObject(o, "code", code) &&
              cJSON_AddStringToObject(o, "name", name) && cJSON_AddNumberToObject(o, "color", t->color) &&
              cJSON_AddStringToObject(o, "kind", name_of(TILE_KIND, N_OF(TILE_KIND), t->kind)) &&
              cJSON_AddNumberToObject(o, "ring", t->ring) && add_ts(o, "last_taken", t->last_taken) &&
              add_ts(o, "next_due", t->next_due) && cJSON_AddBoolToObject(o, "pills", t->pills) &&
              (t->pills ? cJSON_AddNumberToObject(o, "pills_left", t->pills_left)
                        : cJSON_AddNullToObject(o, "pills_left"));
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

static cJSON *due_obj(const hy_api_due_t *d)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    bool ok = cJSON_AddNumberToObject(o, "med_id", d->med_id) && cJSON_AddNumberToObject(o, "slot", (double)d->slot) &&
              cJSON_AddStringToObject(o, "state", name_of(OCC_STATE, N_OF(OCC_STATE), d->state)) &&
              add_ts(o, "snoozed_until", d->snoozed_until);
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

char *hy_api_state_json(const hy_api_state_t *s)
{
    if (!s || (s->n_tiles && !s->tiles) || (s->n_due && !s->due)) return NULL;
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    bool ok = cJSON_AddNumberToObject(o, "api", HY_API_VERSION) != NULL && add_ts(o, "utc_ms", s->utc_ms) &&
              cJSON_AddBoolToObject(o, "clock_valid", s->clock_valid);
    ok = ok && add_text(o, "zone", s->zone, sizeof s->zone);

    cJSON *sync = ok ? cJSON_AddObjectToObject(o, "sync") : NULL;
    ok = sync && add_text(sync, "src", s->sync_src, sizeof s->sync_src) && add_ts(sync, "at", s->sync_at);

    cJSON *bat = ok ? cJSON_AddObjectToObject(o, "battery") : NULL;
    ok = bat && (s->batt_pct < 0 ? cJSON_AddNullToObject(bat, "pct") : cJSON_AddNumberToObject(bat, "pct", s->batt_pct)) &&
         cJSON_AddBoolToObject(bat, "usb", s->usb) && cJSON_AddBoolToObject(bat, "charging", s->charging) &&
         add_text(bat, "estimate", s->estimate, sizeof s->estimate);

    cJSON *net = ok ? cJSON_AddObjectToObject(o, "net") : NULL;
    ok = net && cJSON_AddStringToObject(net, "mode", name_of(NET_MODE, N_OF(NET_MODE), s->net_mode)) &&
         cJSON_AddStringToObject(net, "state", name_of(NET_STATE, N_OF(NET_STATE), s->net_state)) &&
         add_text(net, "ssid", s->ssid, sizeof s->ssid) &&
         (s->rssi == 0 ? cJSON_AddNullToObject(net, "rssi") : cJSON_AddNumberToObject(net, "rssi", s->rssi)) &&
         add_text(net, "ip", s->ip, sizeof s->ip) && add_text(net, "host", s->host, sizeof s->host);

    cJSON *tiles = ok ? cJSON_AddArrayToObject(o, "tiles") : NULL;
    ok = tiles != NULL;
    for (size_t i = 0; ok && i < s->n_tiles; i++) {
        cJSON *t = tile_obj(&s->tiles[i]);
        if (!t) ok = false;
        else cJSON_AddItemToArray(tiles, t);
    }
    cJSON *due = ok ? cJSON_AddArrayToObject(o, "due") : NULL;
    ok = due != NULL;
    for (size_t i = 0; ok && i < s->n_due; i++) {
        cJSON *d = due_obj(&s->due[i]);
        if (!d) ok = false;
        else cJSON_AddItemToArray(due, d);
    }
    char *js = ok ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    return js;
}

/* ---- events ---- */

static const char *const EV_NAME[] = { "taken", "skipped", "missed", "due", "snoozed", "undone", "log_edited",
                                       "meds_changed", "untracked", "battery_low", "battery_critical",
                                       "clock_invalid" };

int hy_api_event_json(const hy_event_t *ev, char *buf, size_t len)
{
    if (!ev || !buf || len == 0 || (unsigned)ev->type >= N_OF(EV_NAME)) return -1;
    cJSON *o = cJSON_CreateObject();
    if (!o) return -1;
    bool ok = cJSON_AddStringToObject(o, "ev", EV_NAME[ev->type]) && cJSON_AddNumberToObject(o, "ts", (double)ev->ts) &&
              cJSON_AddNumberToObject(o, "med_id", ev->med_id) && cJSON_AddNumberToObject(o, "log_id", ev->log_id) &&
              cJSON_AddNumberToObject(o, "slot", (double)ev->slot) && cJSON_AddNumberToObject(o, "value", ev->value);
    int n = ok ? hy_api_print(o, buf, len) : -1;
    cJSON_Delete(o);
    return n;
}
