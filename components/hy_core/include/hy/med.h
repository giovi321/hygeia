#ifndef HY_MED_H
#define HY_MED_H

#include <stddef.h>
#include "hy/types.h"

/*
 * Validate a medicine. `others` is the current medicine list (n_others
 * entries) and may contain `m` itself; entries with the same id as `m` are
 * skipped. Returns HY_OK or the first failing rule, checked in this order:
 *   code format (HY_E_CODE), name (HY_E_NAME), dose terminated within its
 *   17 bytes (HY_E_DOSE), color (HY_E_INVALID), dates (HY_E_INVALID),
 *   schedule (HY_E_TIMES), min_interval_min above HY_INTERVAL_MAX_MIN
 *   (HY_E_INTERVAL, both schedule types), code uniqueness (HY_E_CODE_DUP).
 */
#define HY_INTERVAL_MAX_MIN 1440

/* The schedule rule of hy_med_validate: no weekday bit above Sunday; for fixed, 1 to 8 distinct times below
   1440 and at least one weekday. As needed passes. */
bool hy_med_times_valid(const hy_med_t *m);
hy_err_t hy_med_validate(const hy_med_t *m, const hy_med_t *others, size_t n_others);

#endif
