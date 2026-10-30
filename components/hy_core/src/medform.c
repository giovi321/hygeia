#include <string.h>
#include "hy/medform.h"
#include "hy/occ.h"

#define DEFAULT_TIME 480

static const uint16_t k_intervals[] = { 0, 30, 60, 120, 180, 240, 360, 480, 720, 1440 };
#define N_INTERVALS (sizeof k_intervals / sizeof k_intervals[0])

/* Trims spaces, collapses runs of spaces to one. Returns the length, or -1 when it exceeds max. */
static int normalize(char *dst, size_t max, const char *src)
{
    size_t n = 0;
    bool pending_space = false;
    if (!src) src = "";
    for (const char *p = src; *p; p++) {
        if (*p == ' ') {
            pending_space = n > 0;
            continue;
        }
        if (pending_space) {
            if (n >= max) return -1;
            dst[n++] = ' ';
            pending_space = false;
        }
        if (n >= max) return -1;
        dst[n++] = *p;
    }
    dst[n] = '\0';
    return (int)n;
}

static bool code_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static const hy_med_t *find_active(const hy_med_t *meds, size_t n, uint32_t id)
{
    for (size_t i = 0; i < n; i++) {
        if (meds[i].id == id && !meds[i].archived) return &meds[i];
    }
    return NULL;
}

static void sort_times(hy_med_t *m)
{
    for (size_t i = 1; i < m->n_times && i < HY_MAX_TIMES; i++) {
        uint16_t v = m->times[i];
        size_t j = i;
        while (j > 0 && m->times[j - 1] > v) {
            m->times[j] = m->times[j - 1];
            j--;
        }
        m->times[j] = v;
    }
}

void hy_mf_new(hy_medform_t *f, const hy_med_t *meds, size_t n)
{
    memset(f, 0, sizeof *f);
    hy_med_defaults(&f->draft);
    f->draft.type = HY_SCHED_FIXED;
    f->draft.times[0] = DEFAULT_TIME;
    f->draft.n_times = 1;
    f->draft.color = hy_mf_default_color(meds, n);
    f->step = HY_MF_NAME;
}

hy_err_t hy_mf_edit(hy_medform_t *f, const hy_med_t *meds, size_t n, uint32_t id)
{
    const hy_med_t *live = id ? find_active(meds, n, id) : NULL;
    if (!live) return HY_E_NOT_FOUND;
    memset(f, 0, sizeof *f);
    f->draft = *live;
    if (f->draft.n_times > HY_MAX_TIMES) f->draft.n_times = HY_MAX_TIMES;
    sort_times(&f->draft);
    f->edit_id = id;
    f->step = HY_MF_REVIEW;
    return HY_OK;
}

hy_err_t hy_mf_set_name(hy_medform_t *f, const char *name)
{
    char buf[HY_MF_NAME_MAX + 1];
    int len = normalize(buf, HY_MF_NAME_MAX, name);
    if (len <= 0) return HY_E_NAME;
    buf[0] = upper(buf[0]);
    if (strcmp(buf, f->draft.name) != 0) {
        memcpy(f->draft.name, buf, (size_t)len + 1);
        f->dirty = true;
    }
    return HY_OK;
}

hy_err_t hy_mf_set_dose(hy_medform_t *f, const char *dose)
{
    char buf[HY_MF_DOSE_MAX + 1];
    int len = normalize(buf, HY_MF_DOSE_MAX, dose);
    if (len < 0) return HY_E_DOSE;
    if (strcmp(buf, f->draft.dose) != 0) {
        memcpy(f->draft.dose, buf, (size_t)len + 1);
        f->dirty = true;
    }
    return HY_OK;
}

hy_err_t hy_mf_check_code(const char *code, const hy_med_t *meds, size_t n, uint32_t self_id, uint32_t *dup_id)
{
    char c[3] = { 0, 0, 0 };
    size_t len = code ? strnlen(code, 3) : 0;
    if (len < 1 || len > 2) return HY_E_CODE;
    for (size_t i = 0; i < len; i++) {
        c[i] = upper(code[i]);
        if (!code_char_ok(c[i])) return HY_E_CODE;
    }
    for (size_t i = 0; i < n; i++) {
        if (meds[i].archived || meds[i].id == self_id) continue;
        if (strncmp(meds[i].code, c, sizeof c) == 0) {
            if (dup_id) *dup_id = meds[i].id;
            return HY_E_CODE_DUP;
        }
    }
    return HY_OK;
}

hy_err_t hy_mf_set_code(hy_medform_t *f, const char *code, const hy_med_t *meds, size_t n, uint32_t *dup_id)
{
    hy_err_t e = hy_mf_check_code(code, meds, n, f->edit_id, dup_id);
    if (e != HY_OK) return e;
    char c[3] = { 0, 0, 0 };
    for (size_t i = 0; i < 2 && code[i]; i++) c[i] = upper(code[i]);
    if (strcmp(c, f->draft.code) != 0) {
        memcpy(f->draft.code, c, sizeof c);
        f->dirty = true;
    }
    return HY_OK;
}

static bool code_free(const char *cand, const hy_med_t *meds, size_t n, uint32_t self_id)
{
    return hy_mf_check_code(cand, meds, n, self_id, NULL) == HY_OK;
}

void hy_mf_suggest_code(const char *name, const hy_med_t *meds, size_t n, uint32_t self_id, char out[3])
{
    char cand[3] = { 0, 0, 0 };
    out[0] = '\0';
    if (!name) name = "";
    while (*name == ' ') name++;

    char first = upper(name[0]);
    if (code_char_ok(first)) {
        /* 1. the first character */
        cand[0] = first; cand[1] = '\0';
        if (code_free(cand, meds, n, self_id)) { memcpy(out, cand, 3); return; }

        /* 2. first characters of the first two words */
        const char *sp = strchr(name, ' ');
        if (sp) {
            while (*sp == ' ') sp++;
            char second = upper(*sp);
            if (code_char_ok(second)) {
                cand[0] = first; cand[1] = second; cand[2] = '\0';
                if (code_free(cand, meds, n, self_id)) { memcpy(out, cand, 3); return; }
            }
        }

        /* 3. first character plus each later letter or digit */
        for (const char *p = name + 1; *p; p++) {
            char c = upper(*p);
            if (!code_char_ok(c)) continue;
            cand[0] = first; cand[1] = c; cand[2] = '\0';
            if (code_free(cand, meds, n, self_id)) { memcpy(out, cand, 3); return; }
        }
    }

    /* 4. A..Z, then 0..9 */
    for (int i = 0; i < 36; i++) {
        cand[0] = i < 26 ? (char)('A' + i) : (char)('0' + i - 26);
        cand[1] = '\0';
        if (code_free(cand, meds, n, self_id)) { memcpy(out, cand, 3); return; }
    }
}

uint8_t hy_mf_default_color(const hy_med_t *meds, size_t n)
{
    unsigned used[10] = { 0 };
    for (size_t i = 0; i < n; i++) {
        if (!meds[i].archived && meds[i].color <= 9) used[meds[i].color]++;
    }
    uint8_t best = 0;
    for (uint8_t c = 1; c < 10; c++) {
        if (used[c] < used[best]) best = c;
    }
    return best;
}

hy_err_t hy_mf_set_color(hy_medform_t *f, uint8_t color)
{
    if (color > 9) return HY_E_INVALID;
    if (f->draft.color != color) {
        f->draft.color = color;
        f->dirty = true;
    }
    return HY_OK;
}

void hy_mf_set_type(hy_medform_t *f, hy_sched_type_t t)
{
    if (f->draft.type != t) {
        f->draft.type = t;
        f->dirty = true;
    }
    if (t == HY_SCHED_FIXED && f->draft.n_times == 0) {
        f->draft.times[0] = DEFAULT_TIME;
        f->draft.n_times = 1;
        f->dirty = true;
    }
}

hy_err_t hy_mf_add_time(hy_medform_t *f, uint16_t min)
{
    hy_med_t *d = &f->draft;
    if (min >= 1440 || d->n_times >= HY_MAX_TIMES) return HY_E_TIMES;
    size_t pos = 0;
    while (pos < d->n_times && d->times[pos] < min) pos++;
    if (pos < d->n_times && d->times[pos] == min) return HY_E_TIMES;
    for (size_t i = d->n_times; i > pos; i--) d->times[i] = d->times[i - 1];
    d->times[pos] = min;
    d->n_times++;
    f->dirty = true;
    return HY_OK;
}

hy_err_t hy_mf_remove_time(hy_medform_t *f, size_t idx)
{
    hy_med_t *d = &f->draft;
    if (idx >= d->n_times || d->n_times <= 1) return HY_E_TIMES;
    for (size_t i = idx; i + 1 < d->n_times; i++) d->times[i] = d->times[i + 1];
    d->n_times--;
    d->times[d->n_times] = 0;
    f->dirty = true;
    return HY_OK;
}

uint16_t hy_mf_next_time_default(const hy_medform_t *f)
{
    static const uint16_t cand[] = { 480, 720, 1080, 1320 };
    for (size_t c = 0; c < sizeof cand / sizeof cand[0]; c++) {
        bool used = false;
        for (size_t i = 0; i < f->draft.n_times; i++) {
            if (f->draft.times[i] == cand[c]) used = true;
        }
        if (!used) return cand[c];
    }
    return DEFAULT_TIME;
}

size_t hy_mf_interval_options(uint16_t current, uint16_t *out, size_t max)
{
    size_t n = 0;
    bool extra = current >= 1 && current <= HY_INTERVAL_MAX_MIN;
    for (size_t i = 0; i < N_INTERVALS; i++) {
        if (k_intervals[i] == current) extra = false;
    }
    for (size_t i = 0; i < N_INTERVALS; i++) {
        if (extra && current < k_intervals[i]) {
            if (n < max) out[n++] = current;
            extra = false;
        }
        if (n < max) out[n++] = k_intervals[i];
    }
    return n;
}

hy_err_t hy_mf_set_interval(hy_medform_t *f, uint16_t min)
{
    if (min > HY_INTERVAL_MAX_MIN) return HY_E_INTERVAL;
    if (f->draft.min_interval_min != min) {
        f->draft.min_interval_min = min;
        f->dirty = true;
    }
    return HY_OK;
}

static hy_mf_step_t schedule_step(const hy_medform_t *f)
{
    return f->draft.type == HY_SCHED_AS_NEEDED ? HY_MF_INTERVAL : HY_MF_TIMES;
}

hy_mf_step_t hy_mf_next(const hy_medform_t *f)
{
    if (f->from_review) return HY_MF_REVIEW;
    switch (f->step) {
    case HY_MF_NAME:     return HY_MF_DOSE;
    case HY_MF_DOSE:     return HY_MF_CODE;
    case HY_MF_CODE:     return HY_MF_COLOR;
    case HY_MF_COLOR:    return HY_MF_TYPE;
    case HY_MF_TYPE:     return schedule_step(f);
    case HY_MF_TIMES:
    case HY_MF_INTERVAL:
    case HY_MF_REVIEW:   return HY_MF_REVIEW;
    }
    return HY_MF_REVIEW;
}

hy_mf_step_t hy_mf_prev(const hy_medform_t *f)
{
    switch (f->step) {
    case HY_MF_NAME:     return HY_MF_NAME;
    case HY_MF_DOSE:     return HY_MF_NAME;
    case HY_MF_CODE:     return HY_MF_DOSE;
    case HY_MF_COLOR:    return HY_MF_CODE;
    case HY_MF_TYPE:     return HY_MF_COLOR;
    case HY_MF_TIMES:
    case HY_MF_INTERVAL: return HY_MF_TYPE;
    case HY_MF_REVIEW:   return f->edit_id ? HY_MF_NAME : schedule_step(f);
    }
    return HY_MF_NAME;
}

int hy_mf_step_number(const hy_medform_t *f)
{
    switch (f->step) {
    case HY_MF_NAME:     return 1;
    case HY_MF_DOSE:     return 2;
    case HY_MF_CODE:     return 3;
    case HY_MF_COLOR:    return 4;
    case HY_MF_TYPE:     return 5;
    case HY_MF_TIMES:
    case HY_MF_INTERVAL: return 6;
    case HY_MF_REVIEW:   return 0;
    }
    return 0;
}

hy_mf_step_t hy_mf_step_for_err(hy_err_t e)
{
    switch (e) {
    case HY_E_NAME:      return HY_MF_NAME;
    case HY_E_DOSE:      return HY_MF_DOSE;
    case HY_E_CODE:
    case HY_E_CODE_DUP:  return HY_MF_CODE;
    case HY_E_INVALID:   return HY_MF_COLOR;
    case HY_E_TIMES:     return HY_MF_TIMES;
    case HY_E_INTERVAL:  return HY_MF_INTERVAL;
    default:             return HY_MF_REVIEW;
    }
}

hy_err_t hy_mf_build(const hy_medform_t *f, const hy_med_t *meds, size_t n, hy_med_t *out, hy_mf_step_t *bad)
{
    hy_err_t e;
    const hy_med_t *d = &f->draft;
    if (f->edit_id) {
        const hy_med_t *live = find_active(meds, n, f->edit_id);
        if (!live) {
            e = HY_E_NOT_FOUND;
            goto done;
        }
        *out = *live;
    } else {
        hy_med_defaults(out);
        out->id = 0;
        out->min_interval_min = 0;
        out->archived = false;
        uint16_t max_order = 0;
        for (size_t i = 0; i < n; i++) {
            if (meds[i].order > max_order) max_order = meds[i].order;
        }
        out->order = n ? (uint16_t)(max_order + 1) : 0;
    }
    memcpy(out->code, d->code, sizeof out->code);
    memcpy(out->name, d->name, sizeof out->name);
    memcpy(out->dose, d->dose, sizeof out->dose);
    out->color = d->color;
    out->type = d->type;
    if (d->type == HY_SCHED_AS_NEEDED) {
        out->n_times = 0;
        memset(out->times, 0, sizeof out->times);
        out->min_interval_min = d->min_interval_min;
    } else {
        out->n_times = d->n_times;
        memcpy(out->times, d->times, sizeof out->times);
    }
    e = hy_med_validate(out, meds, n);
done:
    if (bad) *bad = hy_mf_step_for_err(e);
    return e;
}

size_t hy_mf_due_on_save(const hy_med_t *built, const hy_med_t *old, int64_t now,
                         const char *posix_tz, int64_t *out, size_t max)
{
    hy_med_t tmp = *built;
    if (old) {
        size_t k = 0;
        for (size_t i = 0; i < built->n_times && i < HY_MAX_TIMES; i++) {
            bool present = false;
            for (size_t j = 0; j < old->n_times && j < HY_MAX_TIMES; j++) {
                if (old->times[j] == built->times[i]) present = true;
            }
            if (!present) tmp.times[k++] = built->times[i];
        }
        tmp.n_times = (uint8_t)k;
    }
    if (tmp.n_times == 0) return 0;
    int64_t from = now - (int64_t)built->missed_after_min * 60 + 1;
    return hy_occurrences(&tmp, from, now + 1, posix_tz, out, max);
}
