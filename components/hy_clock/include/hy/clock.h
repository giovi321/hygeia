#ifndef HY_CLOCK_H
#define HY_CLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "hy/actions.h"
#include "hy/clock_valid.h"
#include "hy/occ.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default POSIX TZ when none is stored in NVS (namespace "clock", key "tz") */
#define HY_CLOCK_DEFAULT_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
/* Default IANA zone name, matching HY_CLOCK_DEFAULT_TZ (NVS key "zone") */
#define HY_CLOCK_DEFAULT_ZONE "Europe/Zurich"
/* Room for an IANA zone name, including the NUL */
#define HY_ZONE_LEN 48

/*
 * Create the recursive TZ mutex and register it with hy_tz_set_lock (before anything else
 * touches the TZ), add the PCF85063 (0x51) on the shared bus, read it, set the system time if
 * plausible, load the TZ, zone name and last zone change from NVS and apply the TZ. Needs
 * nvs_flash_init() done first. Returns an error only when the RTC cannot be reached; an
 * implausible RTC time returns ESP_OK with hy_clock_valid() false.
 */
esp_err_t hy_clock_init(i2c_master_bus_handle_t bus);

bool hy_clock_valid(void);

/*
 * Set RTC (clears the oscillator-stop flag), system time and TZ (persisted in NVS).
 * posix_tz NULL keeps the current TZ. A non-NULL posix_tz that differs from the current one
 * records a tz change {utc, old, new} and clears the IANA zone name. ESP_ERR_INVALID_ARG if
 * the TZ is empty or at least HY_TZ_LEN long, or utc is before the build epoch. source (e.g. "serial") is logged and kept.
 * The time and zone are in force on ESP_OK and on an NVS error; an NVS error means the zone was
 * not persisted. All state is safe to read from other tasks. The pointer from hy_clock_tz() and the
 * port tz() holds a complete string until the second following TZ change.
 */
esp_err_t hy_clock_set(int64_t utc, const char *posix_tz, const char *source);

const char *hy_clock_tz(void);

/* Copies the current POSIX TZ under the clock mutex. Always NUL-terminates. */
void      hy_clock_tz_copy(char *out, size_t len);
/* Current IANA name ("" when the zone was set as a raw POSIX string). Default "Europe/Zurich". */
void      hy_clock_zone_copy(char *out, size_t len);
/* Changes only the zone. Writes NVS keys "zone" and "tz", records a tz change {now, old, new}
   in NVS ("chg_at", "chg_old", "chg_new") when the POSIX string differs. RTC untouched.
   ESP_ERR_INVALID_ARG for an empty or too long tz, ESP_ERR_INVALID_STATE when the clock is invalid. */
esp_err_t hy_clock_set_zone(const char *iana, const char *posix_tz);
/* Last recorded change; false when none. */
bool      hy_clock_last_tzchange(hy_tzchange_t *out);
/* Incremented by every successful hy_clock_set; *jump_s = new UTC - system UTC just before the set. */
uint32_t  hy_clock_sync_seq(int64_t *jump_s);

/* Copy out the last successful sync: source name (empty if none yet) and the UTC time set */
void hy_clock_last_sync(char *src, size_t src_len, int64_t *at);

/* Log a fresh RTC read (raw registers, decoded UTC), system time (UTC and local), TZ, zone name,
 * validity, last sync, sync sequence and the last tz change */
void hy_clock_report(void);

const hy_clock_port_t *hy_clock_port(void);

#ifdef __cplusplus
}
#endif

#endif
