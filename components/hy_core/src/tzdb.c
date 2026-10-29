#include "hy/tzdb.h"

#include <stdint.h>
#include <string.h>

/* Defined in the generated tzdb_data.c. */
extern const size_t hy_tzdb_n;
extern const char *const hy_tzdb_posix_tab[];
extern const char *const hy_tzdb_names[];
extern const uint8_t hy_tzdb_posix_idx[];
extern const size_t hy_tzdb_region_n;
extern const char *const hy_tzdb_region_tab[];

size_t hy_tzdb_count(void)
{
    return hy_tzdb_n;
}

bool hy_tzdb_at(size_t i, const char **iana, const char **posix)
{
    if (i >= hy_tzdb_n) return false;
    if (iana) *iana = hy_tzdb_names[i];
    if (posix) *posix = hy_tzdb_posix_tab[hy_tzdb_posix_idx[i]];
    return true;
}

const char *hy_tzdb_posix(const char *iana)
{
    if (!iana) return NULL;
    size_t lo = 0, hi = hy_tzdb_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(iana, hy_tzdb_names[mid]);
        if (c == 0) return hy_tzdb_posix_tab[hy_tzdb_posix_idx[mid]];
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

size_t hy_tzdb_regions(const char **out, size_t max)
{
    size_t n = hy_tzdb_region_n < max ? hy_tzdb_region_n : max;
    for (size_t i = 0; i < n; i++) out[i] = hy_tzdb_region_tab[i];
    return n;
}
