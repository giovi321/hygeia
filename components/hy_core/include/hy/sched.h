#ifndef HY_SCHED_H
#define HY_SCHED_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "hy/types.h"
#include "hy/occ.h"
#include "hy/actions.h"

/*
 * Reminder scheduler: decides which doses are open (DUE or LATE), which become
 * missed, when the chime plays and when to evaluate next. Pure: no clock, no
 * storage, no network. Single-threaded (eval uses static scratch buffers). The
 * caller owns the state struct and evaluates it with
 * the current medicines, logs, snoozes and time.
 *
 * Rules:
 * - Only fixed, non-archived medicines. A medicine id not in `seen` is added
 *   with since = now. tracked_since(med) = max(tracking_since, seen.since);
 *   every state query passes the gap {0, tracked_since(med)}, so a slot whose
 *   window closed before tracking began is UNTRACKED (a HY_MISSED log for the
 *   slot still decides MISSED first).
 * - Open: per medicine the earliest occurrence in [now - 8 days, now] whose
 *   state is DUE or LATE; at most one per medicine; snoozed_until from the
 *   matching snooze (0 when none, or when its until is not after now). More
 *   than 16 open: the first 16 in order (base, then medicine order).
 * - to_miss: occurrences in [now - 8 days, now] whose state is MISSED and for
 *   which no log for that slot exists.
 * - newly_due: open entries whose (med, slot) is not in `announced`; they are
 *   appended to the ring.
 * - Chime: the set of open, unsnoozed (med, slot) is compared with `showing`
 *   from the previous eval. If it contains a pair not in the previous set:
 *   chime, chime_plays = 1, chime_next = now + 120. Else if n_showing > 0,
 *   chime_plays < 5 and now >= chime_next: chime, chime_plays++,
 *   chime_next += 120. If n_showing == 0: chime_plays = 0, chime_next = 0.
 * - next_wake: the minimum, over values strictly after now, of: each tracked
 *   medicine's next occurrence base after now; each open entry's
 *   base + late_after_min*60 and base + missed_after_min*60; each snooze
 *   until; chime_next when 0 < chime_plays < 5.
 * - Not tracking: n_open = 0, no lists, chime = false, next_wake = 0.
 */

#define HY_SCHED_MAX_OPEN   16
#define HY_CHIME_MAX_PLAYS  5
#define HY_CHIME_REPEAT_S   120

typedef struct { uint32_t med_id; int64_t slot; } hy_slot_ref_t;
typedef struct { uint32_t med_id; int64_t slot; int64_t base; hy_occ_state_t state; /* DUE or LATE */
                 int64_t snoozed_until; /* 0 = not snoozed */ } hy_open_t;

typedef struct {
    int64_t tracking_since;                                  /* 0 = not tracking */
    int64_t last_eval;
    struct { uint32_t med_id; int64_t since; } seen[HY_MAX_MEDS]; size_t n_seen;
    hy_slot_ref_t announced[32]; size_t announced_next;      /* ring of DUE-announced slots */
    hy_slot_ref_t showing[HY_SCHED_MAX_OPEN]; size_t n_showing_prev;
    int64_t chime_next; uint8_t chime_plays;
} hy_sched_t;

typedef struct {
    const hy_med_t *meds; size_t n_meds;
    const hy_log_t *logs; size_t n_logs;                     /* any medicines, ascending ts */
    const hy_snooze_t *snoozes; size_t n_snoozes;
    const char *tz; const hy_tzchange_t *last_change;       /* last_change may be NULL */
    int64_t now;
} hy_sched_in_t;

typedef struct {
    hy_open_t open[HY_SCHED_MAX_OPEN]; size_t n_open;        /* ordered by base, then medicine order */
    size_t n_showing;                                        /* open and not snoozed */
    hy_slot_ref_t newly_due[HY_SCHED_MAX_OPEN]; size_t n_newly_due;
    hy_slot_ref_t to_miss[HY_MAX_MEDS]; size_t n_to_miss;
    bool chime;                                              /* play one chime now */
    int64_t next_wake;                                       /* earliest instant > now to evaluate again; 0 = none */
} hy_sched_out_t;

void    hy_sched_init(hy_sched_t *s);
void    hy_sched_start_tracking(hy_sched_t *s, int64_t now); /* clock became valid, or jumped */
void    hy_sched_stop_tracking(hy_sched_t *s);               /* clock invalid: eval outputs nothing */
int64_t hy_sched_tracked_since(const hy_sched_t *s, uint32_t med_id); /* 0 when not tracking; tracking_since for a medicine not yet seen */
void    hy_sched_eval(hy_sched_t *s, const hy_sched_in_t *in, hy_sched_out_t *out);

#endif
