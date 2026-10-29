/* Power task: polls the AXP2101 interrupt flags, keeps the battery model, writes the heartbeat and the
 * battery log, and forwards PWR key, USB and battery changes to the UI (LVGL task, lv_async_call). */
#include "hy/power_run.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "batlog.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "lvgl.h"
#include "nvs.h"

#define NS              "power"
#define TASK_PRIO       2
#define TASK_STACK      4096
#define LOOP_MS         200
#define READ_EVERY_S    10
#define ALIVE_EVERY_S   60
#define BATLOG_EVERY_S  600
#define ERR_LOG_EVERY_S 60

static const char *TAG = "power";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static hy_power_state_t s_st;       /* under s_mux */
static hy_power_ui_t s_ui;          /* under s_mux */
static uint32_t s_inject;           /* HY_PMU_* flags injected by `pkey`, under s_mux */
static TaskHandle_t s_task;
static bool s_inited;
static uint8_t s_scr;               /* batlog scr column: 0 on, 1 dim, 2 off; 0 until the screen state is reported */

/* fakebatt: replaces the raw reading; under s_mux */
static bool s_fake, s_fake_usb, s_fake_changed, s_force_read;
static int8_t s_fake_pct;

/* Heartbeat writes come from the power task, the runner (LVGL task) and the console. s_alive_mu serialises
 * the checks below, the NVS write and the cache update; NVS cannot run inside a portMUX critical section. */
static SemaphoreHandle_t s_alive_mu;
static StaticSemaphore_t s_alive_mu_buf;
static int8_t s_clean_cached = -1; /* last "clean" written, -1 unknown; s_alive_mu */
static bool s_stopping;             /* a power-off is in progress: no write may clear clean; s_alive_mu */
static bool s_alive_on;             /* the runner tracks: the power task's periodic heartbeat runs; s_alive_mu */

static int64_t mono_s(void)
{
    return esp_timer_get_time() / 1000000;
}

/* ---- heartbeat and NVS ---- */

bool hy_power_alive_load(hy_alive_t *out)
{
    out->alive = 0;
    out->clean = false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    int64_t a = 0;
    uint8_t c = 0;
    bool ok = nvs_get_i64(h, "alive", &a) == ESP_OK;
    if (nvs_get_u8(h, "clean", &c) == ESP_OK) {
        out->clean = c != 0;
    }
    nvs_close(h);
    out->alive = ok ? a : 0;
    return ok;
}

static void alive_lock(void)
{
    if (!s_alive_mu) {
        s_alive_mu = xSemaphoreCreateMutexStatic(&s_alive_mu_buf);   /* hy_power_init creates it first */
    }
    xSemaphoreTake(s_alive_mu, portMAX_DELAY);
}

static void alive_unlock(void)
{
    xSemaphoreGive(s_alive_mu);
}

/* Under s_alive_mu. */
static void alive_write_locked(int64_t now, bool clean)
{
    if (s_stopping && !clean) {
        return;   /* the power-off has written clean = true: keep it */
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i64(h, "alive", now);
        if (err == ESP_OK && s_clean_cached != (int8_t)clean) {
            err = nvs_set_u8(h, "clean", clean ? 1 : 0);
            if (err == ESP_OK) {
                s_clean_cached = (int8_t)clean;
            }
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "alive write: %s", esp_err_to_name(err));
    }
}

void hy_power_alive_write(int64_t now, bool clean)
{
    alive_lock();
    alive_write_locked(now, clean);
    alive_unlock();
}

void hy_power_alive_enable(bool on)
{
    alive_lock();
    s_alive_on = on;
    alive_unlock();
}

/* The power task's periodic heartbeat: only while the runner tracks and no power-off is in progress. */
static void alive_tick(void)
{
    alive_lock();
    if (s_alive_on && hy_clock_valid()) {
        alive_write_locked((int64_t)time(NULL), false);
    }
    alive_unlock();
}

static int32_t rate_load(void)
{
    nvs_handle_t h;
    int32_t r = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "rate", &r);
        nvs_close(h);
    }
    return r;
}

static void rate_save(int32_t r)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, "rate", r);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "rate saved: %" PRId32 " cph", r);
    } else {
        ESP_LOGE(TAG, "rate save: %s", esp_err_to_name(err));
    }
}

/* A power-off begins: the heartbeat gets clean = true and no later write may clear it. Shared by the user power-off
 * and the debug one. */
static void stop_begin(void)
{
    alive_lock();
    s_stopping = true;
    if (hy_clock_valid()) {
        alive_write_locked((int64_t)time(NULL), true);
    } else {
        ESP_LOGW(TAG, "poweroff: clock invalid, heartbeat not written");
    }
    alive_unlock();
}

/* The power is still on: heartbeats resume. */
static void stop_abort(void)
{
    alive_lock();
    s_stopping = false;
    alive_unlock();
}

void hy_power_off(void)
{
    ESP_LOGI(TAG, "power: off by user");
    stop_begin();
    hy_clock_alarm_set(0);
    hy_board_set_brightness(0);
    hy_board_display_power(false);
    hy_board_pmu_power_off();
    vTaskDelay(pdMS_TO_TICKS(2000));   /* the cut takes effect within this time */
    stop_abort();
    ESP_LOGE(TAG, "power: off failed, still running");
    hy_board_display_power(true);
}

bool hy_power_fake_active(void)
{
    portENTER_CRITICAL(&s_mux);
    bool f = s_fake;
    portEXIT_CRITICAL(&s_mux);
    return f;
}

/* ---- UI forwarding ---- */

static hy_power_ui_t ui_get(void)
{
    portENTER_CRITICAL(&s_mux);
    hy_power_ui_t ui = s_ui;
    portEXIT_CRITICAL(&s_mux);
    return ui;
}

static void async_battery(void *p)
{
    hy_power_ui_t ui = ui_get();
    if (ui.battery) {
        ui.battery((uint32_t)(uintptr_t)p);
    }
}

static void async_pwr_short(void *p)
{
    (void)p;
    hy_power_ui_t ui = ui_get();
    if (ui.pwr_short) {
        ui.pwr_short();
    }
}

static void async_pwr_long(void *p)
{
    (void)p;
    hy_power_ui_t ui = ui_get();
    if (ui.pwr_long) {
        ui.pwr_long();
    }
}

static void async_usb(void *p)
{
    hy_power_ui_t ui = ui_get();
    if (ui.usb) {
        ui.usb(p != NULL);
    }
}

/* lv_async_call is not thread safe: queue it under the LVGL lock */
static void post(void (*fn)(void *), void *arg)
{
    if (!hy_board_display()) {
        return;
    }
    if (!hy_board_lock(1000)) {
        ESP_LOGE(TAG, "post: lvgl lock timeout");
        return;
    }
    if (lv_async_call(fn, arg) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "post: lv_async_call failed");
    }
    hy_board_unlock();
}

void hy_power_set_ui(const hy_power_ui_t *ui)
{
    portENTER_CRITICAL(&s_mux);
    if (ui) {
        s_ui = *ui;
    } else {
        memset(&s_ui, 0, sizeof s_ui);
    }
    portEXIT_CRITICAL(&s_mux);
}

void hy_power_state(hy_power_state_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

/* ---- task ---- */

static void log_irq(uint32_t f)
{
    static const struct {
        uint32_t bit;
        const char *txt;
    } names[] = {
        {HY_PMU_PKEY_SHORT, "pwr short"}, {HY_PMU_PKEY_LONG, "pwr long"},       {HY_PMU_VBUS_IN, "vbus in"},
        {HY_PMU_VBUS_OUT, "vbus out"},    {HY_PMU_BAT_IN, "battery in"},        {HY_PMU_BAT_OUT, "battery out"},
        {HY_PMU_CHG_START, "charge start"}, {HY_PMU_CHG_DONE, "charge done"},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (f & names[i].bit) {
            ESP_LOGI(TAG, "%s", names[i].txt);
        }
    }
}

/* One battery reading into the model. Returns hy_batt_update flags; 0 on a read error. */
static uint32_t read_battery(int64_t mono, bool *refake)
{
    static int64_t last_err_log = -ERR_LOG_EVERY_S;
    hy_batt_raw_t raw;
    esp_err_t err = hy_board_pmu_read(&raw);

    portENTER_CRITICAL(&s_mux);
    bool fake = s_fake, fake_usb = s_fake_usb;
    int8_t fake_pct = s_fake_pct;
    *refake = s_fake_changed;
    s_fake_changed = false;
    portEXIT_CRITICAL(&s_mux);

    if (fake) {
        if (err != ESP_OK) {
            raw.mv = 0;
        }
        raw.present = true;
        raw.pct = fake_pct;
        raw.vbus = fake_usb;
        raw.chg = fake_usb ? (fake_pct >= 100 ? HY_CHG_DONE : HY_CHG_CHARGING) : HY_CHG_NONE;
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        if (mono - last_err_log >= ERR_LOG_EVERY_S) {
            ESP_LOGE(TAG, "pmu read: %s", esp_err_to_name(err));
            last_err_log = mono;
        }
        portENTER_CRITICAL(&s_mux);
        bool was_ok = s_st.pmu_ok;
        s_st.pmu_ok = false;
        portEXIT_CRITICAL(&s_mux);
        return was_ok ? HY_BATT_CHANGED : 0; /* the top bar switches to the unknown text */
    }

    portENTER_CRITICAL(&s_mux);
    hy_batt_t b = s_st.batt;
    bool was_ok = s_st.pmu_ok;
    portEXIT_CRITICAL(&s_mux);
    if (*refake) {
        /* entering or leaving fakebatt: restart the model so the new value shows at once */
        hy_batt_init(&b, b.rate_cph);
    }
    uint32_t flags = hy_batt_update(&b, &raw, mono);
    if (!was_ok) {
        flags |= HY_BATT_CHANGED;
    }
    portENTER_CRITICAL(&s_mux);
    s_st.batt = b;
    s_st.pmu_ok = true;
    portEXIT_CRITICAL(&s_mux);
    return flags;
}

static void power_task(void *arg)
{
    (void)arg;
    int64_t last_read = INT64_MIN / 2, last_alive = INT64_MIN / 2, last_log = INT64_MIN / 2;
    hy_power_state_t st;
    hy_power_state(&st);
    int32_t saved_rate = st.batt.rate_cph;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LOOP_MS));
        int64_t mono = mono_s();

        uint32_t irq = hy_board_pmu_irq();
        portENTER_CRITICAL(&s_mux);
        irq |= s_inject;
        s_inject = 0;
        s_st.irq_seen |= irq;
        bool force = s_force_read;
        s_force_read = false;
        portEXIT_CRITICAL(&s_mux);

        if (irq) {
            log_irq(irq);
        }
        if (irq & HY_PMU_PKEY_SHORT) {
            post(async_pwr_short, NULL);
        }
        if (irq & HY_PMU_PKEY_LONG) {
            post(async_pwr_long, NULL);
        }
        if (irq & HY_PMU_VBUS_IN) {
            post(async_usb, (void *)1);
        }
        if (irq & HY_PMU_VBUS_OUT) {
            post(async_usb, NULL);
        }

        const uint32_t reread = HY_PMU_VBUS_IN | HY_PMU_VBUS_OUT | HY_PMU_BAT_IN | HY_PMU_BAT_OUT |
                                HY_PMU_CHG_START | HY_PMU_CHG_DONE;
        if ((irq & reread) || force || mono - last_read >= READ_EVERY_S) {
            last_read = mono;
            bool refake = false;
            uint32_t flags = read_battery(mono, &refake);
            if (refake) {
                flags |= HY_BATT_CHANGED;
            }
            hy_power_state(&st);
            if (flags & HY_BATT_EV_LOW) {
                ESP_LOGW(TAG, "battery low: %d%%", st.batt.pct);
            }
            if (flags & HY_BATT_EV_CRIT) {
                ESP_LOGW(TAG, "battery critical: %d%%", st.batt.pct);
            }
            if (flags & HY_BATT_CHANGED) {
                ESP_LOGI(TAG, "battery %d%% (raw %d%%, %u mV, vbus %d, chg %d, pmu %s)", st.batt.pct,
                         st.batt.raw.pct, (unsigned)st.batt.raw.mv, st.batt.raw.vbus, (int)st.batt.raw.chg,
                         st.pmu_ok ? "ok" : "error");
            }
            if (flags) {
                post(async_battery, (void *)(uintptr_t)flags);
            }
            if (st.batt.rate_cph != saved_rate) {
                saved_rate = st.batt.rate_cph;
                rate_save(saved_rate);
            }
            if (st.pmu_ok && ((flags & HY_BATT_CHANGED) || mono - last_log >= BATLOG_EVERY_S)) {
                last_log = mono;
                batlog_append(hy_clock_valid() ? (int64_t)time(NULL) : 0, &st.batt.raw, st.batt.pct, s_scr);
            }
        }

        if (mono - last_alive >= ALIVE_EVERY_S) {
            last_alive = mono;
            alive_tick();
        }
    }
}

esp_err_t hy_power_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    if (!s_alive_mu) {
        s_alive_mu = xSemaphoreCreateMutexStatic(&s_alive_mu_buf);
    }
    batlog_init();
    esp_err_t err = hy_board_pmu_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pmu init: %s", esp_err_to_name(err));
    }
    int32_t rate = rate_load();
    portENTER_CRITICAL(&s_mux);
    hy_batt_init(&s_st.batt, rate);
    s_st.pmu_ok = false;
    s_st.irq_seen = 0;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "init: saved rate %" PRId32 " cph", rate);
    s_inited = true;
    return err;
}

void hy_power_start(void)
{
    if (!s_inited || s_task) {
        return;
    }
    if (xTaskCreate(power_task, "power", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_task = NULL;
    }
}

/* ---- debug ---- */

#if CONFIG_HY_DEBUG_CONSOLE
static void wake_task(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void hy_power_debug_pmu(const char *args)
{
    (void)args;
    hy_board_pmu_dump();
}

void hy_power_debug_batt(const char *args)
{
    (void)args;
    hy_power_state_t st;
    hy_power_state(&st);
    const hy_batt_t *b = &st.batt;
    char est[16];
    bool shown = hy_batt_estimate(b, est, sizeof est);
    portENTER_CRITICAL(&s_mux);
    bool fake = s_fake;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "batt: pct %d, raw pct %d, %u mV, present %d, vbus %d, chg %d, pmu %s%s", b->pct, b->raw.pct,
             (unsigned)b->raw.mv, b->raw.present, b->raw.vbus, (int)b->raw.chg, st.pmu_ok ? "ok" : "error",
             fake ? " (fake)" : "");
    ESP_LOGI(TAG, "batt: low armed %d, crit armed %d, anchor %" PRId64 " s at %d%%, rate %" PRId32
             " cph, estimate %s, irq seen 0x%02" PRIx32,
             b->low_armed, b->crit_armed, b->est_t0, b->est_p0, b->rate_cph, shown ? est : "hidden", st.irq_seen);
}

void hy_power_debug_batlog(const char *args)
{
    if (strcmp(args, "clear") == 0) {
        batlog_clear();
    } else if (args[0] == 0) {
        batlog_print();
    } else {
        ESP_LOGE(TAG, "usage: batlog [clear]");
    }
}

void hy_power_debug_alive(const char *args)
{
    (void)args;
    hy_alive_t a;
    bool ok = hy_power_alive_load(&a);
    int64_t now = (int64_t)time(NULL);
    ESP_LOGI(TAG, "alive: %s %" PRId64 " clean %d (now %" PRId64 ", clock %s)", ok ? "record" : "none", a.alive,
             a.clean, now, hy_clock_valid() ? "valid" : "invalid");
}

void hy_power_debug_poweroff(const char *args)
{
    (void)args;
    stop_begin();
    ESP_LOGI(TAG, "power: off (debug)");
    hy_board_pmu_power_off();
    ESP_LOGE(TAG, "poweroff: still running");
    stop_abort();
}

void hy_power_debug_pkey(const char *args)
{
    uint32_t f;
    if (strcmp(args, "short") == 0) {
        f = HY_PMU_PKEY_SHORT;
    } else if (strcmp(args, "long") == 0) {
        f = HY_PMU_PKEY_LONG;
    } else {
        ESP_LOGE(TAG, "usage: pkey short|long");
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_inject |= f;
    portEXIT_CRITICAL(&s_mux);
    wake_task();
}

void hy_power_debug_fakebatt(const char *args)
{
    bool on, usb = false;
    long pct = 0;
    if (strcmp(args, "off") == 0) {
        on = false;
    } else {
        char *end;
        pct = strtol(args, &end, 10);
        while (*end == ' ') {
            end++;
        }
        if (end == args || pct < 0 || pct > 100 || (*end && strcmp(end, "usb") != 0)) {
            ESP_LOGE(TAG, "usage: fakebatt <pct 0..100> [usb] | fakebatt off");
            return;
        }
        on = true;
        usb = *end != 0;
    }
    portENTER_CRITICAL(&s_mux);
    if (s_fake != on) {
        s_fake_changed = true;
    }
    s_fake = on;
    s_fake_usb = usb;
    s_fake_pct = (int8_t)pct;
    s_force_read = true;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "fakebatt: %s", on ? "on" : "off");
    wake_task();
}
#endif
