#include "hy/tzlock.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Registered once at start-up, before any other task touches the TZ, and not changed after. */
static hy_tz_lock_fn s_lock;
static hy_tz_lock_fn s_unlock;
static void *s_ctx;

void hy_tz_set_lock(hy_tz_lock_fn lock, hy_tz_lock_fn unlock, void *ctx)
{
    s_lock = lock;
    s_unlock = unlock;
    s_ctx = ctx;
}

void hy_tz_lock(void)
{
    if (s_lock) s_lock(s_ctx);
}

void hy_tz_unlock(void)
{
    if (s_unlock) s_unlock(s_ctx);
}

bool hy_tz_enter(hy_saved_tz_t *saved, const char *posix_tz)
{
    hy_tz_lock();
    const char *cur = getenv("TZ");
    saved->had = cur != NULL;
    saved->value[0] = '\0';
    if (cur) {
        size_t n = strlen(cur);
        if (n >= sizeof saved->value) {
            hy_tz_unlock();
            return false;
        }
        memcpy(saved->value, cur, n + 1);
    }
    setenv("TZ", posix_tz ? posix_tz : "UTC0", 1);
    tzset();
    return true;
}

void hy_tz_leave(const hy_saved_tz_t *saved)
{
    if (saved->had) setenv("TZ", saved->value, 1);
    else unsetenv("TZ");
    tzset();
    hy_tz_unlock();
}
