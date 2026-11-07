#include "hy/tzlock.h"

#include <stdbool.h>
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

/* The TZ value lives in one fixed slot. newlib's setenv never frees the string it replaces: a value longer than
 * the current one gets a fresh malloc and the old one is lost, so switching between a short and a long zone (the
 * zone change record does that at every evaluation) leaked about 40 bytes of internal RAM each time. The slot is
 * made once by a setenv of a full-length placeholder; after that the value is written into it in place and setenv
 * is not called again. getenv("TZ") points into the environment string, which is writable. */
static char *s_slot;

bool hy_tz_put(const char *value)
{
    size_t n = strlen(value);
    if (n >= HY_SAVED_TZ_LEN) return false;
    char *cur = getenv("TZ");
    if (!s_slot || cur != s_slot) {
        char fill[HY_SAVED_TZ_LEN];
        memset(fill, 'x', sizeof fill - 1);
        fill[sizeof fill - 1] = '\0';
        if (setenv("TZ", fill, 1) != 0) return false;
        s_slot = getenv("TZ");
        if (!s_slot) return false;
    }
    memcpy(s_slot, value, n + 1);
    return true;
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
    const char *want = posix_tz ? posix_tz : "UTC0";
    /* Already in force (the common case: the clock's own zone): setenv + tzset would only re-parse it */
    if (!cur || strcmp(cur, want) != 0) {
        if (!hy_tz_put(want)) setenv("TZ", want, 1);   /* longer than any zone of ours: the old way */
        tzset();
    }
    return true;
}

void hy_tz_leave(const hy_saved_tz_t *saved)
{
    const char *cur = getenv("TZ");
    bool same = saved->had ? (cur && strcmp(cur, saved->value) == 0) : cur == NULL;
    if (!same) {
        /* An unset TZ means UTC to libc; "UTC0" says the same without dropping the slot */
        if (!hy_tz_put(saved->had ? saved->value : "UTC0")) setenv("TZ", saved->value, 1);
        tzset();
    }
    hy_tz_unlock();
}
