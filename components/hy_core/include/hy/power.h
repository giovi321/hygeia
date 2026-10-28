#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HY_RST_POWERON = 0, HY_RST_OTHER } hy_reset_kind_t; /* caller maps esp_reset_reason() */

/* Decide whether the device may keep running after reset.
 * pwron_status: raw AXP2101 PWRON status byte (register 0x20).
 * Only a power-on reset is gated: it must have been caused by the PWR key (bit 0). Any other reset
 * (software restart, panic, watchdog, flash/RTS reset) keeps running. A power-on reset with no known
 * source bit set (status 0x00 or reserved bits only) boots (ruling R22), so a PMU that reports nothing
 * still gives a working device. EN mode (bit 5) also boots (R24). */
bool hy_power_on_allowed(uint8_t pwron_status, hy_reset_kind_t reset);

#ifdef __cplusplus
}
#endif
