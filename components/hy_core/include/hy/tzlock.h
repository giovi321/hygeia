#ifndef HY_TZLOCK_H
#define HY_TZLOCK_H

/*
 * The TZ environment variable and the libc time-zone state are process-wide. Every
 * setenv("TZ") / tzset / localtime_r / mktime that depends on the zone runs between
 * hy_tz_lock() and hy_tz_unlock(), in hy_core and in the device code alike.
 */

typedef void (*hy_tz_lock_fn)(void *ctx);

/* Registers the lock used around every setenv("TZ")/tzset/localtime_r/mktime in hy_core.
   NULL functions = no locking (host default). The lock must be recursive. */
void hy_tz_set_lock(hy_tz_lock_fn lock, hy_tz_lock_fn unlock, void *ctx);
void hy_tz_lock(void);
void hy_tz_unlock(void);

#endif
