#ifndef HY_STORE_H
#define HY_STORE_H

#include <stddef.h>
#include "hy/actions.h"
#include "hy/settings.h"

/*
 * stdio-based persistence. base_dir must exist ("/data" on device, a temp
 * directory on host). Layout: <base>/meds.json and <base>/log/YYYY-MM.jsonl
 * (UTC month of ts). Open creates <base>/log, removes stray *.tmp files,
 * resets the skipped-line counter and sets the next log id to the highest id
 * found in any month file + 1 (ids are global and monotonic). The month files are
 * listed once at open into a sorted index (at most 1200 months, more gives HY_E_FULL
 * from open); a write that creates a month file adds it, and every operation iterates
 * the index instead of reading the directory. Names that are not exactly YYYY-MM.jsonl
 * are ignored.
 * Single-threaded by contract.
 *
 * Port semantics:
 * - load_meds: no meds.json gives HY_OK with n = 0; a corrupt file gives HY_E_IO
 *   and is left untouched.
 * - save_meds, update_log, delete_log rewrite via <name>.tmp + fsync + rename.
 * - append_log assigns e->id and writes one fsync'd line to the month of e->ts. When the
 *   next id would pass 0xFFFFFFFE it returns HY_E_FULL and writes nothing.
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

#ifdef ESP_PLATFORM
#include "esp_err.h"
/* Device only: mounts the LittleFS partition "storage" at "/data"; formats it only if the mount fails. */
esp_err_t hy_store_mount(void);

/* Settings in NVS namespace "settings" (keys sound, dnd, vol, bright, snooze). Needs nvs_flash_init first.
 * load: missing keys give defaults, then clamps, and refreshes the RAM copy. save: writes the given values as is. */
esp_err_t hy_settings_load(hy_settings_t *s);
esp_err_t hy_settings_save(const hy_settings_t *s);
/* RAM copy loaded at boot (defaults until hy_settings_load runs). Never NULL. */
const hy_settings_t *hy_settings(void);
/* Clamps, saves, and updates the RAM copy (the RAM copy is updated even when the save fails). */
esp_err_t hy_settings_update(const hy_settings_t *s);
#endif

#endif /* HY_STORE_H */
