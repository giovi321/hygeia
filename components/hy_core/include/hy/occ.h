#ifndef HY_OCC_H
#define HY_OCC_H

#include <stddef.h>
#include <stdint.h>
#include "hy/types.h"

/*
 * Fills out[] with UTC epochs of scheduled occurrences in [from, to), ascending.
 * Uses posix_tz for local time (setenv("TZ") + tzset internally, restoring the
 * previous TZ). Honours weekdays, active_from/active_until and archived
 * (archived and as-needed medicines produce none). Returns the count, at most max.
 *
 * Local-time rule: a time that does not exist (spring-forward gap) is scheduled
 * at the first valid minute after the gap; a time that occurs twice (fall-back)
 * is scheduled once, at its first occurrence.
 */
size_t hy_occurrences(const hy_med_t *m, int64_t from, int64_t to,
                      const char *posix_tz, int64_t *out, size_t max);

#endif
