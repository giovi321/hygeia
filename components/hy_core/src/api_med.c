#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api_int.h"
#include "codec_obj.h"
#include "hy/api.h"
#include "hy/codec.h"
#include "hy/med.h"

/* ---- etag and encoding ---- */

uint32_t hy_api_med_etag(const hy_med_t *m)
{
    if (!m) return 0;
    hy_med_t c = *m;
    c.pills_left = 0; /* a dose taken on the device must not make an open form stale */
    cJSON *o = hy_codec_med_obj(&c);
    if (!o) return 0;
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return 0;
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    free(s);
    return h;
}

/* The codec object with the etag added. NULL on failure. */
static cJSON *med_obj_with_etag(const hy_med_t *m)
{
    cJSON *o = hy_codec_med_obj(m);
    if (!o) return NULL;
    char tag[9];
    snprintf(tag, sizeof tag, "%08x", (unsigned)hy_api_med_etag(m));
    if (!cJSON_AddStringToObject(o, "etag", tag)) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

char *hy_api_med_json(const hy_med_t *m)
{
    if (!m) return NULL;
    cJSON *o = med_obj_with_etag(m);
    if (!o) return NULL;
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

char *hy_api_meds_json(const hy_med_t *m, size_t n)
{
    if (n > 0 && !m) return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr || !cJSON_AddNumberToObject(root, "api", HY_API_VERSION)) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return NULL;
    }
    cJSON_AddItemToObject(root, "meds", arr);
    for (size_t i = 0; i < n; i++) {
        cJSON *o = med_obj_with_etag(&m[i]);
        if (!o) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* ---- decoding ---- */

/* Members of a medicine body that the caller never sets. */
static bool protected_key(const char *k)
{
    return strcmp(k, "id") == 0 || strcmp(k, "etag") == 0 || strcmp(k, "archived") == 0 ||
           strcmp(k, "order") == 0;
}

/* Checks one string member of the body: type, UTF-8, length (size includes the NUL). */
static hy_err_t check_string(const cJSON *body, const char *key, size_t size, hy_err_t too_long,
                             hy_api_err_t *err)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(body, key);
    if (!it || cJSON_IsNull(it)) return HY_OK;
    if (!cJSON_IsString(it) || it->valuestring == NULL) return hy_api_fail(err, HY_E_INVALID, key);
    if (!hy_api_utf8_valid(it->valuestring)) return hy_api_fail(err, HY_E_INVALID, key);
    if (strlen(it->valuestring) >= size) return hy_api_fail(err, too_long, key);
    return HY_OK;
}

/* Puts a copy of item into obj under its name, replacing a member of that name. */
static bool put_copy(cJSON *obj, const cJSON *item)
{
    cJSON *dup = cJSON_Duplicate(item, true);
    if (!dup) return false;
    if (cJSON_GetObjectItemCaseSensitive(obj, item->string)) {
        if (!cJSON_ReplaceItemInObjectCaseSensitive(obj, item->string, dup)) {
            cJSON_Delete(dup);
            return false;
        }
        return true;
    }
    if (!cJSON_AddItemToObject(obj, item->string, dup)) {
        cJSON_Delete(dup);
        return false;
    }
    return true;
}

/* Overlays the members of the object `from` onto the object member `key` of `obj` (null members skipped). */
static hy_err_t overlay_nested(cJSON *obj, const cJSON *from, const char *key, hy_api_err_t *err)
{
    cJSON *dst = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsObject(dst)) return hy_api_fail(err, HY_E_INVALID, key);
    for (const cJSON *it = from->child; it; it = it->next) {
        if (!it->string || cJSON_IsNull(it)) continue;
        if (!put_copy(dst, it)) return hy_api_fail(err, HY_E_IO, NULL);
    }
    return HY_OK;
}

hy_err_t hy_api_med_from_json(const char *body, const hy_med_t *base, hy_med_t *out, hy_api_err_t *err)
{
    if (!body || !out) return hy_api_fail(err, HY_E_INVALID, NULL);
    cJSON *in = cJSON_ParseWithOpts(body, NULL, true);
    if (!in || !cJSON_IsObject(in)) {
        cJSON_Delete(in);
        return hy_api_fail(err, HY_E_INVALID, NULL);
    }
    hy_med_t b;
    if (base) {
        b = *base;
    } else {
        hy_med_defaults(&b);
        b.id = 0;
        b.code[0] = '\0';
        b.name[0] = '\0';
    }
    hy_err_t r = check_string(in, "code", sizeof b.code, HY_E_CODE, err);
    if (r == HY_OK) r = check_string(in, "name", sizeof b.name, HY_E_NAME, err);
    if (r == HY_OK) r = check_string(in, "dose", sizeof b.dose, HY_E_DOSE, err);

    cJSON *obj = NULL;
    if (r == HY_OK) {
        obj = hy_codec_med_obj(&b);
        if (!obj) r = hy_api_fail(err, HY_E_INVALID, NULL);
    }
    for (const cJSON *it = in->child; r == HY_OK && it; it = it->next) {
        if (!it->string || cJSON_IsNull(it) || protected_key(it->string)) continue;
        if (strcmp(it->string, "schedule") == 0 || strcmp(it->string, "pill_count") == 0) {
            if (!cJSON_IsObject(it)) r = hy_api_fail(err, HY_E_INVALID, it->string);
            else r = overlay_nested(obj, it, it->string, err);
        } else if (!put_copy(obj, it)) {
            r = hy_api_fail(err, HY_E_IO, NULL);
        }
    }
    if (r == HY_OK) {
        hy_med_t m;
        r = hy_codec_med_from_obj(obj, &m);
        if (r != HY_OK) r = hy_api_fail(err, HY_E_INVALID, NULL);
        else *out = m;
    }
    cJSON_Delete(obj);
    cJSON_Delete(in);
    return r;
}

hy_err_t hy_api_order_from_json(const char *body, const hy_med_t *meds, size_t n, uint32_t *ids, size_t *n_ids,
                                hy_api_err_t *err)
{
    if (!body || !ids || !n_ids || (n > 0 && !meds)) return hy_api_fail(err, HY_E_INVALID, NULL);
    cJSON *o = cJSON_ParseWithOpts(body, NULL, true);
    cJSON *arr = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, "ids") : NULL;
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(o);
        return hy_api_fail(err, HY_E_INVALID, "ids");
    }
    size_t active = 0;
    for (size_t i = 0; i < n; i++)
        if (!meds[i].archived) active++;
    hy_err_t r = HY_OK;
    size_t count = (size_t)cJSON_GetArraySize(arr);
    if (count != active) r = HY_E_INVALID;
    size_t k = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (r != HY_OK) break;
        double d = cJSON_IsNumber(it) ? it->valuedouble : 0;
        if (!(d >= 1 && d <= 4294967295.0) || d != floor(d)) { r = HY_E_INVALID; break; }
        uint32_t id = (uint32_t)d;
        bool known = false;
        for (size_t i = 0; i < n; i++)
            if (meds[i].id == id && !meds[i].archived) known = true;
        for (size_t j = 0; j < k; j++)
            if (ids[j] == id) known = false; /* a duplicate */
        if (!known) { r = HY_E_INVALID; break; }
        ids[k++] = id;
    }
    cJSON_Delete(o);
    if (r != HY_OK) return hy_api_fail(err, r, "ids");
    *n_ids = k;
    return HY_OK;
}
