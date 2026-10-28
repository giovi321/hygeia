#include "pcf85063.h"

#include <string.h>

#include "hy/rtc_codec.h"

/*
 * Register layout from the SensorLib PCF85063 driver (lewisxhe/SensorLib, MIT,
 * src/time/pcf85063/SensorPCF85063.hpp at commit dff98e79753dedd7ba709d1e92d71b4353c55005):
 * address 0x51; CTRL1 0x00 (bit 5 STOP, bit 1 12/24 h select, 1 = 12 h); time registers 0x04..0x0A
 * in BCD: seconds 0x04 (mask 0x7F, bit 7 = OS oscillator-stop flag), minutes 0x05 (0x7F),
 * hours 0x06 (24 h mask 0x3F), day 0x07 (0x3F), weekday 0x08 (0x07), month 0x09 (0x1F),
 * year 0x0A (00..99, no century bit, read as 2000 + yy).
 */
#define PCF_ADDR       0x51
#define REG_CTRL1      0x00
#define REG_SEC        0x04
#define CTRL1_12H      (1u << 1)
#define CTRL1_STOP     (1u << 5)
#define SEC_OS         0x80
#define I2C_TIMEOUT_MS 100

static i2c_master_dev_handle_t s_dev;

esp_err_t pcf85063_init(i2c_master_bus_handle_t bus)
{
    if (s_dev) {
        return ESP_OK;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF_ADDR,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, I2C_TIMEOUT_MS);
}

esp_err_t pcf85063_read(pcf85063_time_t *out)
{
    memset(out, 0, sizeof *out);
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = read_regs(REG_CTRL1, &out->ctrl1, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = read_regs(REG_SEC, out->raw, sizeof out->raw);
    if (err != ESP_OK) {
        return err;
    }
    out->osc_stopped = (out->raw[0] & SEC_OS) != 0;
    /* STOP set or 12 h mode: registers are not trustworthy, treat as not decoded (caller logs ctrl1) */
    bool ok = !(out->ctrl1 & (CTRL1_12H | CTRL1_STOP)) && hy_rtc_decode(out->raw, &out->epoch);
    out->decoded = ok;
    return ESP_OK;
}

esp_err_t pcf85063_write(int64_t epoch)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[8];
    if (!hy_rtc_encode(epoch, buf + 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = REG_SEC;

    uint8_t ctrl1;
    esp_err_t err = read_regs(REG_CTRL1, &ctrl1, 1);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t want = ctrl1 & (uint8_t)~(CTRL1_STOP | CTRL1_12H);
    if (want != ctrl1) {
        uint8_t w[2] = { REG_CTRL1, want };
        err = i2c_master_transmit(s_dev, w, sizeof w, I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
    }
    return i2c_master_transmit(s_dev, buf, sizeof buf, I2C_TIMEOUT_MS);
}
