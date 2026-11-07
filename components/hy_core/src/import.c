/*
 * Backup file: export head encoder, import parser and the pure import plan.
 *
 * The "log" array of a backup can hold tens of thousands of entries. It is never parsed as one cJSON tree: a
 * string-aware scanner finds the top-level "log" member, the rest of the object (the head) is copied without it and
 * parsed with cJSON, and the array is walked one element at a time, each element decoded on its own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api_int.h"
#include "hy/codec.h"
#include "hy/import.h"
#include "hy/med.h"

/* ---------------------------------------------------------------- scanner */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* p at the opening quote: the position after the closing quote, NULL when the text ends first. */
static const char *string_end(const char *p)
{
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!p[1]) return NULL;
            p++;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

/* Nesting limits. cJSON parses and deletes recursively and the import runs on a 4 KB stack, so deeper input is
 * refused before any tree is built. A member of the backup object is at most 7 deep (meds, medicine, schedule,
 * times), so the whole head stays within 8; the log array holds flat objects (2 with the array). */
#define HEAD_DEPTH  7
#define LOG_DEPTH   2
#define ENTRY_DEPTH 1

/* p at the first byte of a value: the position after it, NULL when it is cut off, unbalanced or nested deeper than
 * max_depth (then *deep is set when deep is not NULL). Brackets inside strings do not count. Scalars end at the
 * next delimiter; cJSON or the codec checks their spelling later. */
static const char *value_end(const char *p, size_t max_depth, bool *deep)
{
    if (*p == '"') return string_end(p);
    if (*p == '{' || *p == '[') {
        char stack[HEAD_DEPTH];
        size_t depth = 0;
        if (max_depth > sizeof stack) max_depth = sizeof stack;
        for (; *p; p++) {
            if (*p == '"') {
                p = string_end(p);
                if (!p) return NULL;
                p--;
            } else if (*p == '{' || *p == '[') {
                if (depth >= max_depth) {
                    if (deep) *deep = true;
                    return NULL;
                }
                stack[depth++] = *p == '{' ? '}' : ']';
            } else if (*p == '}' || *p == ']') {
                if (depth == 0 || stack[depth - 1] != *p) return NULL;
                if (--depth == 0) return p + 1;
            }
        }
        return NULL;
    }
    const char *s = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    return p == s ? NULL : p;
}

typedef struct {
    const char *member;     /* opening quote of the "log" key */
    const char *val, *end;  /* the array value [val, end) */
} log_span_t;

/* Walks the top-level object once. false when the text is not exactly one object. */
static bool find_log(const char *json, log_span_t *out, bool *dup_log, bool *deep)
{
    memset(out, 0, sizeof *out);
    *dup_log = false;
    *deep = false;
    const char *p = skip_ws(json);
    if (*p != '{') return false;
    p = skip_ws(p + 1);
    if (*p == '}') return *skip_ws(p + 1) == '\0';
    for (;;) {
        if (*p != '"') return false;
        const char *k = p;
        const char *ke = string_end(p);
        if (!ke) return false;
        bool is_log = ke - k == 5 && memcmp(k, "\"log\"", 5) == 0;
        p = skip_ws(ke);
        if (*p != ':') return false;
        p = skip_ws(p + 1);
        const char *v = p;
        const char *ve = value_end(p, is_log ? LOG_DEPTH : HEAD_DEPTH, deep);
        if (!ve) {
            if (is_log) {               /* a cut-off log array: the caller names it */
                out->member = k; out->val = v; out->end = NULL;
            }
            return false;
        }
        if (is_log) {
            if (out->member) *dup_log = true;
            out->member = k; out->val = v; out->end = ve;
        }
        p = skip_ws(ve);
        if (*p == ',') {
            p = skip_ws(p + 1);
            continue;
        }
        if (*p == '}') return *skip_ws(p + 1) == '\0';
        return false;
    }
}

/* The object without its "log" member and one neighbouring comma. malloc'd. */
static char *head_without_log(const char *json, const log_span_t *s)
{
    size_t len = strlen(json);
    if (!s->member) {
        char *h = malloc(len + 1);
        if (h) memcpy(h, json, len + 1);
        return h;
    }
    const char *cut_a = s->member, *cut_b = s->end;
    const char *after = skip_ws(cut_b);
    if (*after == ',') {
        cut_b = after + 1;              /* "log" followed by another member */
    } else {
        const char *q = cut_a;          /* "log" is the last member: drop the comma before it */
        while (q > json && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\n' || q[-1] == '\r')) q--;
        if (q > json && q[-1] == ',') cut_a = q - 1;
    }
    size_t a = (size_t)(cut_a - json);
    size_t b = (size_t)(cut_b - json);
    char *h = malloc(a + (len - b) + 1);   /* the head only: the log array is not copied */
    if (!h) return NULL;
    memcpy(h, json, a);
    memcpy(h + a, json + b, len - b + 1);
    return h;
}

/* ---------------------------------------------------------------- log cursor */

void hy_import_log_begin(const hy_import_t *imp, hy_import_cursor_t *c)
{
    c->p = imp && imp->log ? imp->log + 1 : NULL;
    c->started = false;
    c->done = c->p == NULL;
}

int hy_import_log_next(hy_import_cursor_t *c, hy_log_t *out)
{
    if (c->done) return 0;
    const char *p = skip_ws(c->p);
    if (*p == ']') {
        c->done = true;
        c->p = p + 1;
        return 0;
    }
    if (c->started) {
        if (*p != ',') return -1;
        p = skip_ws(p + 1);
    }
    if (*p != '{') return -1;
    const char *e = value_end(p, ENTRY_DEPTH, NULL);
    if (!e || (size_t)(e - p) >= sizeof c->line) return -1;
    memcpy(c->line, p, (size_t)(e - p));
    c->line[e - p] = '\0';
    hy_err_t r = hy_log_from_jsonl(c->line, out);
    if (r != HY_OK) return -1;
    c->started = true;
    c->p = e;
    return 1;
}

/* ---------------------------------------------------------------- head */

static cJSON *member(const cJSON *o, const char *name)
{
    cJSON *m = cJSON_GetObjectItemCaseSensitive(o, name);
    return cJSON_IsNull(m) ? NULL : m;
}

static bool whole_u32(const cJSON *n, uint32_t *out)
{
    if (!cJSON_IsNumber(n) || n->valuedouble < 0 || n->valuedouble > 4294967295.0 ||
        n->valuedouble != (double)(uint32_t)n->valuedouble)
        return false;
    *out = (uint32_t)n->valuedouble;
    return true;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

/* Wipes every string under o before the tree is deleted (the Wi-Fi member holds passwords). The depth is bounded
 * by the scanner's limits. */
static void wipe_strings(cJSON *o)
{
    if (!o) return;
    if (cJSON_IsString(o) && o->valuestring) wipe(o->valuestring, strlen(o->valuestring));
    for (cJSON *c = o->child; c; c = c->next) wipe_strings(c);
}

static hy_err_t parse_wifi(const cJSON *w, hy_backup_wifi_t *out, hy_api_err_t *err)
{
    if (!cJSON_IsObject(w)) return hy_api_fail(err, HY_E_INVALID, "wifi");
    cJSON *nets = member(w, "networks");
    if (nets) {
        if (!cJSON_IsArray(nets) || cJSON_GetArraySize(nets) > HY_NET_MAX_KNOWN)
            return hy_api_fail(err, HY_E_INVALID, "wifi");
        cJSON *it;
        cJSON_ArrayForEach(it, nets) {
            cJSON *s = member(it, "ssid"), *pw = member(it, "pass");
            const char *pass = pw ? (cJSON_IsString(pw) ? pw->valuestring : NULL) : "";
            if (!cJSON_IsString(s) || !pass || strlen(s->valuestring) >= sizeof out->ssid[0] ||
                strlen(pass) >= sizeof out->pass[0] || hy_net_check_ssid(s->valuestring) != HY_OK ||
                hy_net_check_pass(pass, false) != HY_OK)
                return hy_api_fail(err, HY_E_INVALID, "wifi");
            strcpy(out->ssid[out->n], s->valuestring);
            strcpy(out->pass[out->n], pass);
            out->n++;
        }
    }
    cJSON *ap = member(w, "ap_pass");
    if (ap) {
        if (!cJSON_IsString(ap) || strlen(ap->valuestring) >= sizeof out->ap_pass ||
            hy_net_check_pass(ap->valuestring, true) != HY_OK)
            return hy_api_fail(err, HY_E_INVALID, "wifi");
        strcpy(out->ap_pass, ap->valuestring);
    }
    return HY_OK;
}

static hy_err_t parse_head(const cJSON *root, hy_import_t *imp, hy_api_err_t *err)
{
    cJSON *v = member(root, "v");
    if (!cJSON_IsNumber(v) || v->valuedouble != HY_SCHEMA_VERSION) return hy_api_fail(err, HY_E_INVALID, "v");
    cJSON *ex = member(root, "exported");
    if (ex) {
        if (!cJSON_IsNumber(ex) || ex->valuedouble < 0 || ex->valuedouble > MAX_SAFE_INT)
            return hy_api_fail(err, HY_E_INVALID, "exported");
        imp->exported = (int64_t)ex->valuedouble;
    }

    cJSON *meds = member(root, "meds");
    if (!cJSON_IsArray(meds)) return hy_api_fail(err, HY_E_INVALID, "meds");
    if (cJSON_GetArraySize(meds) > HY_MAX_STORED) return hy_api_fail(err, HY_E_FULL, "meds");
    cJSON *it;
    uint32_t mx = 0;
    cJSON_ArrayForEach(it, meds) {
        hy_med_t *m = &imp->meds[imp->n_meds];
        if (hy_codec_med_from_obj(it, m) != HY_OK || m->id == 0) return hy_api_fail(err, HY_E_INVALID, "meds");
        for (size_t i = 0; i < imp->n_meds; i++)
            if (imp->meds[i].id == m->id) return hy_api_fail(err, HY_E_INVALID, "meds");
        if (m->id > mx) mx = m->id;
        imp->n_meds++;
    }
    uint32_t floor_id = mx == 0xFFFFFFFFu ? mx : mx + 1;
    uint32_t nx = 0;
    imp->next_id = whole_u32(member(root, "next_id"), &nx) && nx >= floor_id ? nx : floor_id;

    cJSON *gaps = member(root, "gaps");
    if (gaps) {
        if (!cJSON_IsArray(gaps) || cJSON_GetArraySize(gaps) > HY_IMPORT_GAPS_MAX)
            return hy_api_fail(err, HY_E_INVALID, "gaps");
        cJSON_ArrayForEach(it, gaps) {
            char line[128];
            hy_gap_reason_t r;
            if (hy_api_print(it, line, sizeof line) < 0 ||
                hy_gap_from_jsonl(line, &imp->gaps[imp->n_gaps], &r) != HY_OK)
                return hy_api_fail(err, HY_E_INVALID, "gaps");
            imp->gap_reasons[imp->n_gaps++] = (uint8_t)r;
        }
    }

    cJSON *set = member(root, "settings");
    if (set) {
        hy_settings_t base, out;
        hy_api_err_t se;
        hy_settings_defaults(&base);
        char *txt = cJSON_IsObject(set) ? cJSON_PrintUnformatted(set) : NULL;
        hy_api_err_t ie;
        hy_err_t sr = !txt ? HY_E_INVALID : hy_api_settings_from_json(txt, &base, &out, &se);
        if (sr == HY_OK) {
            /* the mqtt and webhooks members: a bad one fails the preview with its field */
            sr = hy_api_integ_check_json(txt, &ie);
            if (sr != HY_OK) {
                if (txt) wipe(txt, strlen(txt));
                cJSON_free(txt);
                return hy_api_fail(err, sr, sr == HY_E_INVALID ? ie.field : NULL);
            }
        }
        if (sr != HY_OK) {
            if (txt) wipe(txt, strlen(txt));
            cJSON_free(txt);
            return hy_api_fail(err, HY_E_INVALID, "settings");
        }
        size_t n = strlen(txt);
        imp->settings = malloc(n + 1);
        if (imp->settings) memcpy(imp->settings, txt, n + 1);
        wipe(txt, n);
        cJSON_free(txt);
        if (!imp->settings) return hy_api_fail(err, HY_E_IO, NULL);
        const cJSON *mq = member(set, "mqtt");
        imp->integ_secrets = cJSON_IsObject(mq) && cJSON_IsString(member(mq, "password"));
        const cJSON *wh = member(set, "webhooks"), *h;
        cJSON_ArrayForEach(h, wh)
            if (cJSON_IsObject(h) && cJSON_IsString(member(h, "secret"))) imp->integ_secrets = true;
    }

    cJSON *w = member(root, "wifi");
    if (w) {
        hy_err_t r = parse_wifi(w, &imp->wifi, err);
        if (r != HY_OK) return r;
        imp->has_wifi = true;
    }
    return HY_OK;
}

void hy_import_free(hy_import_t *imp)
{
    if (!imp) return;
    if (imp->settings) wipe(imp->settings, strlen(imp->settings));   /* it can hold the MQTT password and secrets */
    free(imp->settings);
    imp->settings = NULL;
    imp->integ_secrets = false;
    wipe(&imp->wifi, sizeof imp->wifi);
    imp->has_wifi = false;
}

hy_err_t hy_import_parse(const char *json, hy_import_t *imp, hy_api_err_t *err)
{
    hy_api_err_t dummy;
    if (!err) err = &dummy;
    if (!imp) return hy_api_fail(err, HY_E_INVALID, NULL);
    memset(imp, 0, sizeof *imp);
    if (!json) return hy_api_fail(err, HY_E_INVALID, "json");

    log_span_t span;
    bool dup_log, deep;
    if (!find_log(json, &span, &dup_log, &deep)) {
        /* a cut-off log array is the usual way a file breaks: name it. Too deep is "json". */
        return hy_api_fail(err, HY_E_INVALID, !deep && span.member && !span.end ? "log" : "json");
    }
    if (dup_log || (span.member && *span.val != '[')) return hy_api_fail(err, HY_E_INVALID, "log");

    char *head = head_without_log(json, &span);
    if (!head) return hy_api_fail(err, HY_E_IO, NULL);
    cJSON *root = cJSON_ParseWithOpts(head, NULL, true);
    size_t head_len = strlen(head);
    wipe(head, head_len);   /* it can hold Wi-Fi passwords */
    free(head);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return hy_api_fail(err, HY_E_INVALID, "json");
    }
    hy_err_t r = hy_api_reject_dups(root, err);
    if (r == HY_OK) r = parse_head(root, imp, err);
    wipe_strings(cJSON_GetObjectItemCaseSensitive(root, "wifi"));
    hy_api_integ_wipe_tree(cJSON_GetObjectItemCaseSensitive(root, "settings"));
    cJSON_Delete(root);
    if (r != HY_OK) {
        hy_import_free(imp);
        return r;
    }

    if (span.member) {
        imp->log = span.val;
        hy_import_cursor_t *c = malloc(sizeof *c);
        if (!c) {
            hy_import_free(imp);
            return hy_api_fail(err, HY_E_IO, NULL);
        }
        hy_log_t e;
        int k;
        hy_import_log_begin(imp, c);
        while ((k = hy_import_log_next(c, &e)) == 1) imp->n_log++;
        bool ok = k == 0 && c->p == span.end;
        free(c);
        if (!ok) {
            hy_import_free(imp);
            imp->log = NULL;
            return hy_api_fail(err, HY_E_INVALID, "log");
        }
    }
    return HY_OK;
}

/* ---------------------------------------------------------------- plan */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static const hy_import_med_t *map_of(const hy_import_t *imp, uint32_t file_med)
{
    for (size_t i = 0; i < imp->n_meds; i++)
        if (imp->map[i].file_id == file_med) return &imp->map[i];
    return NULL;
}

bool hy_import_target(const hy_import_t *imp, uint32_t file_med, uint32_t *dev_med, uint8_t *kind)
{
    const hy_import_med_t *m = imp ? map_of(imp, file_med) : NULL;
    if (!m) return false;
    if (kind) *kind = m->kind;
    if (m->dev_id == 0) return false;
    if (dev_med) *dev_med = m->dev_id;
    return true;
}

void hy_import_set_dev_id(hy_import_t *imp, uint32_t file_med, uint32_t dev_id)
{
    for (size_t i = 0; imp && i < imp->n_meds; i++)
        if (imp->map[i].file_id == file_med) imp->map[i].dev_id = dev_id;
}

/* Checks the medicine list the import leaves: the limits, then every new or changed record against it. */
static hy_err_t check_result(const hy_med_t *list, size_t n, const bool *touched, hy_api_err_t *err)
{
    size_t active = 0;
    for (size_t i = 0; i < n; i++)
        if (!list[i].archived) active++;
    if (n > HY_MAX_STORED || active > HY_MAX_MEDS) return hy_api_fail(err, HY_E_FULL, "meds");
    for (size_t i = 0; i < n; i++) {
        if (!touched[i]) continue;
        hy_err_t r = hy_med_validate(&list[i], list, n);
        if (r != HY_OK) return hy_api_fail(err, r, NULL);
    }
    return HY_OK;
}

hy_err_t hy_import_plan_parsed(hy_import_t *imp, hy_import_mode_t mode, const hy_med_t *cur, size_t n_cur,
                               hy_import_log_exists_t log_exists, void *ctx, hy_import_diff_t *diff,
                               hy_api_err_t *err)
{
    hy_api_err_t dummy;
    if (!err) err = &dummy;
    if (!imp || !diff || (n_cur && !cur) || n_cur > HY_MAX_STORED) return hy_api_fail(err, HY_E_INVALID, NULL);
    memset(diff, 0, sizeof *diff);
    diff->settings = imp->settings != NULL;
    diff->secrets = imp->has_wifi || imp->integ_secrets;
    diff->gaps_add = imp->n_gaps;

    /* the resulting medicine list, for the rule check */
    hy_med_t *list = malloc(2 * HY_MAX_STORED * sizeof *list);
    bool *touched = calloc(2 * HY_MAX_STORED, sizeof *touched);
    hy_import_cursor_t *c = malloc(sizeof *c);
    uint32_t *ids = NULL;
    hy_err_t r = HY_OK;
    if (!list || !touched || !c) {
        r = hy_api_fail(err, HY_E_IO, NULL);
        goto out;
    }
    size_t n = 0;
    if (mode == HY_IMPORT_REPLACE) {
        for (size_t i = 0; i < imp->n_meds; i++) {
            imp->map[i] = (hy_import_med_t){ imp->meds[i].id, imp->meds[i].id, HY_IMP_ADD };
            list[n] = imp->meds[i];
            touched[n++] = true;
        }
        diff->meds_add = imp->n_meds;
    } else {
        if (n_cur) memcpy(list, cur, n_cur * sizeof *list);
        n = n_cur;
        uint32_t provisional = 0;   /* above every id on either side: never clashes in the check */
        for (size_t i = 0; i < n_cur; i++)
            if (cur[i].id > provisional) provisional = cur[i].id;
        for (size_t i = 0; i < imp->n_meds; i++)
            if (imp->meds[i].id > provisional) provisional = imp->meds[i].id;
        for (size_t i = 0; i < imp->n_meds; i++) {
            const hy_med_t *f = &imp->meds[i];
            size_t k;
            for (k = 0; k < n_cur; k++)
                if (cur[k].id == f->id) break;
            if (k < n_cur && strcmp(cur[k].code, f->code) == 0) {
                bool same = hy_api_med_etag(f) == hy_api_med_etag(&cur[k]);
                imp->map[i] = (hy_import_med_t){ f->id, f->id, same ? HY_IMP_SAME : HY_IMP_CHANGE };
                if (same) {
                    diff->meds_same++;
                } else {
                    diff->meds_change++;
                    list[k] = *f;
                    touched[k] = true;
                }
            } else {
                imp->map[i] = (hy_import_med_t){ f->id, 0, HY_IMP_ADD };
                diff->meds_add++;
                list[n] = *f;
                list[n].id = ++provisional;
                touched[n++] = true;
            }
        }
    }
    r = check_result(list, n, touched, err);
    if (r != HY_OK) goto out;

    if (mode == HY_IMPORT_REPLACE && imp->n_log) {
        ids = malloc(imp->n_log * sizeof *ids);
        if (!ids) {
            r = hy_api_fail(err, HY_E_IO, NULL);
            goto out;
        }
    }
    size_t n_ids = 0;
    hy_log_t e;
    int k;
    hy_import_log_begin(imp, c);
    while ((k = hy_import_log_next(c, &e)) == 1) {
        const hy_import_med_t *m = map_of(imp, e.med_id);
        if (!m) {
            diff->log_skip++;
            continue;
        }
        if (mode == HY_IMPORT_REPLACE) {
            if (e.id == 0 || e.id == 0xFFFFFFFFu || n_ids >= imp->n_log) {   /* the store refuses both */
                r = hy_api_fail(err, HY_E_INVALID, "log");
                goto out;
            }
            ids[n_ids++] = e.id;
            diff->log_add++;
        } else if (m->kind == HY_IMP_ADD || !log_exists || !log_exists(ctx, m->dev_id, e.ts, e.status)) {
            diff->log_add++;
        } else {
            diff->log_dup++;
        }
    }
    if (k < 0) {
        r = hy_api_fail(err, HY_E_INVALID, "log");
        goto out;
    }
    if (n_ids > 1) {
        qsort(ids, n_ids, sizeof *ids, cmp_u32);
        for (size_t i = 1; i < n_ids; i++)
            if (ids[i] == ids[i - 1]) {
                r = hy_api_fail(err, HY_E_INVALID, "log");
                goto out;
            }
    }
out:
    free(ids);
    free(c);
    free(touched);
    free(list);
    return r;
}

hy_err_t hy_import_plan(const char *json, hy_import_mode_t mode, const hy_med_t *cur, size_t n_cur,
                        hy_import_log_exists_t log_exists, void *ctx, hy_import_diff_t *diff, hy_api_err_t *err)
{
    hy_api_err_t dummy;
    if (!err) err = &dummy;
    hy_import_t *imp = malloc(sizeof *imp);
    if (!imp) return hy_api_fail(err, HY_E_IO, NULL);
    hy_err_t r = hy_import_parse(json, imp, err);
    if (r == HY_OK) r = hy_import_plan_parsed(imp, mode, cur, n_cur, log_exists, ctx, diff, err);
    hy_import_free(imp);
    free(imp);
    return r;
}

/* ---------------------------------------------------------------- export head */

static bool add_wifi(cJSON *root, const hy_backup_wifi_t *w)
{
    cJSON *o = cJSON_AddObjectToObject(root, "wifi");
    cJSON *arr = o ? cJSON_AddArrayToObject(o, "networks") : NULL;
    if (!arr) return false;
    for (size_t i = 0; i < w->n && i < HY_NET_MAX_KNOWN; i++) {
        cJSON *n = cJSON_CreateObject();
        if (!n) return false;
        cJSON_AddItemToArray(arr, n);
        if (!cJSON_AddStringToObject(n, "ssid", w->ssid[i]) || !cJSON_AddStringToObject(n, "pass", w->pass[i]))
            return false;
    }
    if (w->ap_pass[0] && !cJSON_AddStringToObject(o, "ap_pass", w->ap_pass)) return false;
    return true;
}

static bool build_head(cJSON *root, const hy_export_head_t *h)
{
    if (!cJSON_AddNumberToObject(root, "v", HY_SCHEMA_VERSION) || !cJSON_AddNumberToObject(root, "api", HY_API_VERSION) ||
        !cJSON_AddNumberToObject(root, "exported", (double)h->exported))
        return false;
    cJSON *meds = cJSON_AddArrayToObject(root, "meds");
    if (!meds) return false;
    for (size_t i = 0; i < h->n_meds; i++) {
        cJSON *m = hy_codec_med_obj(&h->meds[i]);
        if (!m) return false;
        cJSON_AddItemToArray(meds, m);
    }
    if (!cJSON_AddNumberToObject(root, "next_id", (double)h->next_id)) return false;
    cJSON *gaps = cJSON_AddArrayToObject(root, "gaps");
    if (!gaps) return false;
    for (size_t i = 0; i < h->n_gaps; i++) {
        char line[128];
        hy_gap_reason_t r = h->gap_reasons ? (hy_gap_reason_t)h->gap_reasons[i] : HY_GAP_OFF;
        if (hy_gap_to_jsonl(&h->gaps[i], r, line, sizeof line) < 0) continue;   /* an invalid gap is left out */
        cJSON *g = cJSON_Parse(line);
        if (!g) return false;
        cJSON_AddItemToArray(gaps, g);
    }
    if (h->settings) {
        char *s = h->integ ? hy_api_settings_full_json(h->settings, h->integ, h->integ_secrets)
                           : hy_api_settings_json(h->settings);
        cJSON *so = s ? cJSON_Parse(s) : NULL;
        if (s) wipe(s, strlen(s));   /* it can hold the MQTT password and webhook secrets */
        free(s);
        if (!so) return false;
        cJSON_AddItemToObject(root, "settings", so);
    }
    if (h->wifi && !add_wifi(root, h->wifi)) return false;
    return true;
}

char *hy_export_head_json(const hy_export_head_t *h)
{
    if (!h || (h->n_meds && !h->meds) || (h->n_gaps && !h->gaps)) return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    char *txt = build_head(root, h) ? cJSON_PrintUnformatted(root) : NULL;
    wipe_strings(cJSON_GetObjectItemCaseSensitive(root, "wifi"));
    hy_api_integ_wipe_tree(cJSON_GetObjectItemCaseSensitive(root, "settings"));
    cJSON_Delete(root);
    if (!txt) return NULL;
    size_t n = strlen(txt);
    static const char tail[] = ",\"log\":[";
    char *out = (n >= 2 && txt[n - 1] == '}') ? malloc(n - 1 + sizeof tail) : NULL;
    if (out) {
        memcpy(out, txt, n - 1);
        memcpy(out + n - 1, tail, sizeof tail);
    }
    wipe(txt, n);   /* it can hold Wi-Fi passwords */
    cJSON_free(txt);
    return out;
}
