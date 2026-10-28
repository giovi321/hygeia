#include "pcf85063.h"

#include <string.h>

#include "esp_log.h"

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

static int bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static bool bcd_ok(uint8_t v) { return (v >> 4) <= 9 && (v & 0x0F) <= 9; }
static uint8_t dec2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Days since 1970-01-01 for a proleptic Gregorian date (public-domain days_from_civil algorithm) */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

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
    const uint8_t *r = out->raw;
    out->osc_stopped = (r[0] & SEC_OS) != 0;
    uint8_t sec = r[0] & 0x7F, min = r[1] & 0x7F, hr = r[2] & 0x3F;
    uint8_t day = r[3] & 0x3F, mon = r[5] & 0x1F, yr = r[6];
    bool ok = !(out->ctrl1 & CTRL1_12H) && bcd_ok(sec) && bcd_ok(min) && bcd_ok(hr) &&
              bcd_ok(day) && bcd_ok(mon) && bcd_ok(yr);
    if (ok) {
        int s = bcd2dec(sec), mi = bcd2dec(min), h = bcd2dec(hr);
        int d = bcd2dec(day), mo = bcd2dec(mon), y = 2000 + bcd2dec(yr);
        ok = s <= 59 && mi <= 59 && h <= 23 && d >= 1 && d <= 31 && mo >= 1 && mo <= 12;
        if (ok) {
            out->epoch = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
        }
    }
    out->decoded = ok;
    return ESP_OK;
}

esp_err_t pcf85063_write(int64_t epoch)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (epoch < 946684800 || epoch > 4102444799LL) { /* 2000-01-01 .. 2099-12-31 */
        return ESP_ERR_INVALID_ARG;
    }
    int64_t days = epoch / 86400;
    int64_t rem = epoch % 86400;
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    int wd = (int)((days + 4) % 7); /* 1970-01-01 was a Thursday; 0 = Sunday */

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
    uint8_t buf[8];
    buf[0] = REG_SEC;
    buf[1] = dec2bcd((int)(rem % 60)) & 0x7F; /* bit 7 clear: OS flag cleared */
    buf[2] = dec2bcd((int)(rem / 60 % 60));
    buf[3] = dec2bcd((int)(rem / 3600));
    buf[4] = dec2bcd(d);
    buf[5] = (uint8_t)wd;
    buf[6] = dec2bcd(m);
    buf[7] = dec2bcd(y - 2000);
    return i2c_master_transmit(s_dev, buf, sizeof buf, I2C_TIMEOUT_MS);
}
