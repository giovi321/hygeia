#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "lvgl.h"

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

/* Test screen: four 40 px corner squares and a centred label; touching a square turns it white.
 * Caller holds the LVGL lock. */
void hy_board_selftest(void);

#ifdef __cplusplus
}
#endif
