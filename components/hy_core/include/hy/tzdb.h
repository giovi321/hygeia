#ifndef HY_TZDB_H
#define HY_TZDB_H

/*
 * IANA zone name -> POSIX TZ string table, generated from third_party/posix_tz_db by
 * tools/gen_tzdb.py into src/tzdb_data.c. Entries are sorted by IANA name (strcmp order).
 */

#include <stdbool.h>
#include <stddef.h>

size_t hy_tzdb_count(void);

/* Entry i in name order. Returns false past the end. */
bool hy_tzdb_at(size_t i, const char **iana, const char **posix);

/* Exact IANA name lookup (binary search); NULL when unknown. */
const char *hy_tzdb_posix(const char *iana);

/* Regions = text before the first '/', in table order, unique ("Africa", "America", ... , "Etc").
   Writes at most max pointers into out and returns the number of regions found. */
size_t hy_tzdb_regions(const char **out, size_t max);

#endif
