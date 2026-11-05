/* Power task: polls the AXP2101 interrupt flags, keeps the battery model, writes the heartbeat and the
 * battery log, and forwards PWR key, USB and battery changes to the UI (LVGL task, lv_async_call).
 *
 * Sleep mode: when the UI reports the screen OFF (LVGL task), LVGL pauses itself and, without VBUS, automatic
 * light sleep (240 / 40 MHz) is configured with GPIO wakes on touch INT, RTC INT and the user key, and the CPU-max
 * lock is released. With CONFIG_HY_PM_SCREEN_ON_DFS the CPU-max lock is also released while the screen is on without
 * VBUS: the CPU then runs at 240 MHz only while a task runs (FreeRTOS rtos locks) or LVGL refreshes (hy_board's
 * hy_render lock), and at 40 MHz in idle. The task then runs every 1000 ms (PMU poll, heartbeat, battery reading
 * every 60 s) and ends the sleep mode on the first wake cause: PWR key, VBUS change, touch, user key, RTC INT low, now >= next_wake, or a
 * low or critical battery crossing. The UI asks for the sleep mode again after its next tick if the screen stays
 * OFF. */
#include "hy/power_run.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "batlog.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_attr.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "hy/lsstat.h"
#include "hy/wake_gate.h"
#include "lvgl.h"
#include "nvs.h"

#define NS              "power"
#define TASK_PRIO       2
#define TASK_STACK      4096
#define LOOP_MS         200
#define SLEEP_LOOP_MS   1000   /* the loop timeout in the sleep mode: PMU poll */
#define SLEEP_LOOP_WAKERS_MS 5000   /* the same with pwrx wakers on, in light sleep */
#define WG_LOOP_MS      15     /* polling the touch controller while a touch is judged (sleep mode) */
#define WG_REARM_MS     50     /* waiting for the finger to lift before the touch wake is armed again */
#define READ_EVERY_S    10
#define SLEEP_READ_S    60     /* battery reading interval in the sleep mode */
#define CPU_MIN_MHZ     40
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
static uint16_t s_fake_mv;   /* the voltage a fake reading reports; 0 = derive it from the percent */
static hy_batt_crit_confirm_t s_crit_conf;   /* power task only */

/* Heartbeat writes come from the power task, the runner (LVGL task) and the console. s_alive_mu serialises
 * the checks below, the NVS write and the cache update; NVS cannot run inside a portMUX critical section. */
static SemaphoreHandle_t s_alive_mu;
static StaticSemaphore_t s_alive_mu_buf;
static int8_t s_clean_cached = -1; /* last "clean" written, -1 unknown; s_alive_mu */
static bool s_stopping;             /* a power-off is in progress: no write may clear clean; s_alive_mu */
static bool s_alive_on;             /* the runner tracks: the power task's periodic heartbeat runs; s_alive_mu */

/* ---- sleep mode state ---- */
/* W_UI: the LVGL task left the sleep mode itself (the screen left OFF before the pausing pass ended) */
/* W_NET: hy_power_net_hold(true) (the radio started) */
/* W_MOTION: hy_power_motion_wake (a cabinet open reaction) */
typedef enum { W_PWR, W_USB, W_TOUCH, W_KEY, W_RTC, W_NEXT, W_BATT, W_DEBUG, W_UI, W_TIMER, W_NET, W_MOTION,
               W_N } wake_cause_t;
static const char *const k_cause[W_N] = {"pwr", "usb", "touch", "key", "rtc", "next_wake", "batt", "debug",
                                         "ui", "timer-only", "net", "motion"};
static int64_t (*volatile s_next_wake_fn)(void);
static bool (*volatile s_wake_single_fn)(void);
/* A touch woke the CPU with "wake on single tap" off: the power task judges the touches itself (power task only,
 * except that exit_sleep clears the flags). s_wg_rearm: the touch INT wake is still disarmed, re-armed once the finger
 * is up and the pin reads high. */
static hy_wake_gate_t s_wg;
#if CONFIG_HY_DEBUG_CONSOLE
/* pwrx gesture: the held tap and the report counts (power task; RAM, since boot) */
static hy_tap_pair_t s_tp2;
static uint32_t s_gst_reads, s_gst_taps, s_gst_other, s_gst_fail, s_gst_wakes;
#endif
static volatile bool s_wg_active, s_wg_rearm;
static uint8_t s_exit_pending;   /* exit_sleep calls in progress; s_mux */
static int64_t s_wg_rearm_t0_ms;
static SemaphoreHandle_t s_sleep_mu;       /* serialises entering and leaving the sleep mode */
static StaticSemaphore_t s_sleep_mu_buf;
static esp_pm_lock_handle_t s_pm_lock;     /* CPU max: see awake_lock_wanted */
static bool s_lock_held;                   /* s_sleep_mu */
static bool s_light;                       /* light sleep enabled in this sleep mode; s_sleep_mu */
static volatile bool s_asleep;             /* LVGL paused by the sleep mode; written under s_sleep_mu and s_mux */
static bool s_sleep_req;                   /* the UI asked for the sleep mode; s_mux */
static uint8_t s_net_hold;                 /* hy_power_net_hold count: no sleep mode while non-zero; s_mux */
static bool s_vbus_real = true;            /* the last PMU reading saw VBUS (fakebatt does not change it); true until
                                              the first good reading, so light sleep never starts on USB; s_mux */
static volatile bool s_touch_isr, s_key_isr; /* set by the ISRs, taken under s_mux */
static int64_t s_sleep_t0_ms;              /* s_sleep_mu */
/* sleepstat counters, all under s_mux. They live in RTC slow memory without initialisation, so a software, panic,
 * watchdog or brownout reset keeps them and a power-on reset (or a wrong magic word) clears them. Plain loads and
 * stores to that memory are safe from the ISRs: it is not behind the flash cache. */
#define CTR_MAGIC 0x48595346u   /* "HYSF": change it when the struct layout changes */
#define BOOT_HIST 4             /* boots kept with their reset reason and last known uptime */
#define UPTIME_SAVE_S 10        /* the current boot's uptime goes to RTC memory at most this often */
typedef struct {
    uint32_t magic;
    uint32_t boots;                 /* boots since the counters were last cleared, this one included */
    uint32_t entries, wake[W_N], chime_batt;
    uint32_t key_isr, key_isr_asleep, touch_isr, touch_isr_asleep;   /* all, and those that arrived while asleep */
    int64_t ms_asleep, longest_awake_off_ms;
    uint32_t hist_reason[BOOT_HIST];   /* [0] is this boot, [1] the one before, and so on */
    uint32_t hist_up_s[BOOT_HIST];     /* the last uptime seen for that boot, up to UPTIME_SAVE_S short */
} sleep_ctr_t;
static RTC_NOINIT_ATTR sleep_ctr_t s_ctr;
#if CONFIG_HY_DEBUG_CONSOLE
/* lsstat and the battery investigation flags (debug console only). The counters sit in RTC slow memory like s_ctr and
 * follow the same rules (kept over a software, panic, watchdog or brownout reset, cleared by a power-on reset or a
 * wrong magic word). The light-sleep callbacks write them from the idle task with interrupts off, one writer at a time,
 * so they take no lock; a dump may read a half-written 64-bit sum once in a very long while. */
#define LS_MAGIC 0x4C535434u   /* "LST4": change it when the struct layout changes */
typedef struct {
    uint32_t magic;
    uint32_t entered, slept_n;                      /* callback entries, sleeps that ran */
    uint32_t short_n, rej_n;                         /* entries not tried (idle period too short), rejected or woke at once */
    uint64_t slept_us;                               /* time really spent in light sleep */
    uint32_t req_hist[HY_LS_BUCKETS];                /* requested length of each entry */
    uint32_t cause_timer, cause_gpio, cause_other;   /* hardware wake causes of the sleeps that ran */
    uint32_t loops_pwr, loops_net, loops_console;    /* task loops while a light-sleep mode is on */
    hy_scrtime_t st;                                 /* time on battery per screen state */
} ls_ctr_t;
static RTC_NOINIT_ATTR ls_ctr_t s_ls;
static RTC_NOINIT_ATTR hy_mv_ring_t s_ls_mv;         /* the 60 s battery readings; valid with s_ls.magic */
static volatile uint32_t s_px_mask;                  /* pwrx flags, bit n = hy_px_t n; RAM only, 0 at boot */
static bool s_ls_vbus_latch;                         /* this sleep mode runs light sleep on VBUS (pwrx lsvbus); s_sleep_mu */
#endif
static int64_t s_stretch_t0_ms;
static bool s_logged_noreq;                /* the missing request was logged in this OFF stretch */
static bool s_logged_held;                 /* the radio hold was logged in this OFF stretch */
static bool s_stretch_open;                /* awake since a wake that left the screen OFF */

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "unknown";
    }
}

/* Once per boot, before any ISR is armed. */
static void ctr_init(void)
{
    if (s_ctr.magic != CTR_MAGIC || esp_reset_reason() == ESP_RST_POWERON) {
        memset(&s_ctr, 0, sizeof s_ctr);
        s_ctr.magic = CTR_MAGIC;
    }
    s_ctr.boots++;
    for (int i = BOOT_HIST - 1; i > 0; i--) {
        s_ctr.hist_reason[i] = s_ctr.hist_reason[i - 1];
        s_ctr.hist_up_s[i] = s_ctr.hist_up_s[i - 1];
    }
    s_ctr.hist_reason[0] = (uint32_t)esp_reset_reason();
    s_ctr.hist_up_s[0] = 0;
#if CONFIG_HY_DEBUG_CONSOLE
    if (s_ls.magic != LS_MAGIC || esp_reset_reason() == ESP_RST_POWERON) {
        memset(&s_ls, 0, sizeof s_ls);
        hy_mv_ring_clear(&s_ls_mv);
        s_ls.magic = LS_MAGIC;
        hy_scrtime_reset(&s_ls.st);
    }
    s_ls.st.cur = HY_ST_NONE;   /* the stretch that ran when the last boot ended is not counted */
#endif
}

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

/* The power is still on: heartbeats resume, and the clean flag written by stop_begin goes back to false so a crash
 * from here on reads as unclean. */
static void stop_abort(void)
{
    alive_lock();
    s_stopping = false;
    if (s_alive_on && hy_clock_valid()) {
        alive_write_locked((int64_t)time(NULL), false);
    }
    alive_unlock();
}

void hy_power_off(void)
{
    ESP_LOGI(TAG, "power: off by user");
    stop_begin();
    hy_clock_alarm_set(0);
    hy_board_set_brightness(0);
    hy_board_display_power(false);
    hy_board_pmu_power_off();   /* waits for the cut (2.2 s); returning means it did not happen */
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

static void async_wake(void *p)
{
    hy_power_ui_t ui = ui_get();
    if (ui.wake) {
        ui.wake((hy_power_wake_t)(uintptr_t)p);
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

/* ---- sleep mode ---- */

static int64_t mono_ms(void)
{
    return esp_timer_get_time() / 1000;
}

#if CONFIG_HY_DEBUG_CONSOLE
/* Closes the stretch of the current screen state and starts the one the state is in now. Called after every change of
 * the screen state, the sleep mode, the radio hold or VBUS, and by the dumps. Never called with s_mux held. */
static void ls_scr_mark(void)
{
    int64_t now = mono_ms();
    portENTER_CRITICAL(&s_mux);
    int cat = hy_scrtime_category(s_vbus_real, s_scr, s_asleep, s_net_hold > 0);
    hy_scrtime_mark(&s_ls.st, now, cat);
    portEXIT_CRITICAL(&s_mux);
}

static bool px_on(hy_px_t f)
{
    return (s_px_mask & (1u << f)) != 0;
}
#else
#define ls_scr_mark() ((void)0)
#endif

void hy_power_set_next_wake_fn(int64_t (*fn)(void))
{
    s_next_wake_fn = fn;
}

void hy_power_set_wake_single_fn(bool (*fn)(void))
{
    s_wake_single_fn = fn;
}

static bool (*volatile s_double_tap_wake_fn)(void);

static bool double_tap_wake(void)
{
    bool (*fn)(void) = s_double_tap_wake_fn;
    return !fn || fn();
}

/* The function the board calls at each light-sleep entry: cut the touch controller when the setting is off */
static bool touch_cut_now(void)
{
    return !double_tap_wake();
}

void hy_power_set_double_tap_wake_fn(bool (*fn)(void))
{
    s_double_tap_wake_fn = fn;
    hy_board_set_touch_cut_fn(fn ? touch_cut_now : NULL);
}

static bool wake_single(void)
{
    bool (*fn)(void) = s_wake_single_fn;
    return !fn || fn();
}

/* Power task under s_sleep_mu, asleep, a touch woke the CPU and a single tap does not wake the screen: starts judging
 * it. */
static void wg_begin(void)
{
    portENTER_CRITICAL(&s_mux);
    bool ok = s_exit_pending == 0;   /* another task is ending the sleep mode: no judgment starts */
    if (ok) {
        s_wg_active = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!ok) {
        return;
    }
    hy_wake_gate_begin(&s_wg, (uint32_t)mono_ms());
    s_wg_rearm = false;
    ESP_LOGI(TAG, "touch wake: judging (double tap needed)");
}

/* Power task under s_sleep_mu. Without light sleep (USB) the touch wake was never disarmed: nothing to re-arm. */
static void wg_end(const char *why)
{
    s_wg_active = false;
    s_wg_rearm = s_light;   /* the touch INT wake fired and stays disarmed until the finger is up */
    s_wg_rearm_t0_ms = mono_ms();
    hy_wake_gate_reset(&s_wg);
    ESP_LOGI(TAG, "touch wake: %s, back to sleep", why);
}

/* One step of the judgment under s_sleep_mu: LVGL is paused and no other task can end the sleep mode meanwhile, so
 * nothing else reads the controller. */
static hy_wg_act_t wg_step(void)
{
    bool down = false;
    int16_t x = 0, y = 0;
    bool ok = hy_board_touch_poll(&down, &x, &y);
    return hy_wake_gate_sample(&s_wg, (uint32_t)mono_ms(), ok, down, x, y);
}

#if CONFIG_HY_DEBUG_CONSOLE
/* pwrx gesture, power task under s_sleep_mu, asleep: the controller pulsed INT for one gesture report. Reads it, logs
 * the raw bytes, feeds a tap to the tap pair and arms the touch wake again. Byte 4, bits 0x70, is the gesture code
 * (0x10 a tap); bit 0x80 a palm. Position as in a contact report. true: wake the screen. */
static bool gst_step(bool single)
{
    uint8_t r[HY_TOUCH_RAW_LEN] = {0};
    bool ok = hy_board_touch_gesture_read(r);
    bool wake = false;
    if (!ok) {
        s_gst_fail++;
        ESP_LOGW(TAG, "touch gesture: read failed");
    } else {
        s_gst_reads++;
        uint8_t code = r[4] & 0xF0;
        bool tap = code == 0x10;
        int16_t x = (int16_t)((r[1] << 4) | (r[3] >> 4)), y = (int16_t)((r[2] << 4) | (r[3] & 0x0F));
        if (tap) {
            s_gst_taps++;
        } else {
            s_gst_other++;
        }
        hy_wg_act_t act = hy_tap_pair_feed(&s_tp2, single, (uint32_t)mono_ms(), tap, x, y);
        wake = act == HY_WG_WAKE;
        if (wake) {
            s_gst_wakes++;
        }
        ESP_LOGI(TAG, "touch gesture: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x code 0x%02x x %d y %d: %s",
                 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], code, x, y,
                 wake ? "wake" : act == HY_WG_WAIT ? "first tap held" : "not a tap");
    }
    if (!wake) {
        hy_board_touch_wake_rearm();   /* the INT pulse is about 2 ms: the pin is high again by now */
    }
    portENTER_CRITICAL(&s_mux);
    s_touch_isr = false;   /* the edge just handled */
    portEXIT_CRITICAL(&s_mux);
    return wake;
}
#endif

/* The wait of the sleep-mode loop: the touch is judged often, a disarmed touch wake is retried every 50 ms for 3 s
 * and then once a second for as long as it takes (the finger may rest on the glass). */
static uint32_t loop_ms(void)
{
    if (!s_asleep) {
        return LOOP_MS;
    }
    if (s_wg_active) {
        return WG_LOOP_MS;
    }
    if (s_wg_rearm && mono_ms() - s_wg_rearm_t0_ms < 3000) {
        return WG_REARM_MS;
    }
#if CONFIG_HY_DEBUG_CONSOLE
    if (s_light && px_on(HY_PX_WAKERS)) {
        return SLEEP_LOOP_WAKERS_MS;   /* pwrx wakers: the PMU poll waits longer in light sleep */
    }
#endif
    return SLEEP_LOOP_MS;
}

static void pm_set(bool light)
{
    esp_pm_config_t c = {.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, .min_freq_mhz = CPU_MIN_MHZ,
                         .light_sleep_enable = light};
    esp_err_t err = esp_pm_configure(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pm configure (light sleep %d): %s", light, esp_err_to_name(err));
    }
}

static void lock_hold(bool on)
{
    if (!s_pm_lock || s_lock_held == on) {
        return;
    }
    if (on) {
        esp_pm_lock_acquire(s_pm_lock);
    } else {
        esp_pm_lock_release(s_pm_lock);
    }
    s_lock_held = on;
}

/* Whether hy_awake (CPU max) is held. Without CONFIG_HY_PM_SCREEN_ON_DFS: always, except in a light-sleep sleep mode.
 * With it: only on VBUS. On USB the frequency stays fixed as before (power is free there, and the USB console is never
 * exposed to frequency switching); on battery the CPU scales between 40 and 240 MHz with the screen on too. */
static bool awake_lock_wanted(bool asleep, bool vbus)
{
#if CONFIG_HY_PM_SCREEN_ON_DFS
    (void)asleep;
    return vbus;
#else
    return !asleep || vbus;
#endif
}

/* Power task, after a VBUS reading: brings hy_awake in line with VBUS while the sleep mode is off. In the sleep mode
 * enter_sleep and exit_sleep own it, and a VBUS change ends the sleep mode (W_USB). */
static void awake_lock_sync(void)
{
    xSemaphoreTake(s_sleep_mu, portMAX_DELAY);
    if (!s_asleep) {
        portENTER_CRITICAL(&s_mux);
        bool vbus = s_vbus_real;
        portEXIT_CRITICAL(&s_mux);
        lock_hold(awake_lock_wanted(false, vbus));
    }
    xSemaphoreGive(s_sleep_mu);
}

static bool sleep_requested(void)
{
    portENTER_CRITICAL(&s_mux);
    bool r = s_sleep_req;
    portEXIT_CRITICAL(&s_mux);
    return r;
}

/* LVGL task only (hy_power_screen_off). The UI asked for the sleep mode: pause LVGL; without VBUS, light sleep with
 * the GPIO wakes.
 * The pause is requested from the LVGL task itself, never from another task. A pause from another task is a
 * handshake (esp_lv_adapter_pause: set paused, clear pause_done_sem, wait for the worker to give it) that the worker
 * can answer between the flag and the clear: the clear eats the answer, the pause times out and resets the flag,
 * and the worker stays blocked in its paused wait (portMAX_DELAY) with nothing left to notify it (resume returns
 * early when not paused). That froze the UI for good. From the worker the adapter takes the flag at once, and the
 * worker blocks at the top of its loop after the current lv_timer_handler pass: no wait, no timeout. */
static void enter_sleep(void)
{
    if (!hy_board_in_lvgl_task()) {
        ESP_LOGE(TAG, "sleep mode: requested outside the LVGL task, not paused");   /* would reopen the race */
        return;
    }
    xSemaphoreTake(s_sleep_mu, portMAX_DELAY);
    portENTER_CRITICAL(&s_mux);
    bool held = s_net_hold > 0;
    portEXIT_CRITICAL(&s_mux);
    if (held && !s_asleep) {
        /* The radio runs: LVGL keeps running with the screen OFF; the UI asks again on its next tick */
        if (!s_logged_held) {
            s_logged_held = true;
            ESP_LOGI(TAG, "sleep mode: held by the radio");
#if CONFIG_HY_DEBUG_CONSOLE
            if (px_on(HY_PX_LSVBUS)) {
                portENTER_CRITICAL(&s_mux);
                bool usb = s_vbus_real;
                portEXIT_CRITICAL(&s_mux);
                if (usb) {
                    ESP_LOGW(TAG, "pwrx lsvbus is on but the radio holds the sleep mode off on USB: the run measures the awake current, turn Wi-Fi off first (wifi mode off)");
                }
            }
#endif
        }
        xSemaphoreGive(s_sleep_mu);
        return;
    }
    bool asleep_now = s_asleep, requested = sleep_requested();
    if (asleep_now || !requested) {
        /* Asleep already is normal: the UI asks again in the pass that entered the mode. Only a missing request is
           an anomaly. */
        if (!asleep_now && !s_logged_noreq) {
            s_logged_noreq = true;
            ESP_LOGW(TAG, "sleep mode: not entered, no request");
        }
        xSemaphoreGive(s_sleep_mu);
        return;
    }
    esp_err_t err = esp_lv_adapter_pause(0);   /* from the worker: returns at once */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lvgl pause: %s, retry on the next tick", esp_err_to_name(err));
        xSemaphoreGive(s_sleep_mu);
        return;
    }
    int64_t now = mono_ms();
    portENTER_CRITICAL(&s_mux);
    bool vbus = s_vbus_real;
    s_light = !vbus;   /* before s_asleep: the power task reads both without s_sleep_mu */
#if CONFIG_HY_DEBUG_CONSOLE
    s_ls_vbus_latch = vbus && px_on(HY_PX_LSVBUS);   /* pwrx lsvbus: light sleep on USB too, for this sleep mode */
    if (s_ls_vbus_latch) {
        s_light = true;
    }
#endif
    s_asleep = true;
    s_ctr.entries++;
    if (s_stretch_open && now - s_stretch_t0_ms > s_ctr.longest_awake_off_ms) {
        s_ctr.longest_awake_off_ms = now - s_stretch_t0_ms;
    }
    s_stretch_open = false;
    portEXIT_CRITICAL(&s_mux);
    ls_scr_mark();
    s_sleep_t0_ms = now;
    ESP_LOGI(TAG, "power: lvgl paused");
    if (!s_light) {
        ESP_LOGI(TAG, "light sleep: not enabled (vbus)");
    } else {
#if CONFIG_HY_DEBUG_CONSOLE
        if (vbus) {
            ESP_LOGW(TAG, "light sleep: on with vbus (pwrx lsvbus)");
        }
#endif
        ESP_LOGI(TAG, "light sleep: on");
        hy_board_sleep_wake_arm(true);
        pm_set(true);
        lock_hold(false);
    }
    xSemaphoreGive(s_sleep_mu);
    if (s_task) {
        xTaskNotifyGive(s_task);   /* the power task switches to its sleep-mode loop */
    }
}

/* Any task. Ends the sleep mode: CPU max, no light sleep, LVGL running. The request is cleared: the UI asks again if
 * the screen stays OFF. The resume does not wait for the worker, so the LVGL task may call it too: in the pass that
 * paused (the screen left OFF before the pass ended, counted as W_UI), or in the first pass after another task's
 * resume, where it waits for s_sleep_mu and then finds the sleep mode already over.
 * The state and the request are cleared before the resume: a re-request in LVGL's first pass then stands.
 * Work for the LVGL task (lv_async_call) is queued before this call, not after: once LVGL runs, a tick can pause it
 * again, and a call queued after that waits for the next wake. */
static void exit_sleep_locked(wake_cause_t cause)
{
    if (cause == W_DEBUG && hy_board_in_lvgl_task()) {
        cause = W_UI;
    }
    xSemaphoreTake(s_sleep_mu, portMAX_DELAY);
    if (!s_asleep) {
        xSemaphoreGive(s_sleep_mu);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    bool vbus_now = s_vbus_real;
    portEXIT_CRITICAL(&s_mux);
    lock_hold(awake_lock_wanted(false, vbus_now));
    if (s_light) {
        pm_set(false);
        hy_board_sleep_wake_arm(false);
        s_light = false;
    }
    int64_t now = mono_ms();
    int64_t asleep_ms = now - s_sleep_t0_ms;
    portENTER_CRITICAL(&s_mux);
    s_asleep = false;
    s_sleep_req = false;
    s_wg_active = s_wg_rearm = false;
    s_ctr.wake[cause]++;
    s_ctr.ms_asleep += asleep_ms;
    s_stretch_open = true;
    s_stretch_t0_ms = now;
    portEXIT_CRITICAL(&s_mux);
#if CONFIG_HY_DEBUG_CONSOLE
    s_ls_vbus_latch = false;
#endif
    ls_scr_mark();
    esp_lv_adapter_resume();
    xSemaphoreGive(s_sleep_mu);
    ESP_LOGI(TAG, "power: wake (%s) at %lld ms, asleep %lld ms", k_cause[cause], (long long)now,
             (long long)asleep_ms);
}

/* A touch is being judged and another cause ends the sleep mode: the strokes of the double-tap attempt must not reach
 * whatever the wake shows (HY_PWAKE_GUARD swallows every stroke that starts soon after). s_exit_pending is raised before
 * the judgment flag is read, and wg_begin looks at it under the same spinlock: either the judgment began first and
 * is seen here, or it never begins. The guard is queued here, not under s_sleep_mu (the LVGL task may hold the LVGL
 * lock and wait for it), and not from the LVGL task itself. */
static void exit_sleep(wake_cause_t cause)
{
    portENTER_CRITICAL(&s_mux);
    s_exit_pending++;
    bool judging = s_asleep && s_wg_active;
    portEXIT_CRITICAL(&s_mux);
    if (judging && !hy_board_in_lvgl_task()) {
        post(async_wake, (void *)(uintptr_t)HY_PWAKE_GUARD);
    }
    exit_sleep_locked(cause);
    portENTER_CRITICAL(&s_mux);
    s_exit_pending--;
    portEXIT_CRITICAL(&s_mux);
}

void hy_power_screen_off(bool off)
{
    portENTER_CRITICAL(&s_mux);
    s_sleep_req = off;
    if (!off) {
        s_stretch_open = false;   /* the screen came back: not an awake stretch while OFF */
    }
    portEXIT_CRITICAL(&s_mux);
    if (!off) {
        s_logged_noreq = false;   /* a new OFF stretch logs a missing request again */
        s_logged_held = false;
    }
    if (off) {
        enter_sleep();   /* the UI calls this from the LVGL task: LVGL pauses after this lv_timer_handler pass */
        return;
    }
    if (s_asleep) {
        exit_sleep(W_DEBUG);   /* counted as W_UI from the LVGL task (see exit_sleep) */
    }
}

void hy_power_screen_state(uint8_t scr)
{
    s_scr = scr;
    ls_scr_mark();
}

void hy_power_net_hold(bool on)
{
    portENTER_CRITICAL(&s_mux);
    if (on) {
        if (s_net_hold < UINT8_MAX) {
            s_net_hold++;
        }
    } else if (s_net_hold) {
        s_net_hold--;
    }
    uint8_t n = s_net_hold;
    portEXIT_CRITICAL(&s_mux);
    ls_scr_mark();
    ESP_LOGI(TAG, "net hold %s (%u)", on ? "on" : "off", (unsigned)n);
    /* exit_sleep checks s_asleep under s_sleep_mu: a sleep entry that passed its hold check just before the count
     * rose is ended here */
    if (on && s_sleep_mu) {
        exit_sleep(W_NET);
    }
}

void hy_power_motion_wake(void)
{
    if (s_sleep_mu) {
        exit_sleep(W_MOTION);   /* no-op when the sleep mode is not on */
    }
}

void hy_power_chime_on_battery(void)
{
    portENTER_CRITICAL(&s_mux);
    s_ctr.chime_batt++;
    portEXIT_CRITICAL(&s_mux);
}

#if CONFIG_HY_DEBUG_CONSOLE
bool hy_power_px(int flag)
{
    return flag >= 0 && flag < HY_PX_N && px_on((hy_px_t)flag);
}

void hy_power_ls_loop(int who)
{
    if (!s_asleep || !s_light) {
        return;
    }
    if (who == HY_LS_T_NET) {
        s_ls.loops_net++;
    } else if (who == HY_LS_T_CONSOLE) {
        s_ls.loops_console++;
    }
}

void hy_power_resume(void)
{
    if (s_asleep) {
        exit_sleep(W_DEBUG);
    }
}
#endif

static void IRAM_ATTR touch_hook(void)
{
    portENTER_CRITICAL_ISR(&s_mux);
    s_touch_isr = true;
    s_ctr.touch_isr++;
    if (s_asleep) {
        s_ctr.touch_isr_asleep++;
    }
    portEXIT_CRITICAL_ISR(&s_mux);
    if (s_asleep && s_task) {
        BaseType_t hp = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &hp);
        portYIELD_FROM_ISR(hp);
    }
}

static void IRAM_ATTR key_hook(void)
{
    portENTER_CRITICAL_ISR(&s_mux);
    s_key_isr = true;
    s_ctr.key_isr++;
    if (s_asleep) {
        s_ctr.key_isr_asleep++;
    }
    portEXIT_CRITICAL_ISR(&s_mux);
    if (s_task) {
        BaseType_t hp = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &hp);
        portYIELD_FROM_ISR(hp);
    }
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
    if (err == ESP_OK) {
        s_vbus_real = raw.vbus;
    }
    bool fake = s_fake, fake_usb = s_fake_usb;
    int8_t fake_pct = s_fake_pct;
    uint16_t fake_mv = s_fake_mv;
    *refake = s_fake_changed;
    s_fake_changed = false;
    portEXIT_CRITICAL(&s_mux);
    ls_scr_mark();   /* VBUS may have changed */

    if (fake) {
        raw.present = true;
        raw.pct = fake_pct;
        raw.mv = fake_mv ? fake_mv : (uint16_t)(3300 + 9 * fake_pct);   /* 4 % -> 3336 mV, 100 % -> 4200 mV */
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
        s_crit_conf = (hy_batt_crit_confirm_t){0};
        return was_ok ? HY_BATT_CHANGED : 0; /* the top bar switches to the unknown text */
    }

    portENTER_CRITICAL(&s_mux);
    hy_batt_t b = s_st.batt;
    bool was_ok = s_st.pmu_ok;
    portEXIT_CRITICAL(&s_mux);
    if (*refake) {
        /* entering or leaving fakebatt: restart the model so the new value shows at once */
        hy_batt_init(&b, b.rate_cph);
        s_crit_conf = (hy_batt_crit_confirm_t){0};
    }
    uint32_t flags = hy_batt_update(&b, &raw, mono);
    if (hy_batt_crit_confirm(&s_crit_conf, mono, b.pct, raw.mv, raw.vbus)) {
        flags |= HY_BATT_CRIT_CONFIRMED;
    }
    if (!was_ok) {
        flags |= HY_BATT_CHANGED;
    }
    portENTER_CRITICAL(&s_mux);
    s_st.batt = b;
    s_st.pmu_ok = true;
    portEXIT_CRITICAL(&s_mux);
    return flags;
}

/* pwrx lsvbus: this sleep mode runs light sleep with VBUS present on purpose, so that is not a mismatch */
static bool usb_light_ok(void)
{
#if CONFIG_HY_DEBUG_CONSOLE
    return s_ls_vbus_latch && s_vbus_real;
#else
    return false;
#endif
}

static void power_task(void *arg)
{
    (void)arg;
    int64_t last_read = INT64_MIN / 2, last_alive = INT64_MIN / 2, last_log = INT64_MIN / 2, last_up = INT64_MIN / 2;
#if CONFIG_HY_DEBUG_CONSOLE
    int64_t last_mv = INT64_MIN / 2;
#endif
    hy_power_state_t st;
    hy_power_state(&st);
    int32_t saved_rate = st.batt.rate_cph;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(loop_ms()));
        if (s_asleep && s_light) {
#if CONFIG_HY_DEBUG_CONSOLE
            s_ls.loops_pwr++;
#endif
            hy_board_touch_cut_poll();   /* touch cut: the driver may release the reset line by itself */
        }
        int64_t mono = mono_s();
        if (mono - last_up >= UPTIME_SAVE_S) {   /* RTC memory, no flash write */
            last_up = mono;
            portENTER_CRITICAL(&s_mux);
            s_ctr.hist_up_s[0] = (uint32_t)mono;
            portEXIT_CRITICAL(&s_mux);
        }

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
        portENTER_CRITICAL(&s_mux);
        bool touch = s_touch_isr, key = s_key_isr;
        s_touch_isr = s_key_isr = false;
        portEXIT_CRITICAL(&s_mux);
        if (!double_tap_wake()) {
            touch = false;   /* "Double tap to wake" off: while the sleep mode is on a touch is ignored (USB without light sleep) */
        }
        wake_cause_t cause = W_N;
        bool seen_asleep = s_asleep;
        bool double_tap = false;
        const bool single = wake_single();
        if (seen_asleep) {
            int64_t (*nw)(void) = s_next_wake_fn;
            int64_t next = nw ? nw() : 0;
            if (irq & (HY_PMU_PKEY_SHORT | HY_PMU_PKEY_LONG)) {
                cause = W_PWR;
            } else if ((irq & (HY_PMU_VBUS_IN | HY_PMU_VBUS_OUT)) || (s_light == s_vbus_real && !usb_light_ok())) {
                /* light sleep must match the real VBUS (on: no VBUS). A lost VBUS IRQ, or a sleep entered on a VBUS
                 * reading taken before an unplug, ends the sleep mode; the UI asks again with the fresh reading */
                cause = W_USB;
            } else if (touch && single) {
                cause = W_TOUCH;
            } else if (key) {
                cause = W_KEY;
            } else if (hy_board_rtc_int_low()) {
                cause = W_RTC;
            } else if (next && hy_clock_valid() && (int64_t)time(NULL) >= next) {
                cause = W_NEXT;
            }
            bool judging = false;
            if (cause == W_N) {
                /* A touch only wakes the CPU: LVGL stays paused and the screen OFF while it is judged. s_sleep_mu keeps
                 * every other task's exit_sleep (net hold, cabinet reaction) out while the controller is read. */
                xSemaphoreTake(s_sleep_mu, portMAX_DELAY);
#if CONFIG_HY_DEBUG_CONSOLE
                if (s_asleep && touch && hy_board_touch_gesture_active()) {
                    if (gst_step(single)) {
                        cause = W_TOUCH;
                        double_tap = !single;
                    } else {
                        judging = true;   /* not a timer-only wake */
                    }
                } else
#endif
                if (s_asleep) {
                    if (s_wg_active && single) {
                        wg_end("setting changed");
                    } else if (!single && touch && !s_wg_active && !s_wg_rearm) {
                        wg_begin();
                    }
                    if (s_wg_active) {
                        judging = true;
                        hy_wg_act_t act = wg_step();
                        if (act == HY_WG_WAKE) {
                            s_wg_active = false;   /* both taps are over: no guard for the next stroke */
                            cause = W_TOUCH;
                            double_tap = true;
                            judging = false;
                        } else if (act == HY_WG_SLEEP) {
                            wg_end("not a double tap");
                            judging = false;
                        }
                    } else if (s_wg_rearm) {
                        /* the touch wake stays disarmed until the finger is up and the pin reads high; retried for as
                         * long as it takes, never given up */
                        bool down = false;
                        int16_t x, y;
                        bool ok = hy_board_touch_poll(&down, &x, &y);
                        if ((!ok || !down) && hy_board_touch_wake_rearm()) {
                            s_wg_rearm = false;
                            portENTER_CRITICAL(&s_mux);
                            s_touch_isr = false;   /* the edges of the touches just judged */
                            portEXIT_CRITICAL(&s_mux);
                        }
                        judging = true;
                    }
                }
                xSemaphoreGive(s_sleep_mu);
            }
            if (cause == W_RTC) {
                hy_clock_alarm_note_int();   /* the masked ISR missed the edge: the next tick takes and clears AF */
            }
            if (cause == W_N && !judging) {
                portENTER_CRITICAL(&s_mux);
                s_ctr.wake[W_TIMER]++;
                portEXIT_CRITICAL(&s_mux);
            }
        }
        /* The UI work is queued first and the sleep mode ends after it (exit_sleep): LVGL runs it in its first pass */
        if (cause == W_TOUCH) {
            post(async_wake, (void *)(uintptr_t)(double_tap ? HY_PWAKE_DOUBLE_TAP : HY_PWAKE_TOUCH));
        }
        if (key) {
            post(async_wake, (void *)(uintptr_t)HY_PWAKE_KEY);
        }
        if (cause == W_RTC || cause == W_NEXT) {
            post(async_wake, (void *)(uintptr_t)HY_PWAKE_TICK);
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
        if (cause == W_N && !seen_asleep && s_asleep) {
            /* LVGL paused itself while the posts above waited for its lock: they are queued while paused */
            if (irq & (HY_PMU_PKEY_SHORT | HY_PMU_PKEY_LONG)) {
                cause = W_PWR;
            } else if (irq & (HY_PMU_VBUS_IN | HY_PMU_VBUS_OUT)) {
                cause = W_USB;
            } else if (key) {
                cause = W_KEY;
            }
        }
        if (cause != W_N) {
            exit_sleep(cause);
        }

        const uint32_t reread = HY_PMU_VBUS_IN | HY_PMU_VBUS_OUT | HY_PMU_BAT_IN | HY_PMU_BAT_OUT |
                                HY_PMU_CHG_START | HY_PMU_CHG_DONE;
        if ((irq & reread) || force || mono - last_read >= (s_asleep ? SLEEP_READ_S : READ_EVERY_S)) {
            last_read = mono;
            bool refake = false;
            uint32_t flags = read_battery(mono, &refake);
            awake_lock_sync();
            if (refake) {
                flags |= HY_BATT_CHANGED;
            }
            hy_power_state(&st);
#if CONFIG_HY_DEBUG_CONSOLE
            if (st.pmu_ok && mono - last_mv >= SLEEP_READ_S) {   /* pwrx mv: one point a minute, awake or asleep */
                last_mv = mono;
                portENTER_CRITICAL(&s_mux);
                hy_mv_ring_push(&s_ls_mv, (uint32_t)mono, st.batt.raw.mv, st.batt.pct, s_scr);
                portEXIT_CRITICAL(&s_mux);
            }
#endif
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
            if (flags & HY_BATT_CRIT_CONFIRMED) {
                ESP_LOGW(TAG, "battery critical confirmed: %d%%, %u mV", st.batt.pct, (unsigned)st.batt.raw.mv);
            }
            if (flags) {
                post(async_battery, (void *)(uintptr_t)flags);
            }
            if ((flags & (HY_BATT_EV_LOW | HY_BATT_EV_CRIT | HY_BATT_CRIT_CONFIRMED)) && s_asleep) {
                exit_sleep(W_BATT);   /* the UI publishes the event; critical opens the countdown */
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

#if CONFIG_HY_DEBUG_CONSOLE && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
/* Light-sleep callbacks (idle task, interrupts off): count each entry, its requested length, and how it ended. */
static esp_err_t ls_enter_cb(int64_t sleep_time_us, void *arg)
{
    (void)arg;
    s_ls.entered++;
    s_ls.req_hist[hy_ls_bucket(sleep_time_us)]++;
    return ESP_OK;
}

static esp_err_t ls_exit_cb(int64_t slept_us, void *arg)
{
    (void)arg;
    hy_ls_exit_t k = hy_ls_exit_class(slept_us);
    if (k == HY_LS_NOT_TRIED) {
        s_ls.short_n++;   /* the idle period was too short, no sleep was attempted */
        return ESP_OK;
    }
    if (k == HY_LS_REJECTED) {
        s_ls.rej_n++;   /* IDF reports the time spent in the reject path as a sleep: not a sleep that ran */
        return ESP_OK;
    }
    s_ls.slept_n++;
    s_ls.slept_us += (uint64_t)slept_us;
    uint32_t c = esp_sleep_get_wakeup_causes();
    const uint32_t timer = 1u << ESP_SLEEP_WAKEUP_TIMER, gpio = 1u << ESP_SLEEP_WAKEUP_GPIO;
    if (c & timer) {
        s_ls.cause_timer++;
    }
    if (c & gpio) {
        s_ls.cause_gpio++;
    }
    if (c & ~(timer | gpio)) {
        s_ls.cause_other++;
    }
    return ESP_OK;
}

static void ls_register(void)
{
    esp_pm_sleep_cbs_register_config_t cbs = {.enter_cb = ls_enter_cb, .exit_cb = ls_exit_cb};
    esp_err_t err = esp_pm_light_sleep_register_cbs(&cbs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lsstat: light sleep callbacks: %s", esp_err_to_name(err));
    }
}
#endif

esp_err_t hy_power_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    if (!s_alive_mu) {
        s_alive_mu = xSemaphoreCreateMutexStatic(&s_alive_mu_buf);
    }
    s_sleep_mu = xSemaphoreCreateMutexStatic(&s_sleep_mu_buf);
    ctr_init();
#if CONFIG_HY_DEBUG_CONSOLE && CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    ls_register();
#endif
    esp_err_t perr = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "hy_awake", &s_pm_lock);
    if (perr == ESP_OK) {
        lock_hold(true);   /* s_vbus_real is true until the first reading: awake_lock_sync releases it on battery */
        pm_set(false);     /* min 40 MHz: reached only while hy_awake is released */
    } else {
        s_pm_lock = NULL;
        ESP_LOGE(TAG, "pm lock: %s, no light sleep", esp_err_to_name(perr));
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
        return;
    }
    hy_board_set_touch_isr_hook(touch_hook);
    hy_board_set_user_key_isr_hook(key_hook);
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
    if (args && strcmp(args, "rails") == 0) {
        hy_board_pmu_rails();
        return;
    }
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

static void px_print_flags_sleepstat(void);

static uint32_t pct10(uint64_t part, uint64_t whole)   /* tenths of a percent */
{
    return whole ? (uint32_t)(part * 1000 / whole) : 0;
}

/* lsstat: what the light-sleep callbacks, the per-state clock and the task loop counters have collected */
static void ls_print(void)
{
#if !CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    ESP_LOGW(TAG, "lsstat: light-sleep callbacks not built (CONFIG_PM_LIGHT_SLEEP_CALLBACKS is off in sdkconfig): the sleep counts stay 0");
#endif
    ls_scr_mark();
    const volatile ls_ctr_t *vs = &s_ls;
    ls_ctr_t c;
    do {
        c = *vs;   /* a 64-bit sum written while this copy ran is read again */
    } while (c.slept_us != vs->slept_us || c.entered != vs->entered);
    portENTER_CRITICAL(&s_mux);
    c.st = s_ls.st;   /* ls_scr_mark writes it under s_mux */
    portEXIT_CRITICAL(&s_mux);
    uint64_t off_asleep_ms = c.st.ms[HY_ST_OFF_ASLEEP];
    uint32_t res = pct10(c.slept_us / 1000, off_asleep_ms);
    ESP_LOGI(TAG, "lsstat: light-sleep entries %" PRIu32 " (ran %" PRIu32 ", rejected or woke at once %" PRIu32 ", too short to try %" PRIu32
             "), time in light sleep %llu ms, residency %" PRIu32 ".%" PRIu32 "%% of the OFF asleep time (%llu ms)",
             c.entered, c.slept_n, c.rej_n, c.short_n, (unsigned long long)(c.slept_us / 1000), res / 10, res % 10,
             (unsigned long long)off_asleep_ms);
    char h[256];
    int n = snprintf(h, sizeof h, "lsstat requested sleep length:");
    for (int b = 0; b < HY_LS_BUCKETS && n > 0 && n < (int)sizeof h; b++) {
        n += snprintf(h + n, sizeof h - n, "%s %s %" PRIu32, b ? "," : "", hy_ls_bucket_label(b), c.req_hist[b]);
    }
    ESP_LOGI(TAG, "%s", h);
    ESP_LOGI(TAG, "lsstat wake cause of the sleeps that ran: timer %" PRIu32 ", gpio %" PRIu32 ", other %" PRIu32,
             c.cause_timer, c.cause_gpio, c.cause_other);
    ESP_LOGI(TAG, "lsstat task loops while a light-sleep mode is on: power %" PRIu32 ", net %" PRIu32 ", console %" PRIu32,
             c.loops_pwr, c.loops_net, c.loops_console);
    uint64_t total_ms = 0;
    for (int s = 0; s < HY_ST_N; s++) {
        total_ms += c.st.ms[s];
    }
    n = snprintf(h, sizeof h, "lsstat time on battery %llu s:", (unsigned long long)(total_ms / 1000));
    for (int s = 0; s < HY_ST_N && n > 0 && n < (int)sizeof h; s++) {
        uint32_t p10 = pct10(c.st.ms[s], total_ms);
        n += snprintf(h + n, sizeof h - n, "%s %s %llu s (%" PRIu32 ".%" PRIu32 "%%)", s ? "," : "", hy_scrstate_name(s),
                      (unsigned long long)(c.st.ms[s] / 1000), p10 / 10, p10 % 10);
    }
    ESP_LOGI(TAG, "%s", h);
}

static uint32_t px_mask_get(void)
{
    return s_px_mask;
}

static void px_set(hy_px_t f, bool on)
{
    portENTER_CRITICAL(&s_mux);
    if (on) {
        s_px_mask |= 1u << f;
    } else {
        s_px_mask &= ~(1u << f);
    }
    portEXIT_CRITICAL(&s_mux);
    if (f == HY_PX_TOUCH) {
        hy_board_debug_touch_cut(on);
    } else if (f == HY_PX_GESTURE) {
        hy_board_debug_touch_gesture(on);
    } else if (f == HY_PX_PANEL) {
        hy_board_debug_panel_cut(on);
    }
    if (f == HY_PX_WAKERS) {
        wake_task();   /* the power task takes the new wait at once */
    }
}

static void mv_print(void)
{
    uint32_t total, boots;
    portENTER_CRITICAL(&s_mux);
    total = s_ls_mv.total;
    boots = s_ctr.boots;
    portEXIT_CRITICAL(&s_mux);
    uint32_t n = total < HY_MV_RING ? total : HY_MV_RING;
    printf("pwrx mv: %u points kept (%u since the ring was cleared), boot #%u now, t_s restarts at each boot\n",
           (unsigned)n, (unsigned)total, (unsigned)boots);
    printf("tag,t_s,mv,pct,scr\n");
    static const char *const scr[3] = {"ON", "DIM", "OFF"};
    for (uint32_t i = 0; i < n; i++) {
        hy_mv_pt_t pt;
        portENTER_CRITICAL(&s_mux);
        bool ok = hy_mv_ring_get(&s_ls_mv, i, &pt);
        portEXIT_CRITICAL(&s_mux);
        if (ok) {
            printf("mv,%u,%u,%d,%s\n", (unsigned)pt.t_s, (unsigned)pt.mv, (int)pt.pct, scr[pt.scr < 3 ? pt.scr : 0]);
        }
    }
    fflush(stdout);
}

/* The touch driver released the reset line while pwrx touch held it: the sleepstat of that run says so. */
static void px_print_touch_invalid(void)
{
    uint32_t n = hy_board_touch_cut_poll();
    if (n) {
        ESP_LOGW(TAG, "pwrx touch: INVALID, the touch driver released the controller reset %u times since the flag was turned on (scan current back on): repeat the run", (unsigned)n);
    }
}

static void px_print_flags(void)
{
    char names[48];
    hy_px_names(px_mask_get(), names, sizeof names);
    ESP_LOGI(TAG, "pwrx flags in force: %s", names);
    px_print_touch_invalid();
}

/* the first line of every sleepstat: the dump says which flags were on */
static void px_print_flags_sleepstat(void)
{
    char names[48];
    hy_px_names(px_mask_get(), names, sizeof names);
    ESP_LOGI(TAG, "sleepstat flags in force: %s", names);
    px_print_touch_invalid();
}

/* pwrx | pwrx <touch|nonet|panel|wakers|lsvbus|gesture> on|off | pwrx off | pwrx mv [clear] */
void hy_power_debug_pwrx(const char *args)
{
    hy_pxc_t c = hy_px_parse(args);
    switch (c.kind) {
    case HY_PXC_LIST:
        for (int f = 0; f < HY_PX_N; f++) {
            ESP_LOGI(TAG, "pwrx %s: %s", hy_px_name(f), px_on((hy_px_t)f) ? "on" : "off");
        }
        px_print_flags();
        ls_print();
        break;
    case HY_PXC_SET:
        px_set((hy_px_t)c.flag, c.on);
        ESP_LOGI(TAG, "pwrx %s %s (RAM only, off at the next boot; a flag acts from the next sleep or screen off)",
                 hy_px_name(c.flag), c.on ? "on" : "off");
        px_print_flags();
        break;
    case HY_PXC_ALL_OFF:
        for (int f = 0; f < HY_PX_N; f++) {
            px_set((hy_px_t)f, false);
        }
        px_print_flags();
        break;
    case HY_PXC_MV:
        mv_print();
        break;
    case HY_PXC_MV_CLEAR:
        portENTER_CRITICAL(&s_mux);
        hy_mv_ring_clear(&s_ls_mv);
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "pwrx mv: ring cleared");
        break;
    default:
        ESP_LOGE(TAG, "usage: pwrx | pwrx <touch|nonet|panel|wakers|lsvbus|gesture> on|off | pwrx off | pwrx mv [clear]");
        break;
    }
}

void hy_power_sleep_stats(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t boots = s_ctr.boots, entries = s_ctr.entries, chimes = s_ctr.chime_batt, w[W_N];
    memcpy(w, s_ctr.wake, sizeof w);
    int64_t ms = s_ctr.ms_asleep, longest = s_ctr.longest_awake_off_ms;
    uint32_t keys = s_ctr.key_isr, keys_a = s_ctr.key_isr_asleep, touches = s_ctr.touch_isr, touches_a = s_ctr.touch_isr_asleep;
    bool req = s_sleep_req, vbus = s_vbus_real;
    unsigned net_hold = s_net_hold;
    uint32_t h_reason[BOOT_HIST], h_up[BOOT_HIST];
    memcpy(h_reason, s_ctr.hist_reason, sizeof h_reason);
    memcpy(h_up, s_ctr.hist_up_s, sizeof h_up);
    portEXIT_CRITICAL(&s_mux);
    bool asleep = s_asleep;
    if (asleep) {
        ms += mono_ms() - s_sleep_t0_ms;
    }
    px_print_flags_sleepstat();
    ESP_LOGI(TAG, "sleepstat: %s%s, entries %" PRIu32 ", asleep %lld ms total, chime moments on battery %" PRIu32
             ", longest awake stretch while off %lld ms, requested %d, vbus %d, net hold %u",
             asleep ? "asleep" : "awake", asleep ? (s_light ? " (light sleep)" : " (no light sleep)") : "", entries,
             (long long)ms, chimes, (long long)longest, req, vbus, net_hold);
    ESP_LOGI(TAG, "sleepstat wakes: pwr %" PRIu32 ", usb %" PRIu32 ", touch %" PRIu32 ", key %" PRIu32 ", rtc %" PRIu32
             ", next_wake %" PRIu32 ", batt %" PRIu32 ", debug %" PRIu32 ", ui %" PRIu32 ", timer-only %" PRIu32
             ", net %" PRIu32 ", motion %" PRIu32,
             w[W_PWR], w[W_USB], w[W_TOUCH], w[W_KEY], w[W_RTC], w[W_NEXT], w[W_BATT], w[W_DEBUG], w[W_UI],
             w[W_TIMER], w[W_NET], w[W_MOTION]);
    ESP_LOGI(TAG, "sleepstat boot: uptime %lld s, reset reason %s, boots since power-on %" PRIu32 " (counters cover all boots since power-on)",
             (long long)mono_s(), reset_name(esp_reset_reason()), boots);
    /* Newest first. The current boot shows its uptime now; earlier boots show the last uptime saved before the reset. */
    char hist[192];
    int hn = snprintf(hist, sizeof hist, "sleepstat boots:");
    for (uint32_t i = 0; i < BOOT_HIST && i < boots && hn > 0 && hn < (int)sizeof hist; i++) {
        uint32_t up = i == 0 ? (uint32_t)mono_s() : h_up[i];
        hn += snprintf(hist + hn, sizeof hist - hn, "%s #%" PRIu32 " %s (up %" PRIu32 " s%s)", i ? "," : "", boots - i,
                       reset_name((esp_reset_reason_t)h_reason[i]), up, i == 0 ? " now" : "");
    }
    ESP_LOGI(TAG, "%s", hist);
    ESP_LOGI(TAG, "key presses %" PRIu32 " (%" PRIu32 " while asleep)", keys, keys_a);
    ESP_LOGI(TAG, "touch irqs %" PRIu32 " (%" PRIu32 " while asleep)", touches, touches_a);
    ESP_LOGI(TAG, "touch gesture reports since boot: %" PRIu32 " read (%" PRIu32 " taps, %" PRIu32 " other), %" PRIu32
             " failed reads, %" PRIu32 " wakes; gesture mode now %s",
             s_gst_reads, s_gst_taps, s_gst_other, s_gst_fail, s_gst_wakes, hy_board_touch_gesture_active() ? "on" : "off");
    ls_print();
    /* Any lock other than hy_awake held while asleep keeps the chip out of light sleep. With CONFIG_PM_PROFILING (debug
     * build) this also prints the measured time per mode: LIGHT_SLEEP is the light-sleep time since boot, and the
     * light-sleep counts. */
    esp_pm_dump_locks(stdout);
}

void hy_power_debug_fakebatt(const char *args)
{
    bool on, usb = false;
    long pct = 0, mv = 0;
    if (strcmp(args, "off") == 0) {
        on = false;
    } else {
        char *end;
        pct = strtol(args, &end, 10);
        while (*end == ' ') {
            end++;
        }
        bool bad = end == args || pct < 0 || pct > 100;
        if (!bad && *end >= '0' && *end <= '9') {
            const char *num = end;
            mv = strtol(num, &end, 10);
            while (*end == ' ') {
                end++;
            }
            bad = mv < 1000 || mv > 5000;
        }
        if (bad || (*end && strcmp(end, "usb") != 0)) {
            ESP_LOGE(TAG, "usage: fakebatt <pct 0..100> [mv 1000..5000] [usb] | fakebatt off");
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
    s_fake_mv = (uint16_t)mv;
    s_force_read = true;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "fakebatt: %s", on ? "on" : "off");
    wake_task();
}
#endif
