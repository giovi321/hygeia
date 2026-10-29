#ifndef HY_SUMMARY_H
#define HY_SUMMARY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "hy/types.h"
#include "hy/occ.h"
#include "hy/sched.h"

/*
 * Pure summaries for the home tiles, the Overview today rows and the History day marks.
 * Every state query passes the gap {0, hy_sched_tracked_since(s, med)}, exactly as the
 * scheduler does, so tiles, rows and history agree with it. `s` may be NULL (no tracking
 * gap). The today-row, day-mark and adherence functions use static scratch buffers:
 * single-threaded, like hy_sched_eval. Local time work runs inside hy_tz_lock()/hy_tz_unlock().
 */

#define HY_RING_MAX 1000

typedef enum { HY_TILE_AS_NEEDED = 0, HY_TILE_NO_DOSE, HY_TILE_SINCE, HY_TILE_DUE, HY_TILE_LATE,
               HY_TILE_SNOOZED, HY_TILE_MISSED } hy_tile_kind_t;
typedef struct { hy_tile_kind_t kind; int32_t ring; int64_t last_taken; int64_t snoozed_until; } hy_tile_view_t;
/* Same rules as plan 1 home.c compute(): ring = elapsed fraction from the last taken dose (else the previous
   slot) to the next occurrence; full when due/late/missed; as-needed ring = elapsed fraction of
   min_interval since the last dose. SNOOZED replaces DUE/LATE while a snooze is active.
   NO_DOSE (a fixed medicine with no taken dose in the window) has ring 0. */
void hy_tile_view(const hy_med_t *m, const hy_sched_in_t *in, const hy_sched_t *s, hy_tile_view_t *out);

typedef struct { uint32_t med_id; int64_t slot; int64_t ts; hy_occ_state_t state;
                 bool as_needed; uint16_t taken_today; } hy_today_row_t;
/* Today's (local date in in->tz) occurrences of active medicines ascending by slot, then one row per
   as-needed medicine. ts = taken/skipped log ts or 0 (as-needed rows: the last dose today). *taken = TAKEN rows,
   *total = fixed rows that are not UNTRACKED (both may be NULL, both cover all rows, not only max). Returns
   the row count, at most max. As-needed rows have slot 0 and state TAKEN when taken_today > 0, else UPCOMING;
   they do not count in *taken or *total. */
size_t hy_today_rows(const hy_sched_in_t *in, const hy_sched_t *s, hy_today_row_t *out, size_t max,
                     uint16_t *taken, uint16_t *total);

typedef enum { HY_MARK_NONE = 0, HY_MARK_ON_TIME, HY_MARK_LATE, HY_MARK_MISSED, HY_MARK_SKIPPED,
               HY_MARK_UNTRACKED, HY_MARK_AS_NEEDED_TAKEN } hy_mark_t;
/* Aggregate of one medicine on one local date (YYYYMMDD in in->tz). Priority among the day's resolved slots:
   MISSED > LATE > ON_TIME > SKIPPED; all UNTRACKED -> UNTRACKED; no slots -> NONE; as-needed with a taken
   dose -> AS_NEEDED_TAKEN. LATE = taken with delta_min >= late_after_min. *in_progress = the date is today
   and a slot is still UPCOMING, DUE or LATE (in_progress may be NULL). */
hy_mark_t hy_day_mark(const hy_med_t *m, int32_t date, const hy_sched_in_t *in, const hy_sched_t *s,
                      bool *in_progress);

typedef struct { uint16_t taken, resolved; } hy_adherence_t;
/* Over [first_date, first_date + days) for one medicine (med_id) or all (0): resolved = TAKEN + SKIPPED + MISSED
   slots; UNTRACKED, UPCOMING, DUE and LATE are excluded. Percent shown = taken * 100 / resolved rounded half up;
   resolved 0 -> the UI shows an em dash. */
void hy_adherence(uint32_t med_id, int32_t first_date, int days, const hy_sched_in_t *in, const hy_sched_t *s,
                  hy_adherence_t *out);

/* "on time" for |delta| < 5, else "+28 min", "-12 min" (U+2212 minus), "+2 h 05 min" from 120 on. */
void hy_delta_text(int32_t delta_min, char *buf, size_t len);
/* Local date arithmetic: YYYYMMDD of ts in tz, and date + n days. */
int32_t hy_local_date(int64_t ts, const char *posix_tz);
int32_t hy_date_add(int32_t date, int days);

#endif
