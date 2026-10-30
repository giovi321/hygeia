#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hy/battery.h"
#include "hy/gap.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* batt: battery model; pmu_ok: false after a PMU read error, until the next good read;
 * irq_seen: every HY_PMU_* flag seen since boot (OR) */
typedef struct { hy_batt_t batt; bool pmu_ok; uint32_t irq_seen; } hy_power_state_t;

esp_err_t hy_power_init(void);                   /* after hy_board_init, hy_store_mount, hy_clock_init */
void      hy_power_start(void);                  /* after hy_ui_start: starts the task */
/* Snapshot copy under a spinlock; safe from any task. */
void      hy_power_state(hy_power_state_t *out);
/* Wake causes the power task posts to the UI. TOUCH: a touch woke the CPU from the sleep mode; KEY: the user key
 * (also when awake); TICK: the next_wake time or the RTC alarm, the UI runs its scheduler tick at once. */
typedef enum { HY_PWAKE_TOUCH = 0, HY_PWAKE_KEY, HY_PWAKE_TICK } hy_power_wake_t;
/* LVGL-task callbacks registered by the UI; each is called through lv_async_call. */
typedef struct { void (*battery)(uint32_t flags); void (*pwr_short)(void); void (*pwr_long)(void);
                 void (*usb)(bool in); void (*wake)(hy_power_wake_t cause); } hy_power_ui_t;
void      hy_power_set_ui(const hy_power_ui_t *ui);
/* The scheduler's next_wake (epoch s, 0 = none), read by the power task while asleep. hy_power does not depend on
 * hy_sched: app_main registers hy_sched_run_next_wake here. */
void      hy_power_set_next_wake_fn(int64_t (*fn)(void));

/* Sleep mode. The UI calls hy_power_screen_off(true) from the LVGL task, and only from there, when the screen state
 * is OFF (again after each tick while it stays OFF): LVGL pauses at the end of the current lv_timer_handler pass and,
 * without VBUS, light sleep is enabled with the touch, RTC and user key GPIO wakes. Each wake cause ends the sleep
 * mode and clears the request. hy_power_screen_off(false) when the state leaves OFF: returns with LVGL running. A
 * CPU-max PM lock is held whenever the sleep mode is not on. */
void      hy_power_screen_off(bool off);
void      hy_power_screen_state(uint8_t scr);    /* batlog scr column: 0 on, 1 dim, 2 off */
void      hy_power_chime_on_battery(void);       /* sleepstat: a chime moment while on battery */
/* Heartbeat: NVS namespace "power", key "alive" (i64), key "clean" (u8). */
bool      hy_power_alive_load(hy_alive_t *out);
/* Serialised across tasks; while a power-off is in progress a write with clean = false is skipped. */
void      hy_power_alive_write(int64_t now, bool clean);
/* The power task's 60 s heartbeat runs only while on: the runner turns it on once tracking has started
 * (after its own first write) and off when the clock turns invalid, so a crash before that keeps the
 * previous record for the next boot's gap. Off at boot. */
void      hy_power_alive_enable(bool on);

/* LVGL task. Clean power-off: heartbeat clean = true (valid clock only, no later heartbeat may clear it), RTC alarm
 * off, display off, AXP2101 cut. Does not return when the cut works. If power is still on after 2 s it re-enables
 * heartbeats, turns the display back on, logs an error and returns: the caller shows the failure and restores
 * the brightness. */
void hy_power_off(void);
/* fakebatt is active (the readings are injected): a critical countdown must not really power off */
bool hy_power_fake_active(void);

#if CONFIG_HY_DEBUG_CONSOLE
void hy_power_debug_pmu(const char *args);
void hy_power_debug_batt(const char *args);
void hy_power_debug_batlog(const char *args);    /* "" prints both files, "clear" deletes them */
void hy_power_debug_alive(const char *args);
void hy_power_debug_poweroff(const char *args);  /* heartbeat clean = true, then the AXP2101 power-off */
void hy_power_debug_pkey(const char *args);      /* short | long */
void hy_power_debug_fakebatt(const char *args);  /* <pct> [mv] [usb] | off; mv default 3300 + 9 * pct */
void hy_power_resume(void);                      /* any task: leaves the sleep mode so queued LVGL work runs */
void hy_power_sleep_stats(void);                 /* sleepstat: entries, time asleep, wakes by cause, PM locks */
#endif

#ifdef __cplusplus
}
#endif
