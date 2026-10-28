#ifndef HY_STORE_H
#define HY_STORE_H

#include <stddef.h>
#include "hy/actions.h"

/*
 * stdio-based persistence. base_dir must exist ("/data" on device, a temp
 * directory on host). Layout: <base>/meds.json and <base>/log/YYYY-MM.jsonl
 * (UTC month of ts). Open creates <base>/log, removes stray *.tmp files,
 * resets the skipped-line counter and sets the next log id to the highest id
 * found in any month file + 1 (ids are global and monotonic).
 * Single-threaded by contract.
 *
 * Port semantics:
 * - load_meds: no meds.json gives HY_OK with n = 0; a corrupt file gives HY_E_IO
 *   and is left untouched.
 * - save_meds, update_log, delete_log rewrite via <name>.tmp + fsync + rename.
 * - append_log assigns e->id and writes one fsync'd line to the month of e->ts.
 * - update_log looks for the id in the month file of e->ts first, then in the
 *   other month files. If found elsewhere, the entry is written into the target
 *   month and then removed from the old one. A crash or IO failure between the
 *   two steps can leave the entry in both months. Not found: HY_E_NOT_FOUND.
 * - delete_log searches only the month of ts. Not found: HY_E_NOT_FOUND.
 * - query_log covers [from, to), filters by med_id (0 = all), returns entries
 *   ascending by ts (ties by id), at most max, keeping the earliest ones.
 * - Corrupt or over-long lines are skipped on every read path and counted.
 */
hy_err_t hy_store_open(const char *base_dir);
const hy_store_port_t *hy_store_port(void);   /* ctx is NULL */
size_t   hy_store_skipped_lines(void);        /* corrupt lines skipped since open */

#endif /* HY_STORE_H */
