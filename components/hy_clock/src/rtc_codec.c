#include "hy/rtc_codec.h"

/*
 * Field masks follow the SensorLib PCF85063 driver (lewisxhe/SensorLib, MIT,
 * commit dff98e79753dedd7ba709d1e92d71b4353c55005): seconds and minutes 0x7F, hours 24 h 0x3F,
 * day 0x3F, month 0x1F, year 00..99 read as 2000 + yy; seconds bit 7 is the OS flag.
 */

static int bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static bool bcd_ok(uint8_t v) { return (v >> 4) <= 9 && (v & 0x0F) <= 9; }
static uint8_t dec2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_in_month(int y, int m)
{
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 2 && is_leap(y)) ? 29 : dm[m - 1];
}

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

bool hy_rtc_decode(const uint8_t raw[7], int64_t *epoch)
{
    uint8_t sec = raw[0] & 0x7F, min = raw[1] & 0x7F, hr = raw[2] & 0x3F;
    uint8_t day = raw[3] & 0x3F, mon = raw[5] & 0x1F, yr = raw[6];
    if (!(bcd_ok(sec) && bcd_ok(min) && bcd_ok(hr) && bcd_ok(day) && bcd_ok(mon) && bcd_ok(yr))) {
        return false;
    }
    int s = bcd2dec(sec), mi = bcd2dec(min), h = bcd2dec(hr);
    int d = bcd2dec(day), mo = bcd2dec(mon), y = 2000 + bcd2dec(yr);
    if (s > 59 || mi > 59 || h > 23 || mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo)) {
        return false;
    }
    *epoch = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    return true;
}

bool hy_rtc_encode(int64_t epoch, uint8_t raw[7])
{
    if (epoch < 946684800LL || epoch > 4102444799LL) { /* 2000-01-01 .. 2099-12-31 23:59:59 */
        return false;
    }
    int64_t days = epoch / 86400;
    int64_t rem = epoch % 86400;
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    raw[0] = dec2bcd((int)(rem % 60)) & 0x7F; /* bit 7 clear: OS flag cleared */
    raw[1] = dec2bcd((int)(rem / 60 % 60));
    raw[2] = dec2bcd((int)(rem / 3600));
    raw[3] = dec2bcd(d);
    raw[4] = (uint8_t)((days + 4) % 7); /* 1970-01-01 was a Thursday; 0 = Sunday */
    raw[5] = dec2bcd(m);
    raw[6] = dec2bcd(y - 2000);
    return true;
}

bool hy_rtc_alarm_encode(int64_t epoch, uint8_t raw[5])
{
    uint8_t t[7];
    if (!hy_rtc_encode(epoch, t)) {
        return false;
    }
    raw[0] = t[0] & 0x7F; /* AEN clear: field enabled */
    raw[1] = t[1] & 0x7F;
    raw[2] = t[2] & 0x3F;
    raw[3] = t[3] & 0x3F;
    raw[4] = 0x80;        /* weekday disabled */
    return true;
}
