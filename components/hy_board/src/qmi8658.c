/* QMI8658 at 0x6b on the shared bus. Accel only (+-8 g, about 125 Hz), wake on motion, or accel and gyro for a
 * movement capture; left powered down.
 *
 * Register facts, cross-checked against the QMI8658A datasheet (QST, document 13-52-25 Rev A, 2022) and two public
 * references: SensorLib (MIT, lewisxhe/SensorLib, src/SensorQMI8658.hpp) and the Waveshare qmi8658 component
 * (Apache 2.0). No code is copied.
 *   WHO_AM_I 0x00 = 0x05                       both
 *   CTRL1 0x02 bit 6 address auto-increment    both (Waveshare writes 0x60, which includes it)
 *   CTRL1 bit 5 data endianness (1 = big)      SensorLib; this driver keeps it 0 and decodes little endian
 *   CTRL1 bit 4 INT2 output, bit 3 INT1 output (0 = high-Z, 1 = push-pull)   datasheet table 22, SensorLib enableINT
 *   CTRL1 bit 0 sensor disable                 SensorLib; internal oscillator off
 *   CTRL2 0x03 bits 6..4 range (2 = +-8 g)     both
 *   CTRL2 bits 3..0 ODR: 6 = 125 Hz accel only, 112.1 Hz with the gyro; 13 = 21 Hz low power (gyro off only)
 *                                              datasheet table 22, SensorLib AccelODR
 *   CTRL3 0x04 bits 6..4 gyro range (6 = +-1024 dps), bits 3..0 ODR (6 = 112.1 Hz)   datasheet table 22, SensorLib
 *   CTRL5 0x06 bit 4 gyro LPF on, bits 6..5 its mode; bit 0 accel LPF on, bits 2..1 its mode; mode 3 = 13.37 % of
 *                                              ODR    datasheet table 22, SensorLib configAccelerometer/configGyroscope
 *   CTRL7 0x08 bit 0 accel enable, bit 1 gyro enable    both
 *   CTRL8 0x09 bit 7 = 1: CTRL9 handshake on STATUSINT bit 7 instead of INT1   datasheet table 22 and 6.2
 *   CTRL9 0x0A command; done = STATUSINT 0x2D bit 7; host acknowledges with 0x00, the bit then clears
 *                                              datasheet 5.10.1 and 5.10.4, SensorLib writeCommand
 *   WoM setting command 0x08: CAL1_L 0x0B threshold (1 mg, 0 = WoM off), CAL1_H 0x0C bit 7 initial level, bit 6
 *     INT2 (0 = INT1), bits 5..0 blanking samples     datasheet 12.2 table 39, SensorLib configWakeOnMotion
 *   STATUS1 0x2F bit 2 WoM; reading STATUS1 clears it and resets the pin to the initial level  datasheet 12.4
 *   RESET 0x60 <- 0xB0, RST_RESULT 0x4D = 0x80 SensorLib
 *   AX_L..AZ_H 0x35..0x3A, GX_L..GZ_H 0x3B..0x40, burst      both
 *   STATUS0 0x2E bit 0 accel data available   SensorLib (STATUS0_ACCEL_AVAIL; getDataReady masks 0x01 for accel only)
 *   +-8 g scale 4096 LSB/g; +-1024 dps scale 32 LSB/dps      both */
#include "hy/board.h"
#include "hy/motion.h"
#include "board_imu_int.h"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

#define QMI_ADDR          0x6b
#define QMI_REG_WHOAMI    0x00
#define QMI_REG_CTRL1     0x02
#define QMI_REG_CTRL2     0x03
#define QMI_REG_CTRL3     0x04
#define QMI_REG_CTRL5     0x06
#define QMI_REG_CTRL7     0x08
#define QMI_REG_CTRL8     0x09
#define QMI_REG_CTRL9     0x0A
#define QMI_REG_CAL1_L    0x0B
#define QMI_REG_CAL1_H    0x0C
#define QMI_REG_STATUSINT 0x2D
#define QMI_REG_STATUS0   0x2E
#define QMI_STATUS0_ACCEL 0x01
#define QMI_STATUSINT_DONE 0x80
#define QMI_REG_RST_RES   0x4D
#define QMI_REG_RESET     0x60
#define QMI_REG_AX_L      0x35
#define QMI_WHOAMI_ID     0x05
#define QMI_RESET_CMD     0xB0
#define QMI_RST_RES_OK    0x80
#define QMI_CTRL1_AI_LE   0x40 /* address auto-increment, little endian, sensor enabled */
#define QMI_CTRL1_INT1_EN 0x08
#define QMI_CTRL1_INT2_EN 0x10
#define QMI_CTRL1_DISABLE 0x01
#define QMI_CTRL2_8G_125  0x26 /* range +-8 g (2 << 4) | ODR 125 Hz, 112.1 Hz with the gyro (6) */
#define QMI_CTRL2_8G_LP21 0x2D /* range +-8 g | low-power ODR 21 Hz (13): period 47.6 ms, 42 uA typical */
#define QMI_CTRL3_1024_112 0x66 /* range +-1024 dps (6 << 4) | ODR 112.1 Hz (6) */
#define QMI_CTRL5_LPF_15HZ 0x77 /* gyro LPF on, mode 3; accel LPF on, mode 3: 13.37 % of 112.1 Hz = 15 Hz */
#define QMI_CTRL7_ACCEL   0x01
#define QMI_CTRL7_ACC_GYR 0x03
#define QMI_CTRL8_HS_STAT 0x80 /* CTRL9 handshake on STATUSINT bit 7, INT1 untouched */
#define QMI_CMD_ACK       0x00
#define QMI_CMD_WOM       0x08
#define QMI_CAL1H_INT2    0x40
#define QMI_WOM_BLANK     4    /* samples ignored after arming: 190 ms at 21 Hz, past the 3 / ODR filter settling */
#define QMI_WOM_THR_MAX   255  /* CAL1_L is one byte */
#define QMI_LSB_PER_G     4096
#define QMI_LSB_PER_DPS   32
#define QMI_IO_TIMEOUT_MS 50
#define QMI_CMD_TIMEOUT_MS 100
#define QMI_CTRL9_RETRIES 2   /* acknowledge and repeat this often before the chip is reset */
#define QMI_SETTLE_MAX_MS 300
#define QMI_WOM_SETTLE_MS 500

/* Screen frame <- sensor axes: out[i] = sign * raw[src], applied first. The sensor is mounted mirrored: y is negated.
 * The gyro uses the same map (the features need teach and live to agree, not a handedness convention). */
static const struct { uint8_t src; int8_t sign; } HY_IMU_MAP[3] = {
    { 0, 1 }, { 1, -1 }, { 2, 1 },
};

/* Mount of the sensor in the screen plane, in quarter turns (0 to 3), applied to (x, y) after HY_IMU_MAP; z is kept.
 * For the mapped sensor values (x, y) the screen values (sx, sy) are:
 *   0: sx =  x, sy =  y
 *   1: sx = -y, sy =  x
 *   2: sx = -x, sy = -y
 *   3: sx =  y, sy = -x
 * Measured in two poses with the screen frame expected at (0, -1000) top bar up and (-1000, 0) left edge up: raw
 * (0, +940) top bar up and (-985, -104) left edge up, so the mount is mirrored (y negated above) and not turned. */
#define HY_IMU_MOUNT_QUARTER_TURNS 0

typedef enum { M_OFF, M_ACCEL, M_WOM, M_CAPTURE } qmi_mode_t;

static i2c_master_dev_handle_t s_dev;
static esp_err_t s_state = ESP_ERR_INVALID_STATE; /* ESP_OK after a good init, else what every call returns */
static qmi_mode_t s_mode = M_OFF;
/* Interrupt routing for wake on motion: chip pin 1 (INT1) or 2 (INT2) and its GPIO. INT1 on GPIO 17 until the board
 * check settles it; INT2 is expected on GPIO 21. */
static uint8_t s_int_chip = 1;
static int s_int_gpio = 17;
static bool s_ctrl9_reset;   /* the last ctrl9 ended in a chip reset (the chip is configured and powered down) */

static esp_err_t reg_write(uint8_t reg, uint8_t v)
{
    uint8_t buf[2] = { reg, v };
    return i2c_master_transmit(s_dev, buf, sizeof buf, QMI_IO_TIMEOUT_MS);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, QMI_IO_TIMEOUT_MS);
}

/* Oscillator off again after a failed power-on or configuration; errors are ignored, the bus is already faulty */
static void power_down_best_effort(void)
{
    reg_write(QMI_REG_CTRL7, 0x00);
    reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI_LE | QMI_CTRL1_DISABLE);
}

/* STATUSINT bit 7 reaches want within QMI_CMD_TIMEOUT_MS. Fine polls for the first 5 ms, then one per tick. */
static esp_err_t cmd_done_wait(bool want)
{
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        uint8_t st = 0;
        esp_err_t err = reg_read(QMI_REG_STATUSINT, &st, 1);
        if (err == ESP_OK && ((st & QMI_STATUSINT_DONE) != 0) == want) {
            return ESP_OK;
        }
        int64_t us = esp_timer_get_time() - t0;
        if (us >= QMI_CMD_TIMEOUT_MS * 1000) {
            return err != ESP_OK ? err : ESP_ERR_TIMEOUT;
        }
        if (us < 5000) {
            esp_rom_delay_us(200);
        } else {
            vTaskDelay(1);
        }
    }
}

/* The handshake is stuck when CmdDone stays set (the chip waits for the acknowledge) or never rises. The datasheet
 * procedure (5.10.1, 5.10.4) is: command in CTRL9, wait for STATUSINT bit 7, write 0x00 to CTRL9, wait for the bit to
 * clear. A half done handshake is finished by the acknowledge alone. */
static esp_err_t ack_clear(void)
{
    esp_err_t err = reg_write(QMI_REG_CTRL9, QMI_CMD_ACK);
    if (err == ESP_OK) {
        err = cmd_done_wait(false);
    }
    return err;
}

/* One CTRL9 command: a CmdDone left over from an earlier command is acknowledged first (it would pass for this
 * command's answer), then command, wait for CmdDone, acknowledge, wait for CmdDone to clear */
static esp_err_t ctrl9_once(uint8_t cmd)
{
    esp_err_t err = reg_write(QMI_REG_CTRL8, QMI_CTRL8_HS_STAT);
    uint8_t st = 0;
    if (err == ESP_OK && (err = reg_read(QMI_REG_STATUSINT, &st, 1)) == ESP_OK && (st & QMI_STATUSINT_DONE)) {
        ESP_LOGW(TAG, "ctrl9 0x%02x: CmdDone already set (STATUSINT 0x%02x), acknowledging it first", cmd, st);
        err = ack_clear();
    }
    if (err == ESP_OK) {
        err = reg_write(QMI_REG_CTRL9, cmd);
    }
    if (err == ESP_OK) {
        err = cmd_done_wait(true);
    }
    if (err == ESP_OK) {
        err = ack_clear();
    }
    return err;
}

/* Written to CTRL3 before the reset: a reset puts the register back to its default, so reading anything else proves
 * the chip took the reset. CTRL3 is rewritten by every capture start. */
#define QMI_RESET_SENTINEL 0x5A

/* Soft reset and the powered down configuration of init, without the device handle and WHO_AM_I steps. The reset is
 * confirmed before it is recorded: the sentinel in CTRL3 is gone, RST_RESULT reads 0x80 and STATUSINT bit 7 (CmdDone)
 * is clear. The chip may NACK while it resets, so the reset write itself is not checked. s_mode becomes M_OFF only when
 * the reset and every configuration write succeeded, so a failed recovery leaves a recorded WoM exit to be repeated. */
static esp_err_t chip_reset_config(void)
{
    esp_err_t err = reg_write(QMI_REG_CTRL3, QMI_RESET_SENTINEL);
    if (err != ESP_OK) {
        return err;
    }
    reg_write(QMI_REG_RESET, QMI_RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t rr = 0, c3 = QMI_RESET_SENTINEL, st = QMI_STATUSINT_DONE;
    bool ok = false;
    for (int i = 0; i < 5 && !ok; i++) {
        if (reg_read(QMI_REG_RST_RES, &rr, 1) == ESP_OK && reg_read(QMI_REG_CTRL3, &c3, 1) == ESP_OK &&
            reg_read(QMI_REG_STATUSINT, &st, 1) == ESP_OK) {
            ok = rr == QMI_RST_RES_OK && c3 != QMI_RESET_SENTINEL && !(st & QMI_STATUSINT_DONE);
        }
        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "reset not confirmed: RST_RESULT 0x%02x (expected 0x%02x), CTRL3 0x%02x (sentinel 0x%02x), "
                 "STATUSINT 0x%02x (bit 7 must be clear)", rr, QMI_RST_RES_OK, c3, QMI_RESET_SENTINEL, st);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((err = reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI_LE)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL2, QMI_CTRL2_8G_125)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL7, 0x00)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL8, QMI_CTRL8_HS_STAT)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI_LE | QMI_CTRL1_DISABLE)) == ESP_OK) {
        s_mode = M_OFF;
        return ESP_OK;
    }
    return err;
}

/* CTRL9 command with bounded recovery. A timeout is retried QMI_CTRL9_RETRIES times, each after an acknowledge to
 * finish a half done handshake. If the handshake stays stuck the chip is soft reset and configured again (which
 * drops any WoM setting, so a WoM exit is done by it and a WoM arm is not): s_ctrl9_reset is set and ESP_FAIL is
 * returned, and the caller decides whether the command's effect is reached. */
static esp_err_t ctrl9(uint8_t cmd)
{
    s_ctrl9_reset = false;
    esp_err_t err = ctrl9_once(cmd);
    for (int retry = 1; err != ESP_OK && retry <= QMI_CTRL9_RETRIES; retry++) {
        ESP_LOGW(TAG, "ctrl9 0x%02x: %s, retry %d of %d after an acknowledge", cmd, esp_err_to_name(err), retry,
                 QMI_CTRL9_RETRIES);
        ack_clear();   /* the result is judged by the repeat */
        err = ctrl9_once(cmd);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "ctrl9 0x%02x: recovered on retry %d", cmd, retry);
            return ESP_OK;
        }
    }
    if (err == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "ctrl9 0x%02x: %s after %d retries, soft reset and configure again", cmd, esp_err_to_name(err),
             QMI_CTRL9_RETRIES);
    board_imu_int_detach();   /* WoM is gone with the reset */
    esp_err_t rerr = chip_reset_config();
    if (rerr == ESP_OK) {
        s_ctrl9_reset = true;
        ESP_LOGW(TAG, "ctrl9 0x%02x: chip reset and configured, powered down", cmd);
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "ctrl9 0x%02x: reset and configure: %s", cmd, esp_err_to_name(rerr));
    return err;
}

/* Out of wake on motion or the capture mode: sensors off, WoM threshold 0 (pins back to normal), outputs high-Z.
 * The oscillator stays on. */
static esp_err_t mode_exit(void)
{
    esp_err_t err = ESP_OK;
    if (s_mode == M_WOM) {
        board_imu_int_detach();   /* first: the exit must not reach the hook */
        s_ctrl9_reset = false;    /* only a reset inside this exit counts */
        /* Oscillator on before the command: a failed exit leaves CTRL1 with the oscillator off (power_down_best_effort),
           and the CTRL9 engine does not answer then. AI_LE also puts the interrupt outputs high-Z (the pin is detached) */
        if ((err = reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI_LE)) == ESP_OK &&
            (err = reg_write(QMI_REG_CTRL7, 0x00)) == ESP_OK && (err = reg_write(QMI_REG_CAL1_L, 0)) == ESP_OK &&
            (err = reg_write(QMI_REG_CAL1_H, 0)) == ESP_OK && (err = ctrl9(QMI_CMD_WOM)) == ESP_OK) {
            err = reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI_LE);
        }
        if (err != ESP_OK && s_ctrl9_reset) {
            err = ESP_OK;   /* the recovery reset the chip: sensors off, WoM off, powered down; the exit is done */
        }
    } else if (s_mode == M_CAPTURE) {
        err = reg_write(QMI_REG_CTRL7, 0x00);
    }
    if (err == ESP_OK) {
        s_mode = M_OFF;
    }
    return err;
}

esp_err_t hy_board_imu_init(void)
{
    if (s_state == ESP_OK) {
        return ESP_OK;
    }
    if (!s_dev) {
        i2c_device_config_t dcfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = QMI_ADDR,
            .scl_speed_hz = 400000,
        };
        esp_err_t err = i2c_master_bus_add_device(bsp_i2c_get_handle(), &dcfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add device: %s", esp_err_to_name(err));
            s_dev = NULL;
            return err;
        }
    }
    uint8_t id = 0;
    esp_err_t err = reg_read(QMI_REG_WHOAMI, &id, 1);
    if (err != ESP_OK || id != QMI_WHOAMI_ID) {
        ESP_LOGW(TAG, "who_am_i read %s, got 0x%02x, expected 0x%02x", esp_err_to_name(err), id, QMI_WHOAMI_ID);
        s_state = ESP_ERR_INVALID_STATE;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "who_am_i 0x%02x", id);

    if ((err = chip_reset_config()) == ESP_OK) {
        s_state = ESP_OK;
        ESP_LOGI(TAG, "ready: accel +-8 g, 125 Hz, powered down");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "configure: %s", esp_err_to_name(err));
    power_down_best_effort();
    s_state = ESP_ERR_INVALID_STATE;
    return err;
}

esp_err_t hy_board_imu_power(bool on)
{
    if (s_state != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = mode_exit();
    if (on) {
        if (err != ESP_OK || (err = reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI_LE)) != ESP_OK) {
            power_down_best_effort();
            return err;
        }
        if ((err = reg_write(QMI_REG_CTRL2, QMI_CTRL2_8G_125)) != ESP_OK ||
            (err = reg_write(QMI_REG_CTRL5, 0x00)) != ESP_OK ||
            (err = reg_write(QMI_REG_CTRL7, QMI_CTRL7_ACCEL)) != ESP_OK) {
            power_down_best_effort();
            return err;
        }
        s_mode = M_ACCEL;
        return ESP_OK;
    }
    esp_err_t e1 = reg_write(QMI_REG_CTRL7, 0x00);
    esp_err_t e2 = reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI_LE | QMI_CTRL1_DISABLE);
    /* mode_exit left s_mode at M_OFF only when it succeeded; a failed exit stays recorded so the next call retries it */
    return err != ESP_OK ? err : e1 != ESP_OK ? e1 : e2;
}

static void apply_map(const int16_t r[3], int16_t out[3])
{
    int16_t m[3];
    for (int i = 0; i < 3; i++) {
        m[i] = (int16_t)(HY_IMU_MAP[i].sign * r[HY_IMU_MAP[i].src]);
    }
    switch (HY_IMU_MOUNT_QUARTER_TURNS & 3) {
    case 0:
        out[0] = m[0];
        out[1] = m[1];
        break;
    case 1:
        out[0] = (int16_t)-m[1];
        out[1] = m[0];
        break;
    case 2:
        out[0] = (int16_t)-m[0];
        out[1] = (int16_t)-m[1];
        break;
    default:
        out[0] = m[1];
        out[1] = (int16_t)-m[0];
        break;
    }
    out[2] = m[2];
}

static int16_t le16(const uint8_t *b)
{
    return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

esp_err_t hy_board_imu_read_raw(int16_t mg[3])
{
    if (s_state != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[6];
    esp_err_t err = reg_read(QMI_REG_AX_L, b, sizeof b);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 3; i++) {
        mg[i] = (int16_t)((int32_t)le16(&b[2 * i]) * 1000 / QMI_LSB_PER_G);
    }
    return ESP_OK;
}

esp_err_t hy_board_imu_read(int16_t mg[3])
{
    int16_t r[3];
    esp_err_t err = hy_board_imu_read_raw(r);
    if (err != ESP_OK) {
        return err;
    }
    apply_map(r, mg);
    return ESP_OK;
}

esp_err_t hy_board_imu_read6(int16_t a_mg[3], int16_t g_ddps[3])
{
    if (s_state != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[12];
    esp_err_t err = reg_read(QMI_REG_AX_L, b, sizeof b);
    if (err != ESP_OK) {
        return err;
    }
    int16_t a[3], g[3];
    for (int i = 0; i < 3; i++) {
        a[i] = (int16_t)((int32_t)le16(&b[2 * i]) * 1000 / QMI_LSB_PER_G);
        g[i] = (int16_t)((int32_t)le16(&b[6 + 2 * i]) * 10 / QMI_LSB_PER_DPS);
    }
    apply_map(a, a_mg);
    apply_map(g, g_ddps);
    return ESP_OK;
}

esp_err_t hy_board_imu_wait_ready(int timeout_ms)
{
    if (s_state != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The timeout is measured on the clock. The tick is 10 ms here (CONFIG_FREERTOS_HZ 100), so a sleep of
       pdMS_TO_TICKS(5) would be 0 ticks and never wait. One tick between polls is the finest sleep available. */
    int64_t t0 = esp_timer_get_time();
    esp_err_t last;
    for (;;) {
        uint8_t st = 0;
        last = reg_read(QMI_REG_STATUS0, &st, 1);
        if (last == ESP_OK && (st & QMI_STATUS0_ACCEL)) {
            return ESP_OK;
        }
        if (esp_timer_get_time() - t0 >= (int64_t)timeout_ms * 1000) {
            return last != ESP_OK ? last : ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

esp_err_t hy_board_imu_wait_settled(int timeout_ms, int16_t mg[3])
{
    int64_t t0 = esp_timer_get_time();
    int16_t prev[3] = { 0 };
    bool have_prev = false;
    for (;;) {
        int64_t left_ms = timeout_ms - (esp_timer_get_time() - t0) / 1000;
        if (left_ms <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t err = hy_board_imu_wait_ready((int)left_ms);
        int16_t cur[3];
        if (err == ESP_OK) {
            err = hy_board_imu_read(cur);
        }
        if (err != ESP_OK) {
            return err;
        }
        if (have_prev && hy_accel_settled((hy_accel_t){prev[0], prev[1], prev[2]}, (hy_accel_t){cur[0], cur[1], cur[2]})) {
            for (int i = 0; i < 3; i++) {
                mg[i] = cur[i];
            }
            return ESP_OK;
        }
        for (int i = 0; i < 3; i++) {
            prev[i] = cur[i];
        }
        have_prev = true;
    }
}

esp_err_t hy_board_imu_sample_once(int16_t mg[3])
{
    if (s_state != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = hy_board_imu_power(true);
    if (err != ESP_OK) {
        return err;
    }
    err = hy_board_imu_wait_settled(QMI_SETTLE_MAX_MS, mg);
    hy_board_imu_power(false); /* on every path after a successful power-on */
    return err;
}

/* Datasheet figure 25: sensors off, accel ODR and range, threshold and routing, the WoM command, accel on */
static esp_err_t wom_arm_once(uint16_t thr_mg, int16_t rest[3])
{
    if (s_state != ESP_OK || s_int_gpio < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (thr_mg == 0 || thr_mg > QMI_WOM_THR_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = hy_board_imu_power(true);
    if (err != ESP_OK) {
        return err;
    }
    s_ctrl9_reset = false;   /* a reset inside the power up (its WoM exit) is not a reset of this arm */
    int16_t r[3];
    err = hy_board_imu_wait_settled(QMI_WOM_SETTLE_MS, r);
    uint8_t cal1h = (uint8_t)((s_int_chip == 2 ? QMI_CAL1H_INT2 : 0) | QMI_WOM_BLANK);   /* initial level 0 */
    uint8_t ctrl1 = QMI_CTRL1_AI_LE | (s_int_chip == 2 ? QMI_CTRL1_INT2_EN : QMI_CTRL1_INT1_EN);
    if (err == ESP_OK && (err = reg_write(QMI_REG_CTRL7, 0x00)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL2, QMI_CTRL2_8G_LP21)) == ESP_OK) {
        /* From the threshold write on, the chip may hold a WoM setting: every failure below, and every later power
           change, must run the WoM exit, or the accel stream gets no data (datasheet chapter 6) */
        s_mode = M_WOM;
        if ((err = reg_write(QMI_REG_CAL1_L, (uint8_t)thr_mg)) == ESP_OK &&
            (err = reg_write(QMI_REG_CAL1_H, cal1h)) == ESP_OK && (err = ctrl9(QMI_CMD_WOM)) == ESP_OK &&
            (err = reg_write(QMI_REG_CTRL1, ctrl1)) == ESP_OK) {
            err = reg_write(QMI_REG_CTRL7, QMI_CTRL7_ACCEL);
        }
    }
    int level = 0;
    if (err == ESP_OK) {
        err = board_imu_int_attach(s_int_gpio, &level);   /* after the output is enabled: the level is driven */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wom arm: %s", esp_err_to_name(err));
        hy_board_imu_power(false);   /* WoM exit (mode M_WOM), then powered down as the motion task expects */
        return err;
    }
    for (int i = 0; i < 3; i++) {
        rest[i] = r[i];
    }
    ESP_LOGD(TAG, "wom: armed %u mg, INT%u gpio %d level %d", (unsigned)thr_mg, (unsigned)s_int_chip, s_int_gpio,
             level);
    return ESP_OK;
}

/* A CTRL9 timeout that ended in a chip reset dropped the arm (the reset leaves the chip powered down and idle): the arm
 * is repeated once at once, so wake on motion is not lost until the next screen wake */
esp_err_t hy_board_imu_wom_arm(uint16_t thr_mg, int16_t rest[3])
{
    s_ctrl9_reset = false;
    esp_err_t err = wom_arm_once(thr_mg, rest);
    if (err != ESP_OK && s_ctrl9_reset) {
        ESP_LOGW(TAG, "wom arm: the chip was reset by the CTRL9 recovery, arming again");
        s_ctrl9_reset = false;
        err = wom_arm_once(thr_mg, rest);
    }
    return err;
}

esp_err_t hy_board_imu_wom_disarm(void)
{
    return hy_board_imu_power(false);
}

esp_err_t hy_board_imu_capture_on(void)
{
    if (s_state != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = mode_exit();
    if (err == ESP_OK && (err = reg_write(QMI_REG_CTRL7, 0x00)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI_LE)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL2, QMI_CTRL2_8G_125)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL3, QMI_CTRL3_1024_112)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL5, QMI_CTRL5_LPF_15HZ)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL7, QMI_CTRL7_ACC_GYR)) == ESP_OK) {
        s_mode = M_CAPTURE;
        return ESP_OK;
    }
    power_down_best_effort();   /* s_mode stays as mode_exit left it: a failed exit is retried by the next power call */
    return err;
}

#if CONFIG_HY_DEBUG_CONSOLE
esp_err_t hy_board_imu_regs(uint8_t out[HY_IMU_REGS_N])
{
    if (s_state != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    static const uint8_t regs[HY_IMU_REGS_N] = {
        QMI_REG_CTRL1, QMI_REG_CTRL2, QMI_REG_CTRL3, QMI_REG_CTRL5,
        QMI_REG_CTRL7, QMI_REG_CTRL8, QMI_REG_CTRL9, QMI_REG_STATUSINT,
    };
    esp_err_t err = ESP_OK;
    for (int i = 0; i < HY_IMU_REGS_N && err == ESP_OK; i++) {
        err = reg_read(regs[i], &out[i], 1);
    }
    return err;
}

esp_err_t hy_board_imu_reg(uint8_t reg, uint8_t *v)
{
    if (s_state != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    return reg_read(reg, v, 1);
}

esp_err_t hy_board_imu_int_select(uint8_t chip_pin, int gpio)
{
    if ((chip_pin != 1 && chip_pin != 2) || (gpio != 17 && gpio != 21)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mode == M_WOM) {
        return ESP_ERR_INVALID_STATE;
    }
    s_int_chip = chip_pin;
    s_int_gpio = gpio;
    return ESP_OK;
}

void hy_board_imu_int_route(uint8_t *chip_pin, int *gpio)
{
    *chip_pin = s_int_chip;
    *gpio = s_int_gpio;
}
#endif
