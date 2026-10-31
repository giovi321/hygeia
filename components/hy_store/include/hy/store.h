#ifndef HY_STORE_H
#define HY_STORE_H

#include <stddef.h>
#include "hy/actions.h"
#include "hy/settings.h"

/*
 * stdio-based persistence. base_dir must exist ("/data" on device, a temp
 * directory on host). Layout: <base>/meds.json, <base>/gaps.jsonl and <base>/log/YYYY-MM.jsonl
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
 * - append_gap appends one fsync'd line to <base>/gaps.jsonl (append-only, in end order). When
 *   the file passes 1100 lines after an append it is rewritten atomically (temp, fsync,
 *   rename) keeping the newest 1000. query_gaps returns the gaps overlapping [from, to),
 *   ascending by start, at most max (the earliest), with their reasons (reasons may be NULL).
 * - Corrupt or over-long lines are skipped on every read path and counted.
 */
hy_err_t hy_store_open(const char *base_dir);
const hy_store_port_t *hy_store_port(void);   /* ctx is NULL */
size_t   hy_store_skipped_lines(void);        /* corrupt lines skipped since open */
/* The id the next appended log entry takes (0xFFFFFFFF once the ids ran out). */
uint32_t hy_store_next_log_id(void);

/*
 * Staging for an import that replaces everything. Same thread as the port.
 * - begin(bytes): HY_E_FULL when the free space hook reports less than 2 * bytes. Removes a leftover
 *   <base>/import.tmp/ and creates it with an empty meds.json, gaps.jsonl and log/ (all three always exist).
 * - meds, log, gap write there with ids kept (log ids must be nonzero; HY_E_INVALID otherwise). The live
 *   store keeps working meanwhile.
 * - commit: removes an older <base>/pre-import/, writes the marker import.tmp/commit, then for each of
 *   meds.json, gaps.jsonl and log/ moves the live one into pre-import/ and the staged one in, all by rename,
 *   then removes import.tmp and re-runs the open logic (month index, next log id). Every step can be repeated,
 *   so hy_store_open finishes a commit that a reset interrupted after the marker, and discards a staging area
 *   without one. HY_E_IO when a rename failed: the open logic still runs; nothing is deleted except the older
 *   pre-import/.
 * - abort removes import.tmp. Calls outside begin..commit/abort give HY_E_INVALID.
 */
hy_err_t hy_store_import_begin(size_t bytes);
hy_err_t hy_store_import_meds(const hy_med_t *m, size_t n, uint32_t next_id);
hy_err_t hy_store_import_log(const hy_log_t *e, size_t n);
hy_err_t hy_store_import_gap(const hy_gap_t *g, hy_gap_reason_t r);
hy_err_t hy_store_import_commit(void);
void     hy_store_import_abort(void);
/* Free bytes of the file system holding base_dir. NULL (the default) skips the space check. */
void     hy_store_set_free_fn(size_t (*fn)(void));

#ifdef ESP_PLATFORM
#include "esp_err.h"
/* Device only: mounts the LittleFS partition "storage" at "/data"; formats it only if the mount fails. */
esp_err_t hy_store_mount(void);

/* Settings in NVS namespace "settings" (keys sound, dnd, vol, bright, snooze, dim_s, off_bat_s, off_usb_s, rot_lock, rot, q_from, q_to, wifi_batt). Needs nvs_flash_init first.
 * load: missing keys give defaults, then clamps, and refreshes the RAM copy. save: writes the given values as is. */
esp_err_t hy_settings_load(hy_settings_t *s);
esp_err_t hy_settings_save(const hy_settings_t *s);
/* hy_settings() and hy_settings_update() share one unlocked RAM copy: call them from the LVGL task only.
 * RAM copy loaded at boot (defaults until hy_settings_load runs). Never NULL. */
const hy_settings_t *hy_settings(void);
/* Clamps, saves, and updates the RAM copy (the RAM copy is updated even when the save fails). */
esp_err_t hy_settings_update(const hy_settings_t *s);
#endif

#endif /* HY_STORE_H */
