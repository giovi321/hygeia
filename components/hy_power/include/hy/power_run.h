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
/* LVGL-task callbacks registered by the UI; each is called through lv_async_call. */
typedef struct { void (*battery)(uint32_t flags); void (*pwr_short)(void); void (*pwr_long)(void);
                 void (*usb)(bool in); } hy_power_ui_t;
void      hy_power_set_ui(const hy_power_ui_t *ui);
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
void hy_power_debug_fakebatt(const char *args);  /* <pct> [usb] | off */
#endif

#ifdef __cplusplus
}
#endif
