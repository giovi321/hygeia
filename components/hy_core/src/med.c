#include <string.h>
#include "hy/med.h"

void hy_med_defaults(hy_med_t *m)
{
    memset(m, 0, sizeof(*m));
    m->type = HY_SCHED_FIXED;
    m->weekdays = 0x7F;
    m->color = 0;
    m->early_window_min = 60;
    m->late_after_min = 30;
    m->missed_after_min = 120;
}

static bool code_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool code_ok(const char *code)
{
    size_t n = strnlen(code, sizeof(((hy_med_t *)0)->code));
    if (n < 1 || n > 2) {
        return false; /* n == 3 means no terminator: too long */
    }
    for (size_t i = 0; i < n; i++) {
        if (!code_char_ok(code[i])) {
            return false; /* ASCII only: UTF-8 bytes are >= 0x80 and fail */
        }
    }
    return true;
}

/* Plausible local date YYYYMMDD, or 0 = unset. */
static bool date_ok(int32_t d)
{
    if (d == 0) {
        return true;
    }
    int y = d / 10000, mo = (d / 100) % 100, day = d % 100;
    return d > 0 && y >= 2000 && y <= 9999 && mo >= 1 && mo <= 12 && day >= 1 && day <= 31;
}

static bool times_ok(const hy_med_t *m)
{
    if (m->weekdays & 0x80) {
        return false; /* bits above Sunday */
    }
    if (m->type != HY_SCHED_FIXED) {
        return true; /* as-needed: no times required */
    }
    if (m->n_times < 1 || m->n_times > HY_MAX_TIMES) {
        return false;
    }
    if ((m->weekdays & 0x7F) == 0) {
        return false;
    }
    for (size_t i = 0; i < m->n_times; i++) {
        if (m->times[i] >= 1440) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (m->times[j] == m->times[i]) {
                return false;
            }
        }
    }
    return true;
}

/*
 * Check order as listed in med.h.
 * Uniqueness is only enforced for a non-archived `m`, against non-archived
 * others with a different id. An archived medicine is not active, so it may
 * share a code.
 */
hy_err_t hy_med_validate(const hy_med_t *m, const hy_med_t *others, size_t n_others)
{
    if (!code_ok(m->code)) {
        return HY_E_CODE;
    }
    size_t name_len = strnlen(m->name, sizeof(m->name));
    if (name_len == 0 || name_len == sizeof(m->name)) {
        return HY_E_NAME; /* empty, or unterminated */
    }
    if (strnlen(m->dose, sizeof(m->dose)) == sizeof(m->dose)) {
        return HY_E_DOSE; /* unterminated */
    }
    if (m->color > 9) {
        return HY_E_INVALID;
    }
    if (!date_ok(m->active_from) || !date_ok(m->active_until)) {
        return HY_E_INVALID;
    }
    if (!times_ok(m)) {
        return HY_E_TIMES;
    }
    if (m->min_interval_min > HY_INTERVAL_MAX_MIN) {
        return HY_E_INTERVAL;
    }
    if (!m->archived) {
        for (size_t i = 0; i < n_others; i++) {
            const hy_med_t *o = &others[i];
            if (o->id == m->id || o->archived) {
                continue;
            }
            if (strncmp(o->code, m->code, sizeof(m->code)) == 0) {
                return HY_E_CODE_DUP;
            }
        }
    }
    return HY_OK;
}
