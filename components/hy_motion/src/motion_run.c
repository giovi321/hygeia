/* The motion task: owns the IMU after hy_board_imu_init. It samples every 20 ms while the screen is on or dimmed,
   feeds the orientation state and the shake detector, and posts to the LVGL task. It never calls LVGL itself except
   lv_async_call under a bounded hy_board_lock. While the screen is off the IMU is powered down and the task blocks
   on its notification with no timeout: no timer, no wake source, no power lock. */
#include "hy/motion_run.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hy/board.h"
#include "lvgl.h"

static const char *TAG = "motion";

#define SAMPLE_MS      20
#define POST_LOCK_MS   1000
#define RETRY_MS       1000
#define FAILS_MAX      3
#define TASK_PRIO      4
#if CONFIG_HY_DEBUG_CONSOLE
#define TASK_STACK     4096   /* printf of the debug sample lines */
#define DBG_ARM_MS     30000
#else
#define TASK_STACK     3072
#endif

enum { EV_ROTATE = 1, EV_SHAKE = 2 };

/* Everything below is shared between the motion task and any other task, guarded by s_mux */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_want;            /* hy_motion_enable */
static bool s_lock;
static bool s_arm;
static uint32_t s_arm_seq;      /* bumped by every hy_motion_shake_arm(true): the detector restarts */
static uint32_t s_shake_gen;   /* bumped at every arm change and every disable: drops a shake queued before it */
static uint32_t s_set_seq;     /* bumped by hy_motion_set_current; the task remembers the last one it handled */
static hy_rot_t s_applied;     /* the rotation the UI last confirmed through hy_motion_set_current (or init) */
static uint32_t s_gen;         /* bumped at every disable: drops callbacks queued before it */
#if CONFIG_HY_DEBUG_CONSOLE
static int64_t s_dbg_arm_until_ms;
static struct { int remaining; bool raw, rec; } s_ireq;
static SemaphoreHandle_t s_idone;
#endif

static volatile bool s_powered;   /* written by the motion task only, read for the debug state */
static volatile hy_rot_t s_orient_cur;
static TaskHandle_t s_task;
static hy_motion_ui_t s_ui;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void wake_task(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* LVGL task */
static void async_cb(void *p)
{
    uintptr_t v = (uintptr_t)p;
    uint32_t gen = (uint32_t)(v >> 8);
    int ev = (int)((v >> 4) & 0xf);
    hy_rot_t r = (hy_rot_t)(v & 0xf);
    portENTER_CRITICAL(&s_mux);
    uint32_t cur = ev == EV_SHAKE ? s_shake_gen : s_gen;
    bool stale = gen != (cur & 0xffffff) || !s_want;
    portEXIT_CRITICAL(&s_mux);
    if (stale) {
        return;
    }
    if (ev == EV_ROTATE && s_ui.rotate) {
        s_ui.rotate(r);
    } else if (ev == EV_SHAKE && s_ui.shake) {
        s_ui.shake();
    }
}

/* Motion task. lv_async_call is not thread safe: queue it under the LVGL lock, drop on failure */
static bool post(int ev, hy_rot_t r, uint32_t shake_gen)
{
    if (!hy_board_display()) {
        return false;
    }
    uint32_t gen;
    if (ev == EV_SHAKE) {
        gen = shake_gen & 0xffffff;   /* the generation read with the arm snapshot, before the sample */
    } else {
        portENTER_CRITICAL(&s_mux);
        gen = s_gen & 0xffffff;
        portEXIT_CRITICAL(&s_mux);
    }
    uintptr_t arg = ((uintptr_t)gen << 8) | ((uintptr_t)ev << 4) | (uintptr_t)r;
    if (!hy_board_lock(POST_LOCK_MS)) {
        ESP_LOGE(TAG, "post: lvgl lock timeout");
        return false;
    }
    bool ok = lv_async_call(async_cb, (void *)arg) == LV_RESULT_OK;
    if (!ok) {
        ESP_LOGE(TAG, "post: lv_async_call failed");
    }
    hy_board_unlock();
    return ok;
}

#if CONFIG_HY_DEBUG_CONSOLE
static void imu_req_end(void)
{
    portENTER_CRITICAL(&s_mux);
    bool was = s_ireq.remaining > 0;
    s_ireq.remaining = 0;
    portEXIT_CRITICAL(&s_mux);
    if (was) {
        xSemaphoreGive(s_idone);
    }
}

/* One requested sample, from the read the task already made */
static void imu_req_sample(uint32_t t_ms, const int16_t mg[3])
{
    portENTER_CRITICAL(&s_mux);
    int left = s_ireq.remaining;
    bool raw = s_ireq.raw, rec = s_ireq.rec;
    portEXIT_CRITICAL(&s_mux);
    if (left <= 0) {
        return;
    }
    int16_t v[3] = {mg[0], mg[1], mg[2]};
    if (raw && hy_board_imu_read_raw(v) != ESP_OK) {
        ESP_LOGE(TAG, "imu raw read failed");
        imu_req_end();
        return;
    }
    if (rec) {
        printf("imu,%lu,%d,%d,%d\n", (unsigned long)t_ms, v[0], v[1], v[2]);
    } else {
        float a = sqrtf((float)v[0] * v[0] + (float)v[1] * v[1] + (float)v[2] * v[2]);
        printf("imu: x=%d y=%d z=%d mg |a|=%d\n", v[0], v[1], v[2], (int)(a + 0.5f));
    }
    portENTER_CRITICAL(&s_mux);
    left = --s_ireq.remaining;
    portEXIT_CRITICAL(&s_mux);
    if (left == 0) {
        xSemaphoreGive(s_idone);
    }
}
#endif

static void motion_task(void *arg)
{
    hy_orient_t o;
    hy_orient_init(&o, (hy_rot_t)(uintptr_t)arg);
    s_orient_cur = hy_orient_current(&o);
    hy_shake_t sh;
    bool shake_on = false, powered = false, fail_logged = false, imu_bad_logged = false;
    int fails = 0;
    uint32_t seen_seq = 0, seen_arm = 0, seen_gen = 0;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        bool want, lock, arm, set_req;
        hy_rot_t applied;
        uint32_t seq, arm_seq, shake_gen, gen;
#if CONFIG_HY_DEBUG_CONSOLE
        bool dbg_only = false;
#endif
        portENTER_CRITICAL(&s_mux);
        want = s_want;
        lock = s_lock;
        arm = s_arm;
        arm_seq = s_arm_seq;
        shake_gen = s_shake_gen;
        gen = s_gen;
        seq = s_set_seq;
        set_req = want && seq != seen_seq;   /* handled below; a request is not lost if the power up fails */
        applied = s_applied;
#if CONFIG_HY_DEBUG_CONSOLE
        if (!arm && s_dbg_arm_until_ms > 0 && now_ms() < s_dbg_arm_until_ms) {
            arm = true;
            dbg_only = true;
        }
#endif
        portEXIT_CRITICAL(&s_mux);

        if (!want) {
            if (powered) {
                esp_err_t derr = ESP_FAIL;
                for (int i = 0; i < 3 && derr != ESP_OK; i++) {
                    if (i > 0) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    derr = hy_board_imu_power(false);
                }
                if (derr != ESP_OK) {
                    ESP_LOGE(TAG, "imu power down failed: %s", esp_err_to_name(derr));
                }
                powered = false;
                s_powered = false;
                ESP_LOGI(TAG, "motion: off, stack free min %u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
#if CONFIG_HY_DEBUG_CONSOLE
            imu_req_end();
#endif
            shake_on = false;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* no timeout: nothing wakes the CPU for the IMU while off */
            continue;
        }

        if (!powered) {
            esp_err_t perr = hy_board_imu_power(true);
            if (perr != ESP_OK) {
                if (!imu_bad_logged) {
                    ESP_LOGE(TAG, "motion: imu power up failed: %s", esp_err_to_name(perr));
                    imu_bad_logged = true;
                }
                /* No IMU at all cannot succeed later: block with no timeout. Transient bus errors retry. */
                ulTaskNotifyTake(pdTRUE, perr == ESP_ERR_INVALID_STATE ? portMAX_DELAY : pdMS_TO_TICKS(RETRY_MS));
                continue;
            }
            imu_bad_logged = false;
            vTaskDelay(pdMS_TO_TICKS(30));   /* first valid sample about 20 ms after power up */
            powered = true;
            s_powered = true;
            hy_orient_resume(&o);
            /* Resync with the panel: a post dropped before the screen went off must not leave the state ahead of it.
               The wake dwell stays unless a forced rotation is waiting. */
            if (hy_orient_current(&o) != applied) {
                hy_orient_set(&o, applied);
                if (set_req) {
                    o.quick = false;
                }
            }
            shake_on = false;
            fails = 0;
            seen_gen = gen;
            ESP_LOGI(TAG, "motion: on");
            last = xTaskGetTickCount();
        } else if (gen != seen_gen) {
            /* Off and on again inside one sample or one lock wait: the state may be ahead of the panel */
            seen_gen = gen;
            hy_orient_resume(&o);
            if (hy_orient_current(&o) != applied) {
                hy_orient_set(&o, applied);
                if (set_req) {
                    o.quick = false;
                }
            }
            shake_on = false;
        }

        if (set_req) {
            /* A forced or locked rotation is a deliberate choice: the 250 ms wake dwell must not undo it.
               hy_orient_set keeps the quick flag, so it is cleared here when the rotation actually changes. */
            if (hy_orient_current(&o) != applied) {
                hy_orient_set(&o, applied);
                o.quick = false;
            }
            s_orient_cur = hy_orient_current(&o);
            seen_seq = seq;
        }

        int16_t mg[3];
        if (hy_board_imu_read(mg) != ESP_OK) {
            if (++fails >= FAILS_MAX) {
                if (!fail_logged) {
                    ESP_LOGE(TAG, "motion: imu read failed");
                    fail_logged = true;
                }
                fails = 0;
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
                last = xTaskGetTickCount();
                continue;
            }
            vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_MS));
            continue;
        }
        fails = 0;
        fail_logged = false;
        uint32_t t = (uint32_t)now_ms();
        hy_accel_t a = {mg[0], mg[1], mg[2]};

        if (!lock && hy_orient_feed(&o, t, a)) {
            hy_rot_t next = hy_orient_current(&o);
            if (!post(EV_ROTATE, next, 0)) {
                hy_orient_set(&o, applied);   /* never ahead of the panel: a held pose switches again after the dwell */
            }
            s_orient_cur = hy_orient_current(&o);
        }
        if (arm && (!shake_on || arm_seq != seen_arm)) {
            hy_shake_reset(&sh, t);   /* a fresh window, also a re-arm that fell inside one sample */
        }
        seen_arm = arm_seq;
        shake_on = arm;
        if (arm && hy_shake_feed(&sh, t, a)) {
#if CONFIG_HY_DEBUG_CONSOLE
            ESP_LOGI(TAG, "shake detected%s", dbg_only ? " (debug arm, no action)" : "");
            if (!dbg_only) {
                post(EV_SHAKE, hy_orient_current(&o), shake_gen);
            }
#else
            post(EV_SHAKE, hy_orient_current(&o), shake_gen);
#endif
        }
#if CONFIG_HY_DEBUG_CONSOLE
        imu_req_sample(t, mg);
#endif
        TickType_t now_t = xTaskGetTickCount();
        if (now_t - last > pdMS_TO_TICKS(SAMPLE_MS)) {
            last = now_t;   /* a long post or print: do not catch up with a burst of reads */
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_MS));
    }
}

esp_err_t hy_motion_init(const hy_motion_ui_t *ui, hy_rot_t cur)
{
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ui) {
        s_ui = *ui;
    }
    s_applied = cur;
    ESP_LOGI(TAG, "heap free: internal %u before hy_motion_init",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#if CONFIG_HY_DEBUG_CONSOLE
    s_idone = xSemaphoreCreateBinary();
    if (!s_idone) {
        return ESP_ERR_NO_MEM;
    }
#endif
    if (xTaskCreate(motion_task, "motion", TASK_STACK, (void *)(uintptr_t)cur, TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
#if CONFIG_HY_DEBUG_CONSOLE
        vSemaphoreDelete(s_idone);
        s_idone = NULL;
#endif
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "heap free: internal %u after hy_motion_init",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

void hy_motion_enable(bool on)
{
    portENTER_CRITICAL(&s_mux);
    bool changed = s_want != on;
    s_want = on;
    if (changed && !on) {
        s_gen++;
        s_shake_gen++;   /* a shake queued before the screen went off is dropped after a re-enable */
    }
    portEXIT_CRITICAL(&s_mux);
    if (changed) {
        wake_task();
    }
}

void hy_motion_set_lock(bool locked)
{
    portENTER_CRITICAL(&s_mux);
    s_lock = locked;
    portEXIT_CRITICAL(&s_mux);
    wake_task();
}

void hy_motion_set_current(hy_rot_t r)
{
    portENTER_CRITICAL(&s_mux);
    s_applied = r;
    s_set_seq++;
    portEXIT_CRITICAL(&s_mux);
    wake_task();
}

void hy_motion_shake_arm(bool on)
{
    portENTER_CRITICAL(&s_mux);
    s_arm = on;
    if (on) {
        s_arm_seq++;
    }
    s_shake_gen++;   /* every arm change, so a shake queued for an earlier toast never hits the next one */
    portEXIT_CRITICAL(&s_mux);
    wake_task();
}

#if CONFIG_HY_DEBUG_CONSOLE
void hy_motion_debug(const char *args)
{
    if (strncmp(args, "arm", 3) == 0) {
        portENTER_CRITICAL(&s_mux);
        s_dbg_arm_until_ms = now_ms() + DBG_ARM_MS;
        portEXIT_CRITICAL(&s_mux);
        wake_task();
        ESP_LOGI(TAG, "shake detector armed for %d s, detections are only logged", DBG_ARM_MS / 1000);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    bool want = s_want, lock = s_lock, arm = s_arm;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "state: enabled %d imu on %d lock %d arm %d orient %u deg, stack free min %u bytes", want,
             s_powered, lock, arm, (unsigned)s_orient_cur * 90, (unsigned)uxTaskGetStackHighWaterMark(s_task));
}

void hy_motion_imu(bool raw, bool rec, int samples)
{
    if (!s_task) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    bool want = s_want;
    bool busy = s_ireq.remaining > 0;
    if (want && !busy) {
        s_ireq.raw = raw;
        s_ireq.rec = rec;
        s_ireq.remaining = samples;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!want) {
        printf("imu: screen off\n");
        return;
    }
    if (busy) {
        printf("imu: busy\n");
        return;
    }
    xSemaphoreTake(s_idone, 0);
    wake_task();
    if (xSemaphoreTake(s_idone, pdMS_TO_TICKS(samples * SAMPLE_MS + 3000)) != pdTRUE) {
        imu_req_end();
        printf("imu: no samples\n");
    }
}
#endif
