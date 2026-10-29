#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hy/sched.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Device-side scheduler runner. Everything here runs in the LVGL task (or with the LVGL lock held):
 * the evaluation uses static buffers, and the actions it calls are single-threaded by contract.
 * A reminder depends only on the RTC, the store and the speaker, never on the network.
 */

typedef struct {
    hy_sched_in_t in;                 /* meds, logs, snoozes, tz, last_change, now of the last evaluation */
    const hy_sched_t *sched;
    hy_sched_out_t out;
    bool tracking;
    bool flash;                       /* a chime was due but audio is unavailable: the UI flashes */
} hy_snapshot_t;

/* PSRAM log buffer of 4096 entries, event subscription (marks dirty). Call once, after the store,
 * actions and clock are up and before the UI starts. */
esp_err_t hy_sched_run_init(void);
/* LVGL task, every second. Evaluates when dirty, when the minute changed, when now >= next_wake, or when
   hy_clock_alarm_take() is true. Otherwise only returns. */
void      hy_sched_run_tick(void);
const hy_snapshot_t *hy_sched_snapshot(void);
uint32_t  hy_sched_generation(void);  /* +1 per evaluation */

/* Debug: the next hy_sched_run_tick evaluates. */
void      hy_sched_run_invalidate(void);
/* Debug: duration of the log query in the last evaluation (us). */
uint32_t  hy_sched_run_query_us(void);

#ifdef __cplusplus
}
#endif
