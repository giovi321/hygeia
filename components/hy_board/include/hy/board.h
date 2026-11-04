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
#if CONFIG_HY_DEBUG_CONSOLE
/* `touch trace`: esp_timer time of the first touch INT since the previous call (0 when none), then cleared. The
   input layer calls it at every read that sees no finger, so a stale release edge never counts as a press. */
int64_t   hy_board_touch_isr_take(void);
#endif
/* User key (GPIO 18, input with pull-up, falling edge): fn is called from its ISR; NULL clears. */
void      hy_board_set_user_key_isr_hook(void (*fn)(void));
/* Light-sleep GPIO wakes, low level: touch INT (GPIO 11), RTC INT (GPIO 13), user key (GPIO 18). on: each pin that
   reads high is armed (a pin already low would wake at once: logged, not armed); the RTC INT interrupt is masked
   while armed; touch and key disarm their own pin from their ISR on the first edge (no level-interrupt storm).
   While hy_board_imu_int_armed(), the IMU interrupt pin is armed too, on the level opposite the one recorded by
   hy_board_imu_wom_arm (also when wake on motion is armed after this call, until off); its ISR disarms it the same
   way. off: every armed pin back to its edge interrupt. Task context. */
void      hy_board_sleep_wake_arm(bool on);
/* Power task, only while LVGL is paused by the sleep mode (the adapter's worker is blocked, nothing else reads the
   touch controller): reads it once. false when the read failed. *down: a finger is on the glass, then *x and *y are
   its position. */
bool      hy_board_touch_poll(bool *down, int16_t *x, int16_t *y);
/* Power task, in the sleep mode: arms the touch INT level wake again after it fired and the CPU decided to stay in
   the sleep mode. Only when the INT pin reads high (a low pin would wake at once). true: armed (also when it
   already was). false outside the light-sleep arming, or while the pin reads low. */
bool      hy_board_touch_wake_rearm(void);
bool      hy_board_rtc_int_low(void);                /* RTC INT (GPIO 13) reads low: an alarm is pending */

/* QMI8658 at 0x6b on the shared bus, 400 kHz. WHO_AM_I (0x00) must read 0x05, else ESP_ERR_NOT_FOUND and every
   later call returns ESP_ERR_INVALID_STATE. Soft reset (0x60 <- 0xB0, wait 20 ms), CTRL1 (0x02) address
   auto-increment, little-endian data; CTRL2 (0x03) accel +-8 g, ODR about 125 Hz; gyro never enabled; left powered
   down (CTRL7 (0x08) = 0, CTRL1 bit 0 sensor disable = 1). Called from hy_board_init after the power gate. */
esp_err_t hy_board_imu_init(void);
/* on: leaves wake on motion or the capture mode if active, CTRL1 bit 0 = 0, CTRL2 = +-8 g at about 125 Hz, CTRL5 = 0
   (filters off), CTRL7 = 0x01 (accel only), first valid sample about 20 ms later. off: the same exit, then CTRL7 = 0,
   CTRL1 bit 0 = 1 */
esp_err_t hy_board_imu_power(bool on);
/* One burst read of AX_L..AZ_H (0x35..0x3A), converted to milli-g (raw * 1000 / 4096) and mapped to the screen
   frame of hy_accel_t through HY_IMU_MAP (a per-axis source index and sign) and HY_IMU_MOUNT_QUARTER_TURNS (an
   in-plane rotation of x and y), both in qmi8658.c. */
esp_err_t hy_board_imu_read(int16_t mg[3]);
/* The same read before the axis map and the mount rotation (sensor frame), for the bring-up commands */
esp_err_t hy_board_imu_read_raw(int16_t mg[3]);
/* After hy_board_imu_power(true): polls STATUS0 (0x2E) bit 0 (accel data available) once per tick (10 ms) until it is set.
   ESP_OK when set, ESP_ERR_TIMEOUT after timeout_ms, the bus error when the last read failed. */
esp_err_t hy_board_imu_wait_ready(int timeout_ms);
/* After hy_board_imu_power(true): reads samples as they become ready until two consecutive ones pass
   hy_accel_settled (about 1 g and steady), and returns the second in mg. The first samples after power up are not
   usable. ESP_ERR_TIMEOUT when that did not happen within timeout_ms (also when the device is moving), the bus error
   when a read failed. mg is set only on ESP_OK. */
esp_err_t hy_board_imu_wait_settled(int timeout_ms, int16_t mg[3]);
/* One sample at boot for the boot orientation: power up, wait for a settled sample, power down. mg is set only on
   ESP_OK. Else the error: ESP_ERR_INVALID_STATE when the IMU is absent, ESP_ERR_TIMEOUT when no settled sample
   arrived within 300 ms. */
esp_err_t hy_board_imu_sample_once(int16_t mg[3]);
/* Wake on motion. Accel low-power mode at the lowest ODR with a period of at most 50 ms (21 Hz, CTRL2 0x2D), gyro
   off, WoM at thr_mg (1 to 255, a 1-byte threshold on the change between successive samples on any axis; see
   hy_cab_wom_hit) routed to the interrupt pin found at bring-up, initial level 0, 4 samples blanked; *rest = a
   settled reading taken first (hy_board_imu_wait_settled, accel at 125 Hz). Records the pin level after arming.
   The pin changes level on every WoM event and stays there; re-arming resets it. ESP_ERR_INVALID_STATE when the
   IMU or the pin is unavailable, ESP_ERR_INVALID_ARG for thr_mg 0 or above 255 (both leave the IMU as it was). On
   any other error the IMU is left out of wake on motion and powered down. */
esp_err_t hy_board_imu_wom_arm(uint16_t thr_mg, int16_t rest[3]);
esp_err_t hy_board_imu_wom_disarm(void);          /* WoM off, sensor powered down (CTRL7 0, CTRL1 disable) */
/* Accel +-8 g and gyro +-1024 dps at 112.1 Hz with the low-pass filters on (13.37 % of ODR, 15 Hz); the first gyro
   value is valid about HY_CAB_GYRO_START_MS later (measured on the board; the datasheet's gyro turn-on time is
   150 ms + 3 / ODR). Leaves wake on motion first. Ended by hy_board_imu_power(false). */
esp_err_t hy_board_imu_capture_on(void);
/* One burst read of accel and gyro (0x35..0x40), both through HY_IMU_MAP and the mount turn; gyro in deci-dps. */
esp_err_t hy_board_imu_read6(int16_t a_mg[3], int16_t g_ddps[3]);
/* ISR-context hook for the interrupt pin, called on every level change of the pin while wake on motion is armed;
   NULL removes it. fn must be ISR safe. While armed, the pin also joins the light-sleep wake set in
   hy_board_sleep_wake_arm, on the level opposite the recorded one. Returns the hook it replaces, so a temporary
   user (the console) can put it back. */
typedef void (*hy_imu_int_fn_t)(void);
hy_imu_int_fn_t hy_board_imu_int_hook(hy_imu_int_fn_t fn);
bool      hy_board_imu_int_armed(void);

#if CONFIG_HY_DEBUG_CONSOLE
#define HY_IMU_REGS_N 8
/* CTRL1, CTRL2, CTRL3, CTRL5, CTRL7, CTRL8, CTRL9, STATUSINT as read from the chip */
esp_err_t hy_board_imu_regs(uint8_t out[HY_IMU_REGS_N]);
/* One register read, for the bring-up commands. Reading STATUS1 (0x2F) clears the WoM flag and resets the pin. */
esp_err_t hy_board_imu_reg(uint8_t reg, uint8_t *v);
/* Interrupt routing for the next hy_board_imu_wom_arm: chip_pin 1 (INT1) or 2 (INT2) and the ESP32-S3 GPIO it is
   wired to. Default INT1 on GPIO 17 until the board check settles it. ESP_ERR_INVALID_ARG for another chip pin or
   a GPIO that is not 17 or 21, ESP_ERR_INVALID_STATE while armed. */
esp_err_t hy_board_imu_int_select(uint8_t chip_pin, int gpio);
void      hy_board_imu_int_route(uint8_t *chip_pin, int *gpio);
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
