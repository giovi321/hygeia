#ifndef HY_ACTIONS_H
#define HY_ACTIONS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "hy/types.h"

typedef struct {
    hy_err_t (*load_meds)(void *ctx, hy_med_t *out, size_t max, size_t *n);
    hy_err_t (*save_meds)(void *ctx, const hy_med_t *meds, size_t n);
    hy_err_t (*append_log)(void *ctx, hy_log_t *e);              /* assigns e->id */
    hy_err_t (*update_log)(void *ctx, const hy_log_t *e);
    hy_err_t (*delete_log)(void *ctx, uint32_t id, int64_t ts);  /* ts locates the month */
    size_t   (*query_log)(void *ctx, int64_t from, int64_t to, uint32_t med_id /*0 = all*/,
                          hy_log_t *out, size_t max);
} hy_store_port_t;

typedef struct {
    int64_t     (*now)(void *ctx);
    const char *(*tz)(void *ctx);
    bool        (*valid)(void *ctx);
} hy_clock_port_t;

#define HY_MAX_MEDS 50
#define HY_UNDO_WINDOW_S 5

/*
 * Single-threaded by contract: call from one task only. The port structs must
 * outlive the module (the pointers are kept). Resets all state (medicines, undo
 * records) and loads medicines via load_meds. Returns HY_E_INVALID for NULL
 * ports and HY_E_IO if loading fails (the module then holds no medicines).
 * Event subscribers are not touched.
 */
hy_err_t hy_actions_init(const hy_store_port_t *s, void *sctx, const hy_clock_port_t *c, void *cctx);

/* Active and archived medicines, stable-sorted by `order`. The pointer stays valid until the next action. */
size_t   hy_meds(const hy_med_t **out);

/*
 * id 0 = new: assigns id = max id over all medicines (archived too) + 1; HY_E_FULL
 * at HY_MAX_MEDS. A nonzero unknown id gives HY_E_NOT_FOUND. Validation errors
 * come from hy_med_validate against all current medicines. The whole array is
 * saved; on a save failure (HY_E_IO) the in-memory state is unchanged.
 * Publishes HY_EV_MEDS_CHANGED on success.
 */
hy_err_t hy_act_med_upsert(hy_med_t *m);
/* Unarchiving re-validates (code uniqueness). Same save and event rules as upsert. */
hy_err_t hy_act_med_archive(uint32_t id, bool archived);

/*
 * Records a taken dose. ts 0 = now. HY_E_CLOCK if the clock is invalid;
 * HY_E_NOT_FOUND for an unknown or archived medicine. Fills tz (clock port), slot
 * (hy_match_slot over the store's logs for this medicine in
 * [ts - 8 days, max(ts + early window, now)]; the bound includes now because a backdated
 * ts can precede entries written later) and
 * delta_min ((ts - slot) / 60 toward zero, 0 for a free log). A HY_MISSED entry for
 * the matched slot is converted in place (same id, status HY_TAKEN, `edited` stays
 * false) instead of appending. When pills_enabled and pills_left > 0 the count is
 * decremented and the medicines saved.
 * The log write is the commit point. If it fails: HY_E_IO, nothing else changes, no
 * event. Once it succeeds the action succeeds: if the pill save then fails, HY_OK is
 * still returned, the undo record is created, HY_EV_TAKEN is published, and the
 * decremented count stays in memory until the next successful meds save persists it.
 * HY_E_FULL (nothing written) when the store returns 128 logs for the look-back
 * query, since matching on truncated data could pick the wrong slot. As-needed
 * medicines (not HY_SCHED_FIXED) skip the query: slot 0, never HY_E_FULL.
 * Every action returns HY_E_INVALID before a successful hy_actions_init.
 * A NULL tz() from the clock port is treated as "UTC0".
 */
hy_err_t hy_act_log_taken(uint32_t med_id, int64_t ts /*0 = now*/, const char *note,
                          hy_source_t src, uint32_t *out_log_id);

/*
 * Skips the slot matched at now. HY_E_CLOCK, HY_E_NOT_FOUND as for log_taken;
 * HY_E_INVALID when no slot matches (as-needed or nothing due), writing nothing.
 * Writes a HY_SKIPPED entry (converting a HY_MISSED one in place), no pill
 * change, publishes HY_EV_SKIPPED.
 */
hy_err_t hy_act_skip(uint32_t med_id, hy_source_t src, uint32_t *out_log_id);

/*
 * Undoes a taken or skipped entry created by this module, within HY_UNDO_WINDOW_S
 * seconds of creation (clock now). Up to 8 recent records are kept; older ones are
 * forgotten. No record: HY_E_NOT_FOUND. Older than the window: HY_E_UNDO_EXPIRED and
 * the record is dropped, so later calls give HY_E_NOT_FOUND. Otherwise the entry is
 * deleted (or the converted MISSED entry restored), a decremented pill count is
 * restored (+1) and the medicines saved, the record is dropped and HY_EV_UNDONE is
 * published. If the log operation fails: HY_E_IO, record kept. The log operation is
 * the commit point: a failed pill save is not reported and the restored count stays
 * in memory until the next successful meds save. A clock stepped back (now before
 * creation) counts as expired. Needs a valid clock (HY_E_CLOCK otherwise).
 */
hy_err_t hy_act_undo(uint32_t log_id);

/* Sets edited = true, update_log, publishes HY_EV_LOG_EDITED. No clock needed. */
hy_err_t hy_act_log_edit(const hy_log_t *e);
/* delete_log, then publishes HY_EV_LOG_EDITED (there is no delete event). No clock needed. */
hy_err_t hy_act_log_delete(uint32_t log_id, int64_t ts);

#endif
