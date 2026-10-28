#ifndef HY_OCC_H
#define HY_OCC_H

#include <stddef.h>
#include <stdint.h>
#include "hy/types.h"

/*
 * Fills out[] with UTC epochs of scheduled occurrences in [from, to), ascending.
 * Uses posix_tz for local time (setenv("TZ") + tzset internally, restoring the
 * previous TZ, all inside hy_tz_lock from hy/tzlock.h). Honours weekdays,
 * active_from/active_until and archived (archived and as-needed medicines
 * produce none). Returns the count, at most max.
 *
 * Local-time rule: a time that does not exist (spring-forward gap) is scheduled
 * at the first valid minute after the gap; a time that occurs twice (fall-back)
 * is scheduled once, at its first occurrence.
 */
size_t hy_occurrences(const hy_med_t *m, int64_t from, int64_t to,
                      const char *posix_tz, int64_t *out, size_t max);

/*
 * The latest occurrence in [now - 8 days, now], or 0 if there is none. 8 days
 * covers a weekly schedule. Walks the range in chunks, so any number of daily
 * times is handled without truncation.
 */
int64_t hy_prev_occurrence(const hy_med_t *m, int64_t now, const char *posix_tz);

/* A time-zone change: at UTC instant `at` the zone switched from old_tz to new_tz. */
typedef struct { int64_t at; char old_tz[HY_TZ_LEN]; char new_tz[HY_TZ_LEN]; } hy_tzchange_t;

/*
 * Which occurrence a log at ts satisfies: the earliest unsatisfied slot whose
 * window [slot - early_window_min, next_slot) contains ts. next_slot is the
 * following occurrence of the same medicine. A slot is satisfied by a log for
 * this medicine with status HY_TAKEN or HY_SKIPPED and that slot (a HY_MISSED
 * log does not satisfy it). Exception: if that earliest unsatisfied slot is
 * already past its missed deadline at ts (ts >= slot + missed_after_min) and a
 * later unsatisfied slot's early window [s - early_window_min, s) contains ts,
 * the later slot wins, so a dose taken early for the upcoming slot is not
 * attributed to a missed one. Occurrences in [ts - 8 days, ts + early_window_min]
 * are considered. Returns the slot, or 0 for a free log, an as-needed or an
 * archived medicine.
 */
int64_t hy_match_slot(const hy_med_t *m, int64_t ts, const char *posix_tz,
                      const hy_log_t *logs, size_t n_logs);

/*
 * State of one occurrence at time now, evaluated in this order:
 * 1. TAKEN if a taken log for the slot exists, or (west rule) a taken log whose
 *    slot, rendered in the log's own tz, has the same local date and HH:MM as
 *    this slot rendered in posix_tz; else SKIPPED if a skipped log for the slot
 *    exists.
 * 2. UNTRACKED if the slot lies inside a gap [start, end) and its window closed
 *    (base + missed_after_min) at or before the gap end. `now` is not
 *    consulted: gaps are assumed to be recorded only once they have ended.
 * 3. From base: UPCOMING before base, DUE from base, LATE from base +
 *    late_after_min, MISSED from base + missed_after_min.
 * base is the slot, except (east rule) when last_change moved the wall clock
 * forward and the slot's local time in posix_tz lies inside the skipped
 * interval (old wall time at `at`, new wall time at `at`]: then base is
 * last_change->at, so the dose is DUE on arrival. last_change may be NULL.
 * next_slot is accepted for the caller's convenience but not used: matching
 * logs to slots (which needs the window end) is done by hy_match_slot.
 */
hy_occ_state_t hy_occ_state(const hy_med_t *m, int64_t slot, int64_t next_slot, int64_t now,
                            const char *posix_tz,
                            const hy_log_t *logs, size_t n_logs,
                            const hy_gap_t *gaps, size_t n_gaps,
                            const hy_tzchange_t *last_change);

/*
 * For as-needed: HY_W_TOO_SOON if the latest taken dose (ts <= now) is less
 * than min_interval_min ago, else HY_OK (always HY_OK when min_interval_min is
 * 0). *since_min receives minutes since that dose, or -1 if none.
 */
hy_err_t hy_min_interval_check(const hy_med_t *m, int64_t now,
                               const hy_log_t *logs, size_t n_logs, int32_t *since_min);

/* Formats ts in posix_tz as "Tue 27 Oct 22:04" (English names). Always
 * NUL-terminates when len > 0. */
void hy_format_local(int64_t ts, const char *posix_tz, char *buf, size_t len);

#endif
