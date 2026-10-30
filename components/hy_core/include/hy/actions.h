#ifndef HY_ACTIONS_H
#define HY_ACTIONS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "hy/gap.h"
#include "hy/occ.h"
#include "hy/types.h"

typedef struct {
    hy_err_t (*load_meds)(void *ctx, hy_med_t *out, size_t max, size_t *n);
    hy_err_t (*save_meds)(void *ctx, const hy_med_t *meds, size_t n);
    hy_err_t (*append_log)(void *ctx, hy_log_t *e);              /* assigns e->id */
    hy_err_t (*update_log)(void *ctx, const hy_log_t *e);
    hy_err_t (*delete_log)(void *ctx, uint32_t id, int64_t ts);  /* ts locates the month */
    size_t   (*query_log)(void *ctx, int64_t from, int64_t to, uint32_t med_id /*0 = all*/,
                          hy_log_t *out, size_t max);
    /* Untracked gaps. Both may be NULL (host fixtures that do not need them; the device port has both):
     * hy_act_record_gap then returns HY_E_INVALID. query_gaps returns the gaps overlapping
     * [from, to) ascending by start, with their reasons (a hy_gap_reason_t value). */
    hy_err_t (*append_gap)(void *ctx, const hy_gap_t *g, uint8_t reason);
    size_t   (*query_gaps)(void *ctx, int64_t from, int64_t to, hy_gap_t *out, uint8_t *reasons,
                           size_t max);
} hy_store_port_t;

typedef struct {
    int64_t     (*now)(void *ctx);
    const char *(*tz)(void *ctx);
    bool        (*valid)(void *ctx);
    /* Last recorded time-zone change. May be NULL; returns false when no change is recorded. */
    bool        (*last_change)(void *ctx, hy_tzchange_t *out);
} hy_clock_port_t;

#define HY_MAX_MEDS    50    /* active (non-archived) medicines; sizes every per-active array */
#define HY_MAX_STORED  100   /* all stored medicines, archived included; sizes the action module's arrays */
#define HY_UNDO_WINDOW_S 5

/*
 * Single-threaded by contract: call from one task only. The port structs must
 * outlive the module (the pointers are kept). Resets all state (medicines, undo
 * records) and loads medicines via load_meds. Returns HY_E_INVALID for NULL
 * ports and HY_E_IO if loading fails (the module then holds no medicines).
 * Event subscribers are not touched.
 */
hy_err_t hy_actions_init(const hy_store_port_t *s, void *sctx, const hy_clock_port_t *c, void *cctx);

/* Active and archived medicines (at most HY_MAX_STORED), stable-sorted by `order`. The pointer
 * stays valid until the next action. */
size_t   hy_meds(const hy_med_t **out);
/* Medicines in hy_meds that are not archived (at most HY_MAX_MEDS through the actions below). */
size_t   hy_meds_active_count(void);

/*
 * id 0 = new: assigns id = max id over all medicines (archived too) + 1; HY_E_FULL when
 * the new medicine is active and HY_MAX_MEDS medicines are active, or when HY_MAX_STORED
 * medicines are stored. An existing id that goes from archived to active gives HY_E_FULL
 * when HY_MAX_MEDS medicines are active. A nonzero unknown id gives HY_E_NOT_FOUND. Validation errors
 * come from hy_med_validate against all current medicines. The whole array is
 * saved; on a save failure (HY_E_IO) the in-memory state is unchanged.
 * For an existing id the in-memory pills_left is kept and the caller's value is ignored
 * (the caller's copy may be stale), except when the caller turns pills_enabled from false
 * to true: then the caller's value is the starting count. A new medicine takes the caller's value.
 * Publishes HY_EV_MEDS_CHANGED on success.
 */
hy_err_t hy_act_med_upsert(hy_med_t *m);
/* Sets pills_left (0..9999, else HY_E_INVALID; unknown id HY_E_NOT_FOUND), saves the
 * medicines (HY_E_IO, state unchanged on failure) and publishes HY_EV_MEDS_CHANGED. */
hy_err_t hy_act_pills_set(uint32_t med_id, int16_t left);
/* Debug console only. Permanently deletes the medicines with these ids (active or archived): their
 * log entries and snoozes first, then the records (one save). Unknown ids are skipped. *n_removed gets
 * the medicines removed, *n_logs the log entries deleted (either may be NULL). HY_E_IO when a log delete
 * or the save fails; medicines whose logs were all deleted are still removed in that case only if the
 * save works. Publishes HY_EV_MEDS_CHANGED. */
hy_err_t hy_act_med_purge(const uint32_t *ids, size_t n_ids, size_t *n_removed, size_t *n_logs);
/* Unarchiving re-validates (code uniqueness) and gives HY_E_FULL when HY_MAX_MEDS medicines
 * are active. Same save and event rules as upsert. */
hy_err_t hy_act_med_archive(uint32_t id, bool archived);

/*
 * Records a taken dose. ts 0 = now. HY_E_CLOCK if the clock is invalid;
 * HY_E_NOT_FOUND for an unknown or archived medicine. Fills tz (clock port), slot
 * (hy_match_slot, with the clock port's last_change, over the store's logs for this medicine in
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

typedef struct { uint32_t med_id; int64_t slot; int64_t until; } hy_snooze_t;

/*
 * Writes an automatic missed entry: ts = now, tz, slot, delta_min = (now - slot) / 60,
 * status HY_MISSED, source HY_SRC_AUTO. HY_E_CLOCK, HY_E_NOT_FOUND (unknown, archived,
 * as-needed), HY_E_INVALID when slot is not an occurrence of the medicine. If any log
 * for (med, slot) exists (including a MISSED one): HY_OK, *out_log_id = 0, nothing
 * written, no event. Otherwise publishes HY_EV_MISSED with slot and drops a snooze for
 * that slot. Not undoable. HY_E_FULL when the look-back query is truncated.
 * The caller owns the timing: no missed-deadline check is made here.
 */
hy_err_t hy_act_mark_missed(uint32_t med_id, int64_t slot, uint32_t *out_log_id);

/*
 * Snoozes the open occurrence: slot = hy_match_slot at now; it must satisfy
 * base <= now < base + missed_after (base from hy_occ_base with the clock port's
 * last_change), else HY_E_INVALID. minutes 1..120 else HY_E_INVALID.
 * until = min(now + minutes*60, base + missed_after_min*60). Replaces an existing
 * snooze for the medicine. Publishes HY_EV_SNOOZED (ts now, slot). HY_E_CLOCK,
 * HY_E_NOT_FOUND as for log_taken (and for as-needed medicines, as for mark_missed).
 * HY_E_FULL (nothing written) when the look-back query returns 128 logs (possibly
 * truncated), as for log_taken.
 */
hy_err_t hy_act_snooze(uint32_t med_id, uint16_t minutes, hy_source_t src, int64_t *out_until);

/* Snoozes whose until is after now, at most HY_MAX_MEDS. Pointer valid until the next
 * action or call. Archived medicines are skipped. A taken or skipped entry for a
 * snoozed slot drops that snooze (a successful undo restores it unless it has expired);
 * archiving a medicine or changing its schedule through upsert drops it too. */
size_t hy_snoozes(const hy_snooze_t **out);

/* Sets edited = true, update_log, publishes HY_EV_LOG_EDITED. No clock needed. */
hy_err_t hy_act_log_edit(const hy_log_t *e);
/* delete_log, then publishes HY_EV_LOG_EDITED (there is no delete event). No clock needed. */
hy_err_t hy_act_log_delete(uint32_t log_id, int64_t ts);

/*
 * Appends the gap through the store port and publishes HY_EV_UNTRACKED {ts = end, slot = start,
 * value = reason}. HY_E_INVALID when end <= start, the reason is out of range, or the port lacks
 * append_gap; HY_E_IO when the append fails (nothing published). Does not need a valid clock.
 */
hy_err_t hy_act_record_gap(const hy_gap_t *g, hy_gap_reason_t reason);

#endif
