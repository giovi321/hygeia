#include "hy/tzlock.h"

#include <stddef.h>

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
