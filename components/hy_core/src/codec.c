#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "hy/codec.h"
#include "hy/med.h"

#define MAX_SAFE_INT 9007199254740992.0 /* 2^53: every integer below is exact in a double */

static const char *const STATUS_NAMES[] = { "taken", "skipped", "missed" };
static const char *const SOURCE_NAMES[] = { "screen", "web", "mqtt", "auto" };
static const char *const SCHED_NAMES[] = { "fixed", "as_needed" };

/* ---- decode helpers ---- */

/* Member of obj, or NULL when absent or null. */
static cJSON *member(const cJSON *obj, const char *name)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, name);
    return (it == NULL || cJSON_IsNull(it)) ? NULL : it;
}

/* Reads an integer member into *out. Absent: keeps *out unless required. */
static hy_err_t get_int(const cJSON *obj, const char *name, double lo, double hi,
                        bool required, double *out)
{
    cJSON *it = member(obj, name);
    if (!it) return required ? HY_E_INVALID : HY_OK;
    if (!cJSON_IsNumber(it)) return HY_E_INVALID;
    double d = it->valuedouble;
    if (!(d >= lo && d <= hi) || d != floor(d)) return HY_E_INVALID;
    *out = d;
    return HY_OK;
}

static hy_err_t get_bool(const cJSON *obj, const char *name, bool *out)
{
    cJSON *it = member(obj, name);
    if (!it) return HY_OK;
    if (!cJSON_IsBool(it)) return HY_E_INVALID;
    *out = cJSON_IsTrue(it);
    return HY_OK;
}

/* Copies a string member into buf (size bytes incl. NUL); too long is invalid. */
static hy_err_t get_str(const cJSON *obj, const char *name, char *buf, size_t size, bool required)
{
    cJSON *it = member(obj, name);
    if (!it) return required ? HY_E_INVALID : HY_OK;
    if (!cJSON_IsString(it) || it->valuestring == NULL) return HY_E_INVALID;
    if (strlen(it->valuestring) >= size) return HY_E_INVALID;
    strcpy(buf, it->valuestring);
    return HY_OK;
}

/* Maps an enum string member to its index. Absent: keeps *out unless required. */
static hy_err_t get_enum(const cJSON *obj, const char *name, const char *const *names,
                         int count, bool required, int *out)
{
    cJSON *it = member(obj, name);
    if (!it) return required ? HY_E_INVALID : HY_OK;
    if (!cJSON_IsString(it) || it->valuestring == NULL) return HY_E_INVALID;
    for (int i = 0; i < count; i++) {
        if (strcmp(it->valuestring, names[i]) == 0) {
            *out = i;
            return HY_OK;
        }
    }
    return HY_E_INVALID;
}

#define TRY(x) do { hy_err_t e_ = (x); if (e_ != HY_OK) return e_; } while (0)

/* ---- log line ---- */

int hy_log_to_jsonl(const hy_log_t *e, char *buf, size_t len)
{
    if (!e || !buf || len == 0) return -1;
    if ((unsigned)e->status > HY_MISSED || (unsigned)e->source > HY_SRC_AUTO) return -1;

    cJSON *o = cJSON_CreateObject();
    if (!o) return -1;
    bool ok = cJSON_AddNumberToObject(o, "id", (double)e->id) &&
              cJSON_AddNumberToObject(o, "ts", (double)e->ts) &&
              cJSON_AddStringToObject(o, "tz", e->tz) &&
              cJSON_AddNumberToObject(o, "med_id", (double)e->med_id) &&
              cJSON_AddStringToObject(o, "status", STATUS_NAMES[e->status]);
    if (ok && e->slot != 0) ok = cJSON_AddNumberToObject(o, "slot", (double)e->slot) != NULL;
    ok = ok && cJSON_AddNumberToObject(o, "delta_min", (double)e->delta_min);
    if (ok && e->note[0] != '\0') ok = cJSON_AddStringToObject(o, "note", e->note) != NULL;
    ok = ok && cJSON_AddStringToObject(o, "source", SOURCE_NAMES[e->source]) &&
          cJSON_AddBoolToObject(o, "edited", e->edited);
    char *s = ok ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
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

hy_err_t hy_log_from_jsonl(const char *line, hy_log_t *out)
{
    if (!line || !out) return HY_E_INVALID;
    cJSON *o = cJSON_Parse(line);
    if (!o) return HY_E_INVALID;
    hy_err_t r = HY_E_INVALID;
    if (cJSON_IsObject(o)) {
        hy_log_t e;
        memset(&e, 0, sizeof e);
        double d = 0;
        int en;
        r = get_int(o, "id", 0, 4294967295.0, true, &d);
        if (r == HY_OK) { e.id = (uint32_t)d; r = get_int(o, "ts", -MAX_SAFE_INT, MAX_SAFE_INT, true, &d); }
        if (r == HY_OK) { e.ts = (int64_t)d; r = get_str(o, "tz", e.tz, sizeof e.tz, false); }
        if (r == HY_OK) r = get_int(o, "med_id", 0, 4294967295.0, true, &d);
        if (r == HY_OK) {
            e.med_id = (uint32_t)d;
            en = 0;
            r = get_enum(o, "status", STATUS_NAMES, 3, true, &en);
            e.status = (hy_status_t)en;
        }
        if (r == HY_OK) {
            d = 0;
            r = get_int(o, "slot", -MAX_SAFE_INT, MAX_SAFE_INT, false, &d);
            e.slot = (int64_t)d;
        }
        if (r == HY_OK) {
            d = 0;
            r = get_int(o, "delta_min", -2147483648.0, 2147483647.0, false, &d);
            e.delta_min = (int32_t)d;
        }
        if (r == HY_OK) r = get_str(o, "note", e.note, sizeof e.note, false);
        if (r == HY_OK) {
            en = 0;
            r = get_enum(o, "source", SOURCE_NAMES, 4, false, &en);
            e.source = (hy_source_t)en;
        }
        if (r == HY_OK) r = get_bool(o, "edited", &e.edited);
        if (r == HY_OK) *out = e;
    }
    cJSON_Delete(o);
    return r;
}

/* ---- medicines ---- */

static cJSON *med_to_json(const hy_med_t *m)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *sch = cJSON_CreateObject();
    cJSON *times = cJSON_CreateArray();
    cJSON *pc = cJSON_CreateObject();
    if (!o || !sch || !times || !pc || (unsigned)m->type > HY_SCHED_AS_NEEDED ||
        m->n_times > HY_MAX_TIMES) {
        goto fail;
    }
    for (unsigned i = 0; i < m->n_times; i++) {
        char t[8];
        if (m->times[i] >= 24 * 60) goto fail;
        snprintf(t, sizeof t, "%02u:%02u", m->times[i] / 60u, m->times[i] % 60u);
        cJSON *s = cJSON_CreateString(t);
        if (!s) goto fail;
        cJSON_AddItemToArray(times, s);
    }
    if (!cJSON_AddStringToObject(sch, "type", SCHED_NAMES[m->type])) goto fail;
    cJSON_AddItemToObject(sch, "times", times);
    times = NULL;
    if (!cJSON_AddNumberToObject(sch, "weekdays", m->weekdays) ||
        !cJSON_AddNumberToObject(sch, "min_interval_min", m->min_interval_min)) goto fail;
    if (!cJSON_AddBoolToObject(pc, "enabled", m->pills_enabled) ||
        !cJSON_AddNumberToObject(pc, "remaining", m->pills_left) ||
        !cJSON_AddNumberToObject(pc, "alert", m->pills_alert)) goto fail;

    if (!cJSON_AddNumberToObject(o, "id", (double)m->id) ||
        !cJSON_AddStringToObject(o, "code", m->code) ||
        !cJSON_AddStringToObject(o, "name", m->name) ||
        !cJSON_AddStringToObject(o, "dose", m->dose) ||
        !cJSON_AddNumberToObject(o, "color", m->color)) goto fail;
    cJSON_AddItemToObject(o, "schedule", sch);
    sch = NULL;
    if (!cJSON_AddNumberToObject(o, "early_window_min", m->early_window_min) ||
        !cJSON_AddNumberToObject(o, "late_after_min", m->late_after_min) ||
        !cJSON_AddNumberToObject(o, "missed_after_min", m->missed_after_min)) goto fail;
    cJSON_AddItemToObject(o, "pill_count", pc);
    pc = NULL;
    if (m->active_from != 0 && !cJSON_AddNumberToObject(o, "active_from", m->active_from)) goto fail;
    if (m->active_until != 0 && !cJSON_AddNumberToObject(o, "active_until", m->active_until)) goto fail;
    if (!cJSON_AddBoolToObject(o, "archived", m->archived) ||
        !cJSON_AddNumberToObject(o, "order", m->order)) goto fail;
    return o;
fail:
    cJSON_Delete(o);
    cJSON_Delete(sch);
    cJSON_Delete(times);
    cJSON_Delete(pc);
    return NULL;
}

char *hy_meds_to_json(const hy_med_t *meds, size_t n)
{
    if (n > 0 && !meds) return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr || !cJSON_AddNumberToObject(root, "v", HY_SCHEMA_VERSION)) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return NULL;
    }
    cJSON_AddItemToObject(root, "meds", arr);
    for (size_t i = 0; i < n; i++) {
        cJSON *m = med_to_json(&meds[i]);
        if (!m) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(arr, m);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static bool parse_time(const char *s, uint16_t *out)
{
    if (strlen(s) != 5 || s[2] != ':') return false;
    for (int i = 0; i < 5; i++) {
        if (i != 2 && (s[i] < '0' || s[i] > '9')) return false;
    }
    int h = (s[0] - '0') * 10 + (s[1] - '0');
    int m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h > 23 || m > 59) return false;
    *out = (uint16_t)(h * 60 + m);
    return true;
}

static hy_err_t med_from_json(const cJSON *o, hy_med_t *m)
{
    if (!cJSON_IsObject(o)) return HY_E_INVALID;
    hy_med_defaults(m);
    double d = 0;
    int en = 0;

    TRY(get_int(o, "id", 0, 4294967295.0, true, &d));
    m->id = (uint32_t)d;
    TRY(get_str(o, "code", m->code, sizeof m->code, true));
    TRY(get_str(o, "name", m->name, sizeof m->name, true));
    TRY(get_str(o, "dose", m->dose, sizeof m->dose, false));
    d = m->color;
    TRY(get_int(o, "color", 0, 9, false, &d));
    m->color = (uint8_t)d;

    cJSON *sch = member(o, "schedule");
    if (sch) {
        if (!cJSON_IsObject(sch)) return HY_E_INVALID;
        TRY(get_enum(sch, "type", SCHED_NAMES, 2, false, &en));
        m->type = (hy_sched_type_t)en;
        cJSON *times = member(sch, "times");
        if (times) {
            if (!cJSON_IsArray(times)) return HY_E_INVALID;
            int count = cJSON_GetArraySize(times);
            if (count > HY_MAX_TIMES) return HY_E_INVALID;
            int i = 0;
            cJSON *t;
            cJSON_ArrayForEach(t, times) {
                if (!cJSON_IsString(t) || t->valuestring == NULL) return HY_E_INVALID;
                if (!parse_time(t->valuestring, &m->times[i])) return HY_E_INVALID;
                i++;
            }
            m->n_times = (uint8_t)count;
        }
        d = m->weekdays;
        TRY(get_int(sch, "weekdays", 0, 127, false, &d));
        m->weekdays = (uint8_t)d;
        d = m->min_interval_min;
        TRY(get_int(sch, "min_interval_min", 0, 65535, false, &d));
        m->min_interval_min = (uint16_t)d;
    }

    d = m->early_window_min;
    TRY(get_int(o, "early_window_min", 0, 65535, false, &d));
    m->early_window_min = (uint16_t)d;
    d = m->late_after_min;
    TRY(get_int(o, "late_after_min", 0, 65535, false, &d));
    m->late_after_min = (uint16_t)d;
    d = m->missed_after_min;
    TRY(get_int(o, "missed_after_min", 0, 65535, false, &d));
    m->missed_after_min = (uint16_t)d;

    cJSON *pc = member(o, "pill_count");
    if (pc) {
        if (!cJSON_IsObject(pc)) return HY_E_INVALID;
        TRY(get_bool(pc, "enabled", &m->pills_enabled));
        d = m->pills_left;
        TRY(get_int(pc, "remaining", -32768, 32767, false, &d));
        m->pills_left = (int16_t)d;
        d = m->pills_alert;
        TRY(get_int(pc, "alert", -32768, 32767, false, &d));
        m->pills_alert = (int16_t)d;
    }

    d = 0;
    TRY(get_int(o, "active_from", 0, 99991231, false, &d));
    m->active_from = (int32_t)d;
    d = 0;
    TRY(get_int(o, "active_until", 0, 99991231, false, &d));
    m->active_until = (int32_t)d;
    TRY(get_bool(o, "archived", &m->archived));
    d = m->order;
    TRY(get_int(o, "order", 0, 65535, false, &d));
    m->order = (uint16_t)d;
    return HY_OK;
}

hy_err_t hy_meds_from_json(const char *json, hy_med_t *out, size_t max, size_t *n)
{
    if (!json || !n || (max > 0 && !out)) return HY_E_INVALID;
    cJSON *root = cJSON_Parse(json);
    if (!root) return HY_E_INVALID;
    hy_err_t r = HY_E_INVALID;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "meds");
    if (cJSON_IsObject(root) && cJSON_IsNumber(v) && v->valuedouble == HY_SCHEMA_VERSION &&
        cJSON_IsArray(arr)) {
        size_t count = (size_t)cJSON_GetArraySize(arr);
        if (count > max) {
            r = HY_E_FULL;
        } else {
            r = HY_OK;
            size_t i = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, arr) {
                r = med_from_json(it, &out[i]);
                if (r != HY_OK) break;
                i++;
            }
            if (r == HY_OK) *n = count;
        }
    }
    cJSON_Delete(root);
    return r;
}
