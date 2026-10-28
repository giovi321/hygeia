#ifndef HY_CLOCK_H
#define HY_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "hy/actions.h"
#include "hy/clock_valid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default POSIX TZ when none is stored in NVS (namespace "clock", key "tz") */
#define HY_CLOCK_DEFAULT_TZ "CET-1CEST,M3.5.0,M10.5.0/3"

/*
 * Add the PCF85063 (0x51) on the shared bus, read it, set the system time if plausible,
 * load the TZ from NVS and apply it. Needs nvs_flash_init() done first. Returns an error
 * only when the RTC cannot be reached; an implausible RTC time returns ESP_OK with
 * hy_clock_valid() false.
 */
esp_err_t hy_clock_init(i2c_master_bus_handle_t bus);

bool hy_clock_valid(void);

/*
 * Set RTC (clears the oscillator-stop flag), system time and TZ (persisted in NVS).
 * posix_tz NULL keeps the current TZ. ESP_ERR_INVALID_ARG if the TZ is empty or at least
 * HY_TZ_LEN long, or utc is before the build epoch. source (e.g. "serial") is logged and kept.
 */
esp_err_t hy_clock_set(int64_t utc, const char *posix_tz, const char *source);

const char *hy_clock_tz(void);

/* Last successful sync: source name (empty if none yet) and UTC time of the sync */
const char *hy_clock_last_source(int64_t *at);

/* Log a fresh RTC read (raw registers, decoded UTC), system time (UTC and local), TZ, validity, last sync */
void hy_clock_report(void);

const hy_clock_port_t *hy_clock_port(void);

#ifdef __cplusplus
}
#endif

#endif
