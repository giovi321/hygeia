#ifndef HY_API_INT_H
#define HY_API_INT_H

/* Helpers shared by the api_*.c files. */

#include <stdbool.h>
#include "cJSON.h"
#include "codec_obj.h"
#include "hy/api.h"

/* Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF. */
bool hy_api_utf8_valid(const char *s);

/* Fills *err with the mapping of e and the given field (NULL = keep the mapping's own field), returns e. */
hy_err_t hy_api_fail(hy_api_err_t *err, hy_err_t e, const char *field);

/* Prints o unformatted into buf when it fits. Length, or -1. Frees nothing. */
int hy_api_print(const cJSON *o, char *buf, size_t len);

/* "YYYY-MM-DD HH:MM" of ts in posix_tz (empty = UTC). False when the zone cannot be entered. */
bool hy_api_fmt_local(int64_t ts, const char *posix_tz, char *buf, size_t len);

#endif /* HY_API_INT_H */
