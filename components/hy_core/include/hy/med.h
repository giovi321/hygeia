#ifndef HY_MED_H
#define HY_MED_H

#include <stddef.h>
#include "hy/types.h"

/*
 * Validate a medicine. `others` is the current medicine list (n_others
 * entries) and may contain `m` itself; entries with the same id as `m` are
 * skipped. Returns HY_OK or the first failing rule, checked in this order:
 *   code format (HY_E_CODE), name (HY_E_NAME), color (HY_E_INVALID),
 *   schedule (HY_E_TIMES), code uniqueness (HY_E_CODE_DUP).
 */
hy_err_t hy_med_validate(const hy_med_t *m, const hy_med_t *others, size_t n_others);

#endif
