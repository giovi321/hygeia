#ifndef HY_TZLOCK_H
#define HY_TZLOCK_H

/*
 * The TZ environment variable and the libc time-zone state are process-wide. Every
 * setenv("TZ") / tzset / localtime_r / mktime that depends on the zone runs between
 * hy_tz_lock() and hy_tz_unlock(), in hy_core and in the device code alike.
 */

#include <stdbool.h>

typedef void (*hy_tz_lock_fn)(void *ctx);

/* Registers the lock used around every setenv("TZ")/tzset/localtime_r/mktime in hy_core.
   NULL functions = no locking (host default). The lock must be recursive. */
void hy_tz_set_lock(hy_tz_lock_fn lock, hy_tz_lock_fn unlock, void *ctx);
void hy_tz_lock(void);
void hy_tz_unlock(void);

#define HY_SAVED_TZ_LEN 64
typedef struct {
    bool had;
    char value[HY_SAVED_TZ_LEN];
} hy_saved_tz_t;

/* Takes the TZ lock, saves the current TZ in a fixed buffer (no allocation that could fail), sets
 * TZ to posix_tz (NULL = "UTC0") and calls tzset. Returns false, with TZ unchanged and the lock
 * released, when the previous TZ does not fit the buffer; the caller must then not compute local
 * times. Our own zones are at most HY_TZ_LEN - 1 characters, so this only guards foreign TZs.
 * On success the caller ends with hy_tz_leave. */
bool hy_tz_enter(hy_saved_tz_t *saved, const char *posix_tz);
/* Restores the saved TZ, calls tzset, and only then releases the lock. */
void hy_tz_leave(const hy_saved_tz_t *saved);

#endif
