#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hy/sched.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Device-side scheduler runner. Everything here runs in the LVGL task: the evaluation uses static
 * buffers, and the actions it calls are single-threaded by contract. A reminder depends only on the RTC,
 * the store and the speaker, never on the network. The snapshot is rewritten by every evaluation: event
 * subscribers must not read it synchronously inside their callback (an evaluation publishes events).
 */

typedef struct {
    hy_sched_in_t in;                 /* meds, logs, snoozes, tz, last_change, now of the last evaluation */
    const hy_sched_t *sched;
    hy_sched_out_t out;
    bool tracking;
    bool flash;                       /* a chime was due but audio is unavailable: the UI flashes. Latched:
                                         stays set until hy_sched_take_flash() or until nothing is showing */
} hy_snapshot_t;

/* PSRAM log buffer of 4096 entries, event subscription (marks dirty). Call once, after the store,
 * actions and clock are up and before the UI starts. */
esp_err_t hy_sched_run_init(void);
/* LVGL task, every second. Evaluates when dirty, when the minute changed, when now >= next_wake, or when
   hy_clock_alarm_take() is true. Otherwise only returns. A clock step of more than 120 s from the monotonic
   timer without a hy_clock_set sync is not evaluated (a set in progress); after 5 such ticks it counts
   as a jump and tracking restarts.
   Untracked gaps: the first tracking start after boot, and each start after the clock was invalid, records
   the gap since the heartbeat (hy_gap_on_start) and writes a fresh one; a forward jump of HY_GAP_MIN_S or
   more records [pre, now) as JUMP. Each recorded gap publishes one HY_EV_UNTRACKED (hy_act_record_gap). */
void      hy_sched_run_tick(void);
const hy_snapshot_t *hy_sched_snapshot(void);
uint32_t  hy_sched_generation(void);  /* +1 per evaluation */
/* Returns the latched flash request and clears it (the UI calls it when it starts the flash). */
bool      hy_sched_take_flash(void);

/* The next hy_sched_run_tick evaluates (also after a zone set, which bumps no sync seq). */
void      hy_sched_run_invalidate(void);
/* next_wake of the last evaluation (epoch s), 0 when none or not tracking. Spinlock copy: any task. */
int64_t   hy_sched_run_next_wake(void);
/* Debug: duration of the log query in the last evaluation (us). */
uint32_t  hy_sched_run_query_us(void);

#ifdef __cplusplus
}
#endif
