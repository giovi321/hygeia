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

/* "off", "lost", "clock", "jump"; "off" for an out-of-range value. */
const char *hy_gap_reason_str(hy_gap_reason_t r);

#endif /* HY_GAP_H */
