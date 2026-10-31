/* QMI8658 accelerometer at 0x6b on the shared bus. Accel only, +-8 g, about 125 Hz, left powered down.
 *
 * Register facts, cross-checked against two public references: SensorLib (MIT, lewisxhe/SensorLib, QMI8658 register
 * and driver sources) and the Waveshare qmi8658 component (Apache 2.0). No code is copied.
 *   WHO_AM_I 0x00 = 0x05                       both
 *   CTRL1 0x02 bit 6 address auto-increment    both (Waveshare writes 0x60, which includes it)
 *   CTRL1 bit 5 data endianness (1 = big)      SensorLib; this driver keeps it 0 and decodes little endian
 *   CTRL1 bit 0 sensor disable                 SensorLib; internal oscillator off
 *   CTRL2 0x03 bits 6..4 range (2 = +-8 g)     both
 *   CTRL2 bits 3..0 ODR (6 = about 125 Hz accel only) both
 *   CTRL7 0x08 bit 0 accel enable              both
 *   RESET 0x60 <- 0xB0, RST_RESULT 0x4D = 0x80 SensorLib
 *   AX_L..AZ_H 0x35..0x3A, 6-byte burst       both
 *   STATUS0 0x2E bit 0 accel data available   SensorLib (STATUS0_ACCEL_AVAIL; getDataReady masks 0x01 for accel only)
 *   +-8 g scale 4096 LSB/g                     both */
#include "hy/board.h"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

#define QMI_ADDR          0x6b
#define QMI_REG_WHOAMI    0x00
#define QMI_REG_CTRL1     0x02
#define QMI_REG_CTRL2     0x03
#define QMI_REG_CTRL7     0x08
#define QMI_REG_STATUS0   0x2E
#define QMI_STATUS0_ACCEL 0x01
#define QMI_REG_RST_RES   0x4D
#define QMI_REG_RESET     0x60
#define QMI_REG_AX_L      0x35
#define QMI_WHOAMI_ID     0x05
#define QMI_RESET_CMD     0xB0
#define QMI_RST_RES_OK    0x80
#define QMI_CTRL1_AI_LE   0x40 /* address auto-increment, little endian, sensor enabled */
#define QMI_CTRL1_DISABLE 0x01
#define QMI_CTRL2_8G_125  0x26 /* range +-8 g (2 << 4) | ODR 125 Hz (6) */
#define QMI_CTRL7_ACCEL   0x01
#define QMI_LSB_PER_G     4096
#define QMI_IO_TIMEOUT_MS 50
#define QMI_READY_POLL_MS 5
#define QMI_READY_MAX_MS  100

/* Screen frame <- sensor axes: out[i] = sign * raw[src], applied first. The sensor is mounted mirrored: y is negated */
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

static i2c_master_dev_handle_t s_dev;
static esp_err_t s_state = ESP_ERR_INVALID_STATE; /* ESP_OK after a good init, else what every call returns */

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

    /* The chip may NACK while it resets, so the write result of the reset is not checked */
    reg_write(QMI_REG_RESET, QMI_RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t rr = 0;
    for (int i = 0; i < 5; i++) {
        if (reg_read(QMI_REG_RST_RES, &rr, 1) == ESP_OK && rr == QMI_RST_RES_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (rr != QMI_RST_RES_OK) {
        ESP_LOGW(TAG, "reset result 0x%02x, expected 0x%02x", rr, QMI_RST_RES_OK);
    }

    if ((err = reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI_LE)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL2, QMI_CTRL2_8G_125)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL7, 0x00)) == ESP_OK &&
        (err = reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI_LE | QMI_CTRL1_DISABLE)) == ESP_OK) {
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
    esp_err_t err;
    if (on) {
        if ((err = reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI_LE)) != ESP_OK) {
            return err;
        }
        if ((err = reg_write(QMI_REG_CTRL7, QMI_CTRL7_ACCEL)) != ESP_OK) {
            power_down_best_effort();
            return err;
        }
        return ESP_OK;
    }
    esp_err_t e1 = reg_write(QMI_REG_CTRL7, 0x00);
    esp_err_t e2 = reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI_LE | QMI_CTRL1_DISABLE);
    return e1 != ESP_OK ? e1 : e2;
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
        int16_t raw = (int16_t)((uint16_t)b[2 * i] | ((uint16_t)b[2 * i + 1] << 8));
        mg[i] = (int16_t)((int32_t)raw * 1000 / QMI_LSB_PER_G);
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
    int16_t m[3];
    for (int i = 0; i < 3; i++) {
        m[i] = (int16_t)(HY_IMU_MAP[i].sign * r[HY_IMU_MAP[i].src]);
    }
    switch (HY_IMU_MOUNT_QUARTER_TURNS & 3) {
    case 0:
        mg[0] = m[0];
        mg[1] = m[1];
        break;
    case 1:
        mg[0] = (int16_t)-m[1];
        mg[1] = m[0];
        break;
    case 2:
        mg[0] = (int16_t)-m[0];
        mg[1] = (int16_t)-m[1];
        break;
    default:
        mg[0] = m[1];
        mg[1] = (int16_t)-m[0];
        break;
    }
    mg[2] = m[2];
    return ESP_OK;
}

esp_err_t hy_board_imu_wait_ready(int timeout_ms)
{
    if (s_state != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t last = ESP_OK;
    for (int waited = 0;; waited += QMI_READY_POLL_MS) {
        uint8_t st = 0;
        last = reg_read(QMI_REG_STATUS0, &st, 1);
        if (last == ESP_OK && (st & QMI_STATUS0_ACCEL)) {
            return ESP_OK;
        }
        if (waited >= timeout_ms) {
            return last != ESP_OK ? last : ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(QMI_READY_POLL_MS));
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
    /* The first conversion after the oscillator starts may not be settled: wait for data, drop that sample, wait for
       the next one and use it */
    int16_t first[3];
    err = hy_board_imu_wait_ready(QMI_READY_MAX_MS);
    if (err == ESP_OK) {
        err = hy_board_imu_read(first);
    }
    if (err == ESP_OK) {
        err = hy_board_imu_wait_ready(QMI_READY_MAX_MS);
    }
    if (err == ESP_OK) {
        err = hy_board_imu_read(mg);
    }
    hy_board_imu_power(false); /* on every path after a successful power-on */
    return err;
}

#if CONFIG_HY_DEBUG_CONSOLE
esp_err_t hy_board_imu_regs(uint8_t out[3])
{
    if (s_state != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = reg_read(QMI_REG_CTRL1, &out[0], 1);
    if (err == ESP_OK) {
        err = reg_read(QMI_REG_CTRL2, &out[1], 1);
    }
    if (err == ESP_OK) {
        err = reg_read(QMI_REG_CTRL7, &out[2], 1);
    }
    return err;
}
#endif
