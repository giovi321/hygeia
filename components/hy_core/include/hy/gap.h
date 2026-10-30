#ifndef HY_GAP_H
#define HY_GAP_H

#include <stdbool.h>
#include <stdint.h>
#include "hy/types.h"

/* Shortest interval recorded as untracked, in seconds. */
#define HY_GAP_MIN_S 120

typedef enum { HY_GAP_OFF = 0, HY_GAP_LOST, HY_GAP_CLOCK, HY_GAP_JUMP } hy_gap_reason_t;

/* NVS heartbeat: alive is the last time the device was known running (epoch s, 0 = never
 * written); clean is true when the last stop was a power-off through the PMU. */
typedef struct { int64_t alive; bool clean; } hy_alive_t;

/*
 * Tracking starts at now (boot, or the clock became valid). The gap is [a->alive, now) when
 * a->alive > 0 and now - a->alive >= HY_GAP_MIN_S. Reason: CLOCK when clock_was_invalid, else
 * OFF when a->clean, else LOST. Returns false when a is NULL, alive is 0, the gap is shorter
 * than HY_GAP_MIN_S, or alive > now (the caller logs that case).
 */
bool hy_gap_on_start(const hy_alive_t *a, int64_t now, bool clock_was_invalid, hy_gap_t *out,
                     hy_gap_reason_t *why);

/* Forward clock jump while tracking: [pre, now) when now - pre >= HY_GAP_MIN_S; backward jumps give false. */
bool hy_gap_on_jump(int64_t pre, int64_t now, hy_gap_t *out);

/*
 * Pending forward clock jump (ruling R12). A JUMP gap is held in RAM for HY_GAP_CONFIRM_S after its end
 * before it is recorded, so a mistaken forward time set that is corrected soon after leaves no untracked
 * band over time the device did track. The caller records and publishes only what these functions hand out.
 */
#define HY_GAP_CONFIRM_S 600

typedef struct { bool active; hy_gap_t gap; } hy_gap_pending_t;

typedef enum {
    HY_GAP_PEND_NONE = 0,   /* nothing pending */
    HY_GAP_PEND_KEPT,       /* the clock went back to at or after the gap's end: unchanged */
    HY_GAP_PEND_CLIPPED,    /* end moved back to now, still HY_GAP_MIN_S or longer */
    HY_GAP_PEND_DROPPED,    /* now before start + HY_GAP_MIN_S: no gap left */
} hy_gap_back_t;

/* Forward jump from pre to now. When it forms a gap (hy_gap_on_jump), a gap already pending is handed out in
 * *flushed (return true) and [pre, now) becomes the pending one. A shorter step changes nothing (false). */
bool hy_gap_pend_jump(hy_gap_pending_t *p, int64_t pre, int64_t now, hy_gap_t *flushed);

/* The clock was set back to now: clip the pending gap to [start, now) or drop it (see hy_gap_back_t). */
hy_gap_back_t hy_gap_pend_back(hy_gap_pending_t *p, int64_t now);

/* True, with the gap in *out and nothing pending afterwards, once now - end >= HY_GAP_CONFIRM_S. */
bool hy_gap_pend_due(hy_gap_pending_t *p, int64_t now, hy_gap_t *out);

/* Hands out the pending gap at once (the clock turned invalid); false when none. */
bool hy_gap_pend_take(hy_gap_pending_t *p, hy_gap_t *out);

/* "off", "lost", "clock", "jump"; "off" for an out-of-range value. */
const char *hy_gap_reason_str(hy_gap_reason_t r);

#endif /* HY_GAP_H */
