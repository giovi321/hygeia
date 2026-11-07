#include "hy/job.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hy/board.h"
#include "lvgl.h"

static const char *TAG = "job";

/* QUEUED -> RUNNING (LVGL task, before fn) or QUEUED -> ABANDONED (handler, at the timeout). Exactly one of the two
 * transitions wins, so fn never runs for a handler that already answered 503 before it started. */
enum { JOB_QUEUED = 0, JOB_RUNNING, JOB_ABANDONED };

struct hy_job {
    void (*fn)(void *ctx);
    atomic_int ref;
    atomic_int state;
    int64_t queued_us;
    StaticSemaphore_t sem_buf;
    SemaphoreHandle_t sem;
    size_t ctx_size;
    _Alignas(8) uint8_t ctx[];
};

#if CONFIG_HY_DEBUG_CONSOLE
static atomic_uint s_slow_ms;
#endif

hy_job_t *hy_job_new(void (*fn)(void *ctx), size_t ctx_size)
{
    size_t sz = sizeof(hy_job_t) + ctx_size;
    hy_job_t *j = heap_caps_calloc(1, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j) {
        j = heap_caps_calloc(1, sz, MALLOC_CAP_8BIT);
    }
    if (!j) {
        ESP_LOGE(TAG, "no memory for a job of %u bytes", (unsigned)sz);
        return NULL;
    }
    j->fn = fn;
    j->ctx_size = ctx_size;
    atomic_init(&j->ref, 2);
    atomic_init(&j->state, JOB_QUEUED);
    j->sem = xSemaphoreCreateBinaryStatic(&j->sem_buf);
    return j;
}

void *hy_job_ctx(hy_job_t *j)
{
    return j ? j->ctx : NULL;
}

void hy_job_put(hy_job_t *j)
{
    if (!j) {
        return;
    }
    if (atomic_fetch_sub(&j->ref, 1) == 1) {
        vSemaphoreDelete(j->sem);
        /* A context can hold a request body with secrets: overwrite all of it, also when the job never ran. */
        volatile unsigned char *v = (volatile unsigned char *)j->ctx;
        for (size_t n = j->ctx_size; n; n--) {
            *v++ = 0;
        }
        heap_caps_free(j);
    }
}

/* LVGL task (or the caller without a display) */
static void job_exec(void *arg)
{
    hy_job_t *j = arg;
    int expected = JOB_QUEUED;
    if (!atomic_compare_exchange_strong(&j->state, &expected, JOB_RUNNING)) {
        ESP_LOGW(TAG, "job abandoned before it started (queued %lld ms ago)",
                 (long long)((esp_timer_get_time() - j->queued_us) / 1000));
        hy_job_put(j);
        return;
    }
#if CONFIG_HY_DEBUG_CONSOLE
    uint32_t slow = atomic_exchange(&s_slow_ms, 0);
    if (slow) {
        ESP_LOGW(TAG, "webjob slow: sleeping %u ms in the LVGL task", (unsigned)slow);
        vTaskDelay(pdMS_TO_TICKS(slow));
    }
#endif
    j->fn(j->ctx);
    int64_t ms = (esp_timer_get_time() - j->queued_us) / 1000;
    if (ms > HY_JOB_TIMEOUT_MS) {
        ESP_LOGW(TAG, "job finished after %lld ms", (long long)ms);
    }
    xSemaphoreGive(j->sem);
    hy_job_put(j);
}

esp_err_t hy_job_run(hy_job_t *j, uint32_t timeout_ms)
{
    if (!j) {
        return ESP_ERR_INVALID_ARG;
    }
    j->queued_us = esp_timer_get_time();
    if (!hy_board_display()) {
        job_exec(j);   /* no LVGL task: the caller is the only writer */
        return ESP_OK;
    }
    bool queued = false;
    if (hy_board_lock(1000)) {
        queued = lv_async_call(job_exec, j) == LV_RESULT_OK;
        hy_board_unlock();
    }
    if (!queued) {
        ESP_LOGW(TAG, "job not queued (LVGL busy)");
        hy_job_put(j);   /* the LVGL side's reference: job_exec will never run */
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(j->sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return ESP_OK;
    }
    int expected = JOB_QUEUED;
    if (atomic_compare_exchange_strong(&j->state, &expected, JOB_ABANDONED)) {
        ESP_LOGW(TAG, "job timed out after %u ms before it started: abandoned", (unsigned)timeout_ms);
    } else if (xSemaphoreTake(j->sem, 0) == pdTRUE) {
        return ESP_OK;   /* finished right at the timeout */
    } else {
        ESP_LOGW(TAG, "job timed out after %u ms while running: its result is dropped", (unsigned)timeout_ms);
    }
    return ESP_ERR_TIMEOUT;
}

#if CONFIG_HY_DEBUG_CONSOLE
void hy_job_debug_slow(uint32_t ms)
{
    atomic_store(&s_slow_ms, ms);
}
#endif
