#include "hy/occ.h"
#include "hy/tzlock.h"

#include <stdbool.h>
#include <stdio.h>
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
    if (!found) return false;
    /* Fall-back overlap: the same wall-clock minute may also exist earlier.
     * Probe instead of trusting the libc to honour both tm_isdst values. */
    static const int64_t probes[] = { 3600, 1800 };
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        if (wall_matches((time_t)(best - probes[i]), y, mon, d, hour, min)) {
            best -= probes[i];
            break;
        }
    }
    *out = best;
    return true;
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

/* The previous TZ is saved in a fixed buffer (no allocation that could fail).
 * A previous TZ that does not fit is never overwritten: tz_enter returns false,
 * leaves TZ unchanged and the caller must not compute local times. Our own
 * zones are at most HY_TZ_LEN - 1 characters, so this only guards foreign TZs. */
#define SAVED_TZ_LEN 64

typedef struct {
    bool had;
    char value[SAVED_TZ_LEN];
} saved_tz_t;

/* tz_enter takes the process-wide TZ lock before saving the old TZ and keeps it on success;
 * tz_leave restores the old TZ and only then releases the lock. On failure tz_enter releases
 * the lock itself. */
static bool tz_enter(saved_tz_t *s, const char *posix_tz)
{
    hy_tz_lock();
    const char *cur = getenv("TZ");
    s->had = cur != NULL;
    s->value[0] = '\0';
    if (cur) {
        size_t n = strlen(cur);
        if (n >= sizeof s->value) {
            hy_tz_unlock();
            return false;
        }
        memcpy(s->value, cur, n + 1);
    }
    setenv("TZ", posix_tz ? posix_tz : "UTC0", 1);
    tzset();
    return true;
}

static void tz_leave(const saved_tz_t *s)
{
    if (s->had) setenv("TZ", s->value, 1);
    else unsetenv("TZ");
    tzset();
    hy_tz_unlock();
}

/* Local wall-clock time of a UTC instant in posix_tz, as seconds since
 * 1970-01-01 00:00 of the local calendar. */
static bool local_wall(int64_t utc, const char *posix_tz, int64_t *out)
{
    saved_tz_t saved;
    if (!tz_enter(&saved, posix_tz)) return false;
    time_t t = (time_t)utc;
    struct tm tm;
    bool ok = localtime_r(&t, &tm) != NULL;
    tz_leave(&saved);
    if (!ok) return false;
    *out = days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) * 86400 +
           tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    return true;
}

/* ---- public ---- */

size_t hy_occurrences(const hy_med_t *m, int64_t from, int64_t to,
                      const char *posix_tz, int64_t *out, size_t max)
{
    if (!m || !out || max == 0 || to <= from) return 0;
    if (m->archived || m->type != HY_SCHED_FIXED || m->n_times == 0) return 0;

    saved_tz_t saved;
    if (!tz_enter(&saved, posix_tz)) return 0;

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

/* ---- dose matching ---- */

#define MATCH_LOOKBACK (8 * 86400LL)
#define MATCH_CHUNK 64

/* A slot is satisfied by a taken or skipped log for this medicine and slot. */
static bool slot_satisfied(const hy_med_t *m, int64_t slot, const hy_log_t *logs, size_t n_logs)
{
    for (size_t i = 0; i < n_logs; i++) {
        const hy_log_t *l = &logs[i];
        if (l->med_id == m->id && l->slot == slot &&
            (l->status == HY_TAKEN || l->status == HY_SKIPPED))
            return true;
    }
    return false;
}

/* The first occurrence after `after`, looking up to 8 days ahead; INT64_MAX if none. */
static int64_t next_occurrence(const hy_med_t *m, int64_t after, const char *posix_tz)
{
    int64_t out[1];
    if (hy_occurrences(m, after + 1, after + 1 + MATCH_LOOKBACK, posix_tz, out, 1) == 1)
        return out[0];
    return INT64_MAX;
}

int64_t hy_prev_occurrence(const hy_med_t *m, int64_t now, const char *posix_tz)
{
    int64_t buf[16];
    int64_t from = now - MATCH_LOOKBACK, last = 0;
    for (;;) {
        size_t n = hy_occurrences(m, from, now + 1, posix_tz, buf, 16);
        if (n == 0) break;
        last = buf[n - 1];
        if (n < 16) break;
        from = last + 1;
    }
    return last;
}

int64_t hy_match_slot(const hy_med_t *m, int64_t ts, const char *posix_tz,
                      const hy_log_t *logs, size_t n_logs)
{
    if (!m || m->archived || m->type != HY_SCHED_FIXED) return 0;
    if (!logs) n_logs = 0;

    int64_t early = (int64_t)m->early_window_min * 60;
    int64_t from = ts - MATCH_LOOKBACK;
    int64_t to = ts + early + 1; /* the slot at exactly ts + early is included */
    int64_t missed = (int64_t)m->missed_after_min * 60;
    int64_t occ[MATCH_CHUNK];
    int64_t cand = 0; /* earliest unsatisfied slot whose window contains ts */

    /* Walk the occurrences in ascending chunks so a long range is never truncated. */
    for (;;) {
        size_t n = hy_occurrences(m, from, to, posix_tz, occ, MATCH_CHUNK);
        for (size_t i = 0; i < n; i++) {
            int64_t s = occ[i];
            int64_t next = i + 1 < n ? occ[i + 1] : next_occurrence(m, s, posix_tz);
            if (ts < s - early || ts >= next) continue;
            if (slot_satisfied(m, s, logs, n_logs)) continue;
            if (cand == 0) {
                cand = s;
                /* Not yet past its missed deadline: the earliest slot wins. */
                if (ts < cand + missed) return cand;
                continue;
            }
            /* The earliest candidate is already missed: a later unsatisfied slot
             * whose early window [s - early, s) contains ts wins instead. */
            if (ts < s) return s;
        }
        if (n < MATCH_CHUNK) break;
        from = occ[n - 1] + 1;
    }
    return cand;
}

/* ---- slot states ---- */

static bool slot_taken(const hy_med_t *m, int64_t slot, const char *posix_tz,
                       const hy_log_t *logs, size_t n_logs)
{
    int64_t slot_wall = 0;
    bool have_wall = local_wall(slot, posix_tz, &slot_wall);
    for (size_t i = 0; i < n_logs; i++) {
        const hy_log_t *l = &logs[i];
        if (l->med_id != m->id || l->status != HY_TAKEN || l->slot == 0) continue;
        if (l->slot == slot) return true;
        /* West rule: the log's slot, in the log's own zone, is the same local
         * date and HH:MM as this slot in posix_tz. Zone offsets differ by at
         * most about a day, so logs further than 2 days away cannot match;
         * skipping them bounds the setenv/tzset cost on a long history. */
        int64_t dist = l->slot > slot ? l->slot - slot : slot - l->slot;
        if (dist > 2 * 86400LL) continue;
        int64_t log_wall;
        if (have_wall && local_wall(l->slot, l->tz, &log_wall) &&
            log_wall / 60 == slot_wall / 60)
            return true;
    }
    return false;
}

static bool slot_skipped(const hy_med_t *m, int64_t slot, const hy_log_t *logs, size_t n_logs)
{
    for (size_t i = 0; i < n_logs; i++) {
        const hy_log_t *l = &logs[i];
        if (l->med_id == m->id && l->status == HY_SKIPPED && l->slot == slot) return true;
    }
    return false;
}

/* East rule: the instant the state clock counts from. */
static int64_t state_base(int64_t slot, const char *posix_tz, const hy_tzchange_t *ch)
{
    if (!ch) return slot;
    int64_t w_old, w_new, w_slot;
    if (!local_wall(ch->at, ch->old_tz, &w_old) || !local_wall(ch->at, ch->new_tz, &w_new))
        return slot;
    if (w_new <= w_old) return slot; /* not a forward jump */
    if (!local_wall(slot, posix_tz, &w_slot)) return slot;
    if (w_slot > w_old && w_slot <= w_new) return ch->at;
    return slot;
}

hy_occ_state_t hy_occ_state(const hy_med_t *m, int64_t slot, int64_t next_slot, int64_t now,
                            const char *posix_tz,
                            const hy_log_t *logs, size_t n_logs,
                            const hy_gap_t *gaps, size_t n_gaps,
                            const hy_tzchange_t *last_change)
{
    (void)next_slot; /* see occ.h */
    if (!m) return HY_OCC_UPCOMING;
    if (!logs) n_logs = 0;
    if (!gaps) n_gaps = 0;

    if (slot_taken(m, slot, posix_tz, logs, n_logs)) return HY_OCC_TAKEN;
    if (slot_skipped(m, slot, logs, n_logs)) return HY_OCC_SKIPPED;

    int64_t base = state_base(slot, posix_tz, last_change);
    int64_t late = base + (int64_t)m->late_after_min * 60;
    int64_t missed = base + (int64_t)m->missed_after_min * 60;

    for (size_t i = 0; i < n_gaps; i++) {
        if (slot >= gaps[i].start && slot < gaps[i].end && missed <= gaps[i].end)
            return HY_OCC_UNTRACKED;
    }

    if (now < base) return HY_OCC_UPCOMING;
    if (now < late) return HY_OCC_DUE;
    if (now < missed) return HY_OCC_LATE;
    return HY_OCC_MISSED;
}

/* ---- as-needed interval ---- */

hy_err_t hy_min_interval_check(const hy_med_t *m, int64_t now,
                               const hy_log_t *logs, size_t n_logs, int32_t *since_min)
{
    if (since_min) *since_min = -1;
    if (!m) return HY_E_INVALID;
    if (!logs) n_logs = 0;

    bool found = false;
    int64_t last = 0;
    for (size_t i = 0; i < n_logs; i++) {
        const hy_log_t *l = &logs[i];
        if (l->med_id != m->id || l->status != HY_TAKEN || l->ts > now) continue;
        if (!found || l->ts > last) {
            last = l->ts;
            found = true;
        }
    }
    if (!found) return HY_OK;

    int64_t since = (now - last) / 60;
    if (since > INT32_MAX) since = INT32_MAX;
    if (since_min) *since_min = (int32_t)since;
    if (m->min_interval_min != 0 && since < m->min_interval_min) return HY_W_TOO_SOON;
    return HY_OK;
}

/* ---- formatting ---- */

void hy_format_local(int64_t ts, const char *posix_tz, char *buf, size_t len)
{
    static const char *const wday[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (!buf || len == 0) return;
    buf[0] = '\0';

    saved_tz_t saved;
    if (!tz_enter(&saved, posix_tz)) return;
    time_t t = (time_t)ts;
    struct tm tm;
    bool ok = localtime_r(&t, &tm) != NULL;
    tz_leave(&saved);
    if (!ok || tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_mon < 0 || tm.tm_mon > 11) return;

    snprintf(buf, len, "%s %02d %s %02d:%02d", wday[tm.tm_wday], tm.tm_mday, mon[tm.tm_mon],
             tm.tm_hour, tm.tm_min);
}
