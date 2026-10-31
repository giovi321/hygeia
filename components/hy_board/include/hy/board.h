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

/* I2C bus, display, touch, LVGL port; brightness 50% */
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
bool hy_board_in_lvgl_task(void);   /* the caller is the LVGL adapter's worker task */
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
/* Screen off: brightness 0, Display Off, Sleep In (GRAM and registers kept; not Deep Standby). Screen on: Sleep
   Out, Display On, a full invalidate; the brightness stays 0 until the first frame after the wake has been sent,
   then takes the last hy_board_set_brightness value. Blocks about 120 ms on, 10 ms off. While the panel is off the
   LVGL flushes are dropped (completed at once) and hy_board_set_brightness only records the value. */
void      hy_board_display_power(bool on);
/* Panel rotation, rot 0..3 as hy_rot_t (the UI turned 0, 90, 180, 270 degrees clockwise on the panel); LVGL keeps one
   unrotated 480 x 480 frame. LVGL task, or any task holding hy_board_lock, outside a refresh. In order: brightness
   to 0 with the wake hold, MADCTL from HY_ROT_TABLE[rot] (raw panel_cmd, which waits for queued colour transfers),
   the panel gap and the touch swap_xy / mirror_x / mirror_y from the same row, a full invalidate of the active
   screen, lv_layer_top and lv_layer_sys; the brightness returns after the first full frame has been flushed
   (refr_ready_cb). No-op when rot equals the current one. While the panel is off only the row is stored: Sleep Out
   in hy_board_display_power(true) sends MADCTL and the wake's full invalidate redraws. Logs
   `rot: <deg> madctl 0x.. touch s.. mx.. my..`, then `rot: madctl write to brightness back N ms`. */
void    hy_board_set_rotation(uint8_t rot);
uint8_t hy_board_rotation(void);
#if CONFIG_HY_DEBUG_CONSOLE
/* Bring-up: applies these MADCTL, touch flags and panel gaps as hy_board_set_rotation applies a row (same order,
   same brightness hold, full redraw). Odd gaps are refused (the 2-pixel rounder). The next hy_board_set_rotation
   applies even for the current rot. Same calling context as hy_board_set_rotation. */
void    hy_board_debug_madctl(uint8_t madctl, bool swap, bool mx, bool my, int16_t x_gap, int16_t y_gap);
#endif
/* Called from the touch interrupt (ISR context) on every touch INT edge, beside the LVGL adapter's own
   handling; NULL clears. fn must be ISR safe. */
void      hy_board_set_touch_isr_hook(void (*fn)(void));
/* User key (GPIO 18, input with pull-up, falling edge): fn is called from its ISR; NULL clears. */
void      hy_board_set_user_key_isr_hook(void (*fn)(void));
/* Light-sleep GPIO wakes, low level: touch INT (GPIO 11), RTC INT (GPIO 13), user key (GPIO 18). on: each pin that
   reads high is armed (a pin already low would wake at once: logged, not armed); the RTC INT interrupt is masked
   while armed; touch and key disarm their own pin from their ISR on the first edge (no level-interrupt storm).
   off: every armed pin back to its falling-edge interrupt. Task context. */
void      hy_board_sleep_wake_arm(bool on);
bool      hy_board_rtc_int_low(void);                /* RTC INT (GPIO 13) reads low: an alarm is pending */

/* QMI8658 at 0x6b on the shared bus, 400 kHz. WHO_AM_I (0x00) must read 0x05, else ESP_ERR_NOT_FOUND and every
   later call returns ESP_ERR_INVALID_STATE. Soft reset (0x60 <- 0xB0, wait 20 ms), CTRL1 (0x02) address
   auto-increment, little-endian data; CTRL2 (0x03) accel +-8 g, ODR about 125 Hz; gyro never enabled; left powered
   down (CTRL7 (0x08) = 0, CTRL1 bit 0 sensor disable = 1). Called from hy_board_init after the power gate. */
esp_err_t hy_board_imu_init(void);
/* on: CTRL1 bit 0 = 0, CTRL7 = 0x01 (accel only), first valid sample about 20 ms later. off: CTRL7 = 0, CTRL1 bit 0 = 1 */
esp_err_t hy_board_imu_power(bool on);
/* One burst read of AX_L..AZ_H (0x35..0x3A), converted to milli-g (raw * 1000 / 4096) and mapped to the screen
   frame of hy_accel_t through HY_IMU_MAP (a per-axis source index and sign) and HY_IMU_MOUNT_QUARTER_TURNS (an
   in-plane rotation of x and y), both in qmi8658.c. */
esp_err_t hy_board_imu_read(int16_t mg[3]);
/* The same read before the axis map and the mount rotation (sensor frame), for the bring-up commands */
esp_err_t hy_board_imu_read_raw(int16_t mg[3]);
/* One sample at boot (power up, wait 30 ms, read, power down) for the boot orientation; ESP_FAIL when absent. */
esp_err_t hy_board_imu_sample_once(int16_t mg[3]);
#if CONFIG_HY_DEBUG_CONSOLE
/* CTRL1, CTRL2, CTRL7 as read from the chip */
esp_err_t hy_board_imu_regs(uint8_t out[3]);
#endif

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
