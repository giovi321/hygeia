#ifndef HY_RTC_CODEC_H
#define HY_RTC_CODEC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Pure PCF85063 time-register codec, no ESP-IDF headers. raw[7] is registers 0x04..0x0A:
 * seconds, minutes, hours (24 h), day, weekday, month, year (BCD, 00..99 = 2000..2099).
 */

/* Decode to UTC epoch seconds. The OS flag (seconds bit 7) is ignored. False for non-BCD digits,
 * out-of-range fields or a day past the month length (leap years included). */
bool hy_rtc_decode(const uint8_t raw[7], int64_t *epoch);

/* Encode an epoch in 2000-01-01..2099-12-31 into raw[7] (weekday 0 = Sunday, OS flag clear). False if out of range. */
bool hy_rtc_encode(int64_t epoch, uint8_t raw[7]);

#endif
