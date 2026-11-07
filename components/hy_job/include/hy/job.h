#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

/*
 * Task to LVGL job bridge. A web handler or another task that needs the store, the scheduler snapshot or an action
 * (all single-writer, owned by the LVGL task) packs its input into the job context, runs the job, and reads the
 * result from the same context once hy_job_run returned ESP_OK.
 *
 * Lifetime: a job starts with refcount 2, one reference for the handler and one for the LVGL side. The LVGL side
 * drops its reference after fn returned and the semaphore was given (or at once when the job was never queued or was
 * abandoned before it started). The handler always calls hy_job_put exactly once, whatever hy_job_run returned. The
 * last put frees the job, so a job that outlives a timed-out handler stays valid until fn is done with it.
 *
 * Timeout: hy_job_run waits up to timeout_ms. A job that the LVGL task has not started by then is abandoned: fn never
 * runs. A job already running finishes, and its result is dropped. Either way ESP_ERR_TIMEOUT: the handler answers
 * 503 with Retry-After 2 and must not read the context.
 */

#define HY_JOB_TIMEOUT_MS 3000   /* every route except import and export pages */

typedef struct hy_job hy_job_t;

/* PSRAM job with ctx_size bytes of zeroed context and refcount 2. NULL on no memory. */
hy_job_t *hy_job_new(void (*fn)(void *ctx), size_t ctx_size);
void     *hy_job_ctx(hy_job_t *j);
/* Queues fn with lv_async_call under hy_board_lock(1000) and waits on the job's semaphore. Without a display (no
 * LVGL task) fn runs in the caller. hy_power_net_hold is already held while the radio is on, so LVGL is running.
 * ESP_OK: fn returned. ESP_ERR_TIMEOUT: see above (also when the lock or the queueing failed). */
esp_err_t hy_job_run(hy_job_t *j, uint32_t timeout_ms);
void      hy_job_put(hy_job_t *j);

#if CONFIG_HY_DEBUG_CONSOLE
/* The next job sleeps ms in the LVGL task before fn (0 = off). */
void hy_job_debug_slow(uint32_t ms);
#endif
