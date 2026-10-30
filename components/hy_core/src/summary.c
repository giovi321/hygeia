#include "hy/summary.h"
#include "hy/tzlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DAY_S 86400LL
#define DAY_SLOTS 32                              /* slots found around one local date: 8 times x 3 days + slack */
#define ROW_SCRATCH (HY_MAX_MEDS * HY_MAX_TIMES)

/* ---- civil calendar (proleptic Gregorian, days since 1970-01-01) ---- */

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

static int64_t date_to_days(int32_t date)
{
    return days_from_civil(date / 10000, (date / 100) % 100, date % 100);
}

int32_t hy_date_add(int32_t date, int days)
{
    int64_t y;
    int m, d;
    civil_from_days(date_to_days(date) + days, &y, &m, &d);
    return (int32_t)(y * 10000 + m * 100 + d);
}

/* ---- local time ---- */

int32_t hy_local_date(int64_t ts, const char *posix_tz)
{
    hy_saved_tz_t saved;
    if (!hy_tz_enter(&saved, posix_tz)) return 0;
    time_t t = (time_t)ts;
    struct tm tm;
    bool ok = localtime_r(&t, &tm) != NULL;
    hy_tz_leave(&saved);
    if (!ok) return 0;
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/* ---- shared helpers ---- */

static const char *tz_of(const hy_sched_in_t *in)
{
    return in->tz ? in->tz : "UTC0";
}

static bool fixed_active(const hy_med_t *m)
{
    return m->type == HY_SCHED_FIXED && !m->archived;
}

static hy_occ_state_t state_of(const hy_med_t *m, int64_t slot, const hy_sched_in_t *in, const hy_sched_t *s)
{
    hy_gap_t gap = { 0, hy_sched_tracked_since(s, m->id) };
    return hy_occ_state(m, slot, 0, in->now, tz_of(in), in->logs, in->logs ? in->n_logs : 0, &gap, 1,
                        in->last_change);
}

/* First log of this medicine for this slot with status TAKEN or SKIPPED (a taken one wins). */
static const hy_log_t *slot_log(const hy_sched_in_t *in, uint32_t med_id, int64_t slot)
{
    const hy_log_t *skipped = NULL;
    for (size_t i = 0; in->logs && i < in->n_logs; i++) {
        const hy_log_t *l = &in->logs[i];
        if (l->med_id != med_id || l->slot != slot) continue;
        if (l->status == HY_TAKEN) return l;
        if (l->status == HY_SKIPPED && !skipped) skipped = l;
    }
    return skipped;
}

/* Occurrences of m whose local date in tz is `date`, ascending. A local date spans at most
 * UTC-12 .. UTC+14, so the window around its UTC midnight is [-15 h, +40 h). */
static size_t day_slots(const hy_med_t *m, int32_t date, const char *tz, int64_t *out, size_t max)
{
    int64_t g = date_to_days(date) * DAY_S;
    int64_t raw[DAY_SLOTS];
    size_t n = hy_occurrences(m, g - 15 * 3600, g + 40 * 3600, tz, raw, DAY_SLOTS);
    size_t w = 0;
    for (size_t i = 0; i < n && w < max; i++)
        if (hy_local_date(raw[i], tz) == date) out[w++] = raw[i];
    return w;
}

/* ---- tile ---- */

static int32_t frac(int64_t from, int64_t span_s, int64_t now)
{
    if (from <= 0 || span_s <= 0) return 0;
    int64_t v = (now - from) * HY_RING_MAX / span_s;
    return v < 0 ? 0 : v > HY_RING_MAX ? HY_RING_MAX : (int32_t)v;
}

/* Latest taken dose of the medicine in [from, now]; 0 if none. */
static int64_t last_taken(const hy_sched_in_t *in, uint32_t med_id, int64_t from)
{
    int64_t best = 0;
    for (size_t i = 0; in->logs && i < in->n_logs; i++) {
        const hy_log_t *l = &in->logs[i];
        if (l->med_id == med_id && l->status == HY_TAKEN && l->ts >= from && l->ts <= in->now && l->ts > best)
            best = l->ts;
    }
    return best;
}

void hy_tile_view(const hy_med_t *m, const hy_sched_in_t *in, const hy_sched_t *s, hy_tile_view_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!m || !in) return;
    const int64_t now = in->now;
    const char *tz = tz_of(in);

    if (m->type == HY_SCHED_AS_NEEDED) {
        int64_t span = DAY_S;
        int64_t iv = (int64_t)m->min_interval_min * 60;
        if (iv > span) span = iv;
        int64_t taken = last_taken(in, m->id, now - span);
        out->kind = HY_TILE_AS_NEEDED;
        out->last_taken = taken;
        out->ring = m->min_interval_min ? frac(taken, iv, now) : 0;
        return;
    }

    int64_t prev = hy_prev_occurrence(m, now, tz);
    int64_t log_from = prev ? prev - (int64_t)m->early_window_min * 60 : now - DAY_S;
    int64_t taken = last_taken(in, m->id, log_from);
    int64_t next = 0;
    int64_t nx;
    if (hy_occurrences(m, now + 1, now + 1 + 8 * DAY_S, tz, &nx, 1) == 1) next = nx;
    out->last_taken = taken;

    hy_occ_state_t st = HY_OCC_TAKEN;
    if (prev) {
        st = state_of(m, prev, in, s);
        /* a dose taken early for the upcoming slot settles the display */
        if (next && (st == HY_OCC_DUE || st == HY_OCC_LATE || st == HY_OCC_MISSED)) {
            hy_occ_state_t sn = state_of(m, next, in, s);
            if (sn == HY_OCC_TAKEN || sn == HY_OCC_SKIPPED) st = HY_OCC_TAKEN;
        }
    }
    if (st == HY_OCC_DUE || st == HY_OCC_LATE) {
        out->kind = st == HY_OCC_DUE ? HY_TILE_DUE : HY_TILE_LATE;
        out->ring = HY_RING_MAX;
        for (size_t k = 0; in->snoozes && k < in->n_snoozes; k++) {
            const hy_snooze_t *z = &in->snoozes[k];
            if (z->med_id == m->id && z->slot <= now && z->until > now) {
                out->kind = HY_TILE_SNOOZED;
                out->snoozed_until = z->until;
                break;
            }
        }
        return;
    }
    if (st == HY_OCC_MISSED) {
        out->kind = HY_TILE_MISSED;
        out->ring = HY_RING_MAX;
        return;
    }
    if (!taken) {
        out->kind = HY_TILE_NO_DOSE;
        out->ring = (prev && next && next > prev) ? frac(prev, next - prev, now) : 0;
        return;
    }
    out->kind = HY_TILE_SINCE;
    out->ring = (next && next > taken) ? frac(taken, next - taken, now) : 0;
}

/* ---- today rows ---- */

typedef struct { int64_t slot; uint16_t order; size_t idx; } slot_ref_t;

static bool ref_before(const slot_ref_t *x, const slot_ref_t *y)
{
    if (x->slot != y->slot) return x->slot < y->slot;
    if (x->order != y->order) return x->order < y->order;
    return x->idx < y->idx;
}

/* insertion sort: slot, then medicine order, then position in the list (as the scheduler orders) */
static void sort_refs(slot_ref_t *r, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        slot_ref_t c = r[i];
        size_t j = i;
        while (j > 0 && ref_before(&c, &r[j - 1])) {
            r[j] = r[j - 1];
            j--;
        }
        r[j] = c;
    }
}

size_t hy_today_rows(const hy_sched_in_t *in, const hy_sched_t *s, hy_today_row_t *out, size_t max,
                     uint16_t *taken, uint16_t *total)
{
    uint16_t n_taken = 0, n_total = 0;
    size_t n_out = 0;
    if (in && in->meds) {
        static HY_EXT_BSS slot_ref_t refs[ROW_SCRATCH];
        size_t nr = 0;
        const char *tz = tz_of(in);
        int32_t today = hy_local_date(in->now, tz);

        for (size_t i = 0; i < in->n_meds; i++) {
            const hy_med_t *m = &in->meds[i];
            if (!fixed_active(m)) continue;
            int64_t sl[DAY_SLOTS];
            size_t n = day_slots(m, today, tz, sl, DAY_SLOTS);
            for (size_t k = 0; k < n && nr < ROW_SCRATCH; k++) {
                refs[nr].slot = sl[k];
                refs[nr].order = m->order;
                refs[nr].idx = i;
                nr++;
            }
        }
        sort_refs(refs, nr);

        for (size_t r = 0; r < nr; r++) {
            const hy_med_t *m = &in->meds[refs[r].idx];
            hy_occ_state_t st = state_of(m, refs[r].slot, in, s);
            if (st == HY_OCC_TAKEN) n_taken++;
            if (st != HY_OCC_UNTRACKED) n_total++;
            if (out && n_out < max) {
                const hy_log_t *l = (st == HY_OCC_TAKEN || st == HY_OCC_SKIPPED) ? slot_log(in, m->id, refs[r].slot) : NULL;
                hy_today_row_t *row = &out[n_out++];
                memset(row, 0, sizeof *row);
                row->med_id = m->id;
                row->slot = refs[r].slot;
                row->ts = l ? l->ts : 0;
                row->state = st;
            }
        }

        for (size_t i = 0; i < in->n_meds; i++) {
            const hy_med_t *m = &in->meds[i];
            if (m->type != HY_SCHED_AS_NEEDED || m->archived) continue;
            uint16_t cnt = 0;
            int64_t last = 0;
            for (size_t k = 0; in->logs && k < in->n_logs; k++) {
                const hy_log_t *l = &in->logs[k];
                if (l->med_id != m->id || l->status != HY_TAKEN || l->ts > in->now) continue;
                if (hy_local_date(l->ts, tz) != today) continue;
                cnt++;
                if (l->ts > last) last = l->ts;
            }
            if (out && n_out < max) {
                hy_today_row_t *row = &out[n_out++];
                memset(row, 0, sizeof *row);
                row->med_id = m->id;
                row->ts = last;
                row->state = cnt ? HY_OCC_TAKEN : HY_OCC_UPCOMING;
                row->as_needed = true;
                row->taken_today = cnt;
            }
        }
    }
    if (taken) *taken = n_taken;
    if (total) *total = n_total;
    return n_out;
}

/* ---- day marks ---- */

hy_mark_t hy_day_mark(const hy_med_t *m, int32_t date, const hy_sched_in_t *in, const hy_sched_t *s,
                      bool *in_progress)
{
    if (in_progress) *in_progress = false;
    if (!m || !in || m->archived) return HY_MARK_NONE;
    const char *tz = tz_of(in);

    if (m->type == HY_SCHED_AS_NEEDED) {
        for (size_t k = 0; in->logs && k < in->n_logs; k++) {
            const hy_log_t *l = &in->logs[k];
            if (l->med_id == m->id && l->status == HY_TAKEN && l->ts <= in->now && hy_local_date(l->ts, tz) == date)
                return HY_MARK_AS_NEEDED_TAKEN;
        }
        return HY_MARK_NONE;
    }

    int64_t sl[DAY_SLOTS];
    size_t n = day_slots(m, date, tz, sl, DAY_SLOTS);
    if (n == 0) return HY_MARK_NONE;

    bool is_today = hy_local_date(in->now, tz) == date;
    bool missed = false, late = false, on_time = false, skipped = false, resolved = false, open = false;
    size_t untracked = 0;
    for (size_t i = 0; i < n; i++) {
        hy_occ_state_t st = state_of(m, sl[i], in, s);
        switch (st) {
        case HY_OCC_MISSED: missed = resolved = true; break;
        case HY_OCC_SKIPPED: skipped = resolved = true; break;
        case HY_OCC_TAKEN: {
            const hy_log_t *l = slot_log(in, m->id, sl[i]);
            if (l && l->status == HY_TAKEN && l->delta_min >= (int32_t)m->late_after_min) late = true;
            else on_time = true;
            resolved = true;
            break;
        }
        case HY_OCC_UNTRACKED: untracked++; break;
        case HY_OCC_UPCOMING:
        case HY_OCC_DUE:
        case HY_OCC_LATE: open = true; break;
        }
    }
    if (in_progress) *in_progress = is_today && open;
    if (missed) return HY_MARK_MISSED;
    if (late) return HY_MARK_LATE;
    if (on_time) return HY_MARK_ON_TIME;
    if (skipped) return HY_MARK_SKIPPED;
    if (!resolved && untracked == n) return HY_MARK_UNTRACKED;
    return HY_MARK_NONE;
}

/* ---- adherence ---- */

void hy_adherence(uint32_t med_id, int32_t first_date, int days, const hy_sched_in_t *in, const hy_sched_t *s,
                  hy_adherence_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!in || !in->meds) return;
    const char *tz = tz_of(in);
    int32_t date = first_date;
    for (int d = 0; d < days; d++, date = hy_date_add(date, 1)) {
        for (size_t i = 0; i < in->n_meds; i++) {
            const hy_med_t *m = &in->meds[i];
            if (!fixed_active(m) || (med_id != 0 && m->id != med_id)) continue;
            int64_t sl[DAY_SLOTS];
            size_t n = day_slots(m, date, tz, sl, DAY_SLOTS);
            for (size_t k = 0; k < n; k++) {
                hy_occ_state_t st = state_of(m, sl[k], in, s);
                if (st == HY_OCC_TAKEN) {
                    if (out->taken < UINT16_MAX) out->taken++;
                    if (out->resolved < UINT16_MAX) out->resolved++;
                } else if ((st == HY_OCC_SKIPPED || st == HY_OCC_MISSED) && out->resolved < UINT16_MAX) {
                    out->resolved++;
                }
            }
        }
    }
}

/* ---- delta text ---- */

void hy_delta_text(int32_t delta_min, char *buf, size_t len)
{
    if (!buf || len == 0) return;
    int32_t a = delta_min < 0 ? -delta_min : delta_min;
    if (a < 5) {
        snprintf(buf, len, "on time");
        return;
    }
    const char *sign = delta_min < 0 ? "\xE2\x88\x92" : "+";
    if (a < 120) snprintf(buf, len, "%s%d min", sign, (int)a);
    else snprintf(buf, len, "%s%d h %02d min", sign, (int)(a / 60), (int)(a % 60));
}

bool hy_due_within(const hy_med_t *meds, size_t n, const char *posix_tz, int64_t now, int64_t horizon_s)
{
    if (!meds || horizon_s <= 0) return false;
    for (size_t i = 0; i < n; i++) {
        const hy_med_t *m = &meds[i];
        if (!fixed_active(m)) continue;
        int64_t slot;
        /* hy_occurrences covers [from, to): (now, now + horizon_s] is [now + 1, now + horizon_s + 1) */
        if (hy_occurrences(m, now + 1, now + horizon_s + 1, posix_tz, &slot, 1) > 0) return true;
    }
    return false;
}
