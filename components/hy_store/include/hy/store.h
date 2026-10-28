#ifndef HY_STORE_H
#define HY_STORE_H

#include <stddef.h>
#include "hy/actions.h"

/*
 * stdio-based persistence. base_dir must exist ("/data" on device, a temp
 * directory on host). Layout: <base>/meds.json and <base>/log/YYYY-MM.jsonl
 * (UTC month of ts). Open creates <base>/log, removes stray *.tmp files,
 * resets the skipped-line counter and sets the next log id to the highest id
 * found in the newest non-empty month file + 1. Single-threaded by contract.
 */
hy_err_t hy_store_open(const char *base_dir);
const hy_store_port_t *hy_store_port(void);   /* ctx is NULL */
size_t   hy_store_skipped_lines(void);        /* corrupt lines skipped since open */

#endif /* HY_STORE_H */
