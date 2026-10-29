#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "hy/power.h"
#include "lvgl.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* I2C bus, display, touch, LVGL port; brightness 80% */
esp_err_t hy_board_init(void);

lv_display_t *hy_board_display(void);

/* The touch input device (long-press time HY_LONG_PRESS_MS); NULL before a successful init */
lv_indev_t *hy_board_indev(void);

/* Shared I2C master bus (touch, RTC, PMU) */
i2c_master_bus_handle_t hy_board_i2c(void);

/* Log the power-on status and reset reason captured at boot, plus the live status register */
void hy_board_power_report(void);

void hy_board_set_brightness(uint8_t pct);

/* Take the LVGL lock. timeout_ms 0 waits forever. Returns true when taken */
bool hy_board_lock(uint32_t timeout_ms);
void hy_board_unlock(void);

/* PMU setup after the power gate: IRQ enable = hy_axp_irq_enable_mask, IRQ status cleared, key timing
   = hy_axp_key_timing, battery voltage measure on and, with CONFIG_HY_PMU_CHARGER_SETUP,
   hy_board_pmu_charger_setup(). Every register it touches is logged as `pmu: reg 0x27 0x.. -> 0x..`
   (or `unchanged`). ESP_ERR_INVALID_STATE when the boot gate found no AXP2101. */
esp_err_t hy_board_pmu_init(void);
/* TS-pin measure off (as XPowersLib disableTSPinMeasure), charge target 4.2 V, constant charge
   current 150 mA. Read-modify-write, each register logged. The charger enable bit is not touched. */
esp_err_t hy_board_pmu_charger_setup(void);
/* Reads INTSTS1..3, writes the same bytes back to clear them, returns hy_axp_decode_irq. 0 on error. */
uint32_t  hy_board_pmu_irq(void);
esp_err_t hy_board_pmu_read(hy_batt_raw_t *out);    /* STATUS1, STATUS2, 0xA4, 0x34/0x35 */
void      hy_board_pmu_power_off(void);              /* the boot gate's AXP2101 power-off, public */
/* Panel sleep in/out and display on/off; brightness unchanged. The caller keeps LVGL from flushing
   while the panel is off. */
void      hy_board_display_power(bool on);
/* Called from the touch interrupt (ISR context) on every touch INT edge, beside the LVGL adapter's own
   handling; NULL clears. fn must be ISR safe. */
void      hy_board_set_touch_isr_hook(void (*fn)(void));

#if CONFIG_HY_DEBUG_CONSOLE
/* Log the raw PMU registers: STATUS1/2, 0xA4, battery mV, INTEN1..3, 0x22, 0x27, 0x30, 0x50, 0x61..0x64 */
void hy_board_pmu_dump(void);
#endif

/* Test screen: four 40 px corner squares and a centred label; touching a square turns it white.
 * Caller holds the LVGL lock. */
void hy_board_selftest(void);

#ifdef __cplusplus
}
#endif
