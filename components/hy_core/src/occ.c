#include "hy/occ.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- civil calendar helpers (proleptic Gregorian, days since 1970-01-01) ---- */

static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2);
}

/* Monday = 0 ... Sunday = 6. 1970-01-01 was a Thursday. */
static int weekday_mon0(int64_t days)
{
    int64_t w = (days + 3) % 7;
    return (int)(w < 0 ? w + 7 : w);
}

/* ---- local time <-> UTC (the TZ environment must already be set) ---- */

static bool wall_matches(time_t t, int64_t y, int mon, int d, int hour, int min)
{
    struct tm back;
    if (!localtime_r(&t, &back)) return false;
    return back.tm_year + 1900 == y && back.tm_mon + 1 == mon && back.tm_mday == d &&
           back.tm_hour == hour && back.tm_min == min;
}

/* UTC instant for one exact local wall-clock minute. Returns false if that
 * wall-clock time does not exist (DST gap). If it occurs twice, the earlier
 * instant is returned. Validity is checked by converting back, so this does not
 * depend on how a given libc resolves tm_isdst = -1. */
static bool local_exact(int64_t y, int mon, int d, int hour, int min, int64_t *out)
{
    bool found = false;
    int64_t best = 0;
    for (int isdst = 0; isdst <= 1; isdst++) {
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        tm.tm_year = (int)(y - 1900);
        tm.tm_mon = mon - 1;
        tm.tm_mday = d;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_isdst = isdst;
        time_t t = mktime(&tm);
        if (t == (time_t)-1) continue;
        if (!wall_matches(t, y, mon, d, hour, min)) continue;
        if (!found || (int64_t)t < best) {
            best = (int64_t)t;
            found = true;
        }
    }
    if (found) *out = best;
    return found;
}

/* Local day `day` (days since epoch, local calendar) at `minute` after local
 * midnight. A nonexistent time moves forward minute by minute to the first
 * valid one. */
static int64_t local_to_utc(int64_t day, int minute)
{
    int64_t t = 0;
    for (int i = 0; i < 24 * 60; i++) {
        int64_t total = (int64_t)minute + i;
        int64_t dd = day + total / 1440;
        int mm = (int)(total % 1440);
        int64_t y;
        int mon, d;
        civil_from_days(dd, &y, &mon, &d);
        if (local_exact(y, mon, d, mm / 60, mm % 60, &t)) return t;
    }
    return t; /* unreachable for real zones */
}

/* Local calendar day number (days since 1970-01-01) of a UTC instant. */
static int64_t local_day_of(int64_t utc)
{
    time_t t = (time_t)utc;
    struct tm tm;
    if (!localtime_r(&t, &tm)) return utc / 86400;
    return days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* ---- TZ save / restore ---- */

typedef struct {
    bool had;
    char *value;
} saved_tz_t;

static void tz_enter(saved_tz_t *s, const char *posix_tz)
{
    const char *cur = getenv("TZ");
    s->had = cur != NULL;
    s->value = NULL;
    if (cur) {
        size_t n = strlen(cur) + 1;
        s->value = malloc(n);
        if (s->value) memcpy(s->value, cur, n);
    }
    setenv("TZ", posix_tz ? posix_tz : "UTC0", 1);
    tzset();
}

static void tz_leave(saved_tz_t *s)
{
    if (s->had && s->value) setenv("TZ", s->value, 1);
    else unsetenv("TZ");
    tzset();
    free(s->value);
    s->value = NULL;
}

/* ---- public ---- */

size_t hy_occurrences(const hy_med_t *m, int64_t from, int64_t to,
                      const char *posix_tz, int64_t *out, size_t max)
{
    if (!m || !out || max == 0 || to <= from) return 0;
    if (m->archived || m->type != HY_SCHED_FIXED || m->n_times == 0) return 0;

    saved_tz_t saved;
    tz_enter(&saved, posix_tz);

    size_t n = 0;
    int64_t first_day = local_day_of(from) - 1; /* safety margin */
    int64_t last_day = local_day_of(to);

    for (int64_t day = first_day; day <= last_day && n < max; day++) {
        if (!(m->weekdays & (1u << weekday_mon0(day)))) continue;

        int64_t y;
        int mon, d;
        civil_from_days(day, &y, &mon, &d);
        int32_t ymd = (int32_t)(y * 10000 + mon * 100 + d);
        if (m->active_from != 0 && ymd < m->active_from) continue;
        if (m->active_until != 0 && ymd > m->active_until) continue;

        int64_t today[HY_MAX_TIMES];
        size_t nt = 0;
        for (uint8_t i = 0; i < m->n_times && i < HY_MAX_TIMES; i++) {
            int64_t t = local_to_utc(day, m->times[i]);
            if (t >= from && t < to) today[nt++] = t;
        }
        qsort(today, nt, sizeof today[0], cmp_i64);
        for (size_t i = 0; i < nt && n < max; i++) {
            if (n > 0 && out[n - 1] == today[i]) continue; /* collapsed by a gap */
            out[n++] = today[i];
        }
    }

    tz_leave(&saved);
    return n;
}
