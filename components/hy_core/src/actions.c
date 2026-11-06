#include <string.h>
#include "hy/actions.h"
#include "hy/events.h"
#include "hy/gap.h"
#include "hy/med.h"
#include "hy/occ.h"

#define UNDO_SLOTS 8
#define QUERY_MAX 128
#define PURGE_LOG_END 4102444800LL   /* 2100-01-01 UTC */
#define LOOKBACK_S (8 * 86400LL)
#define LOG_TS_MAX 253402300799LL   /* 9999-12-31 UTC: the latest month the store accepts */

typedef struct {
    bool used;
    uint32_t log_id, med_id;
    int64_t ts;          /* ts of the entry as written (locates the month) */
    int64_t slot;        /* slot of the entry, 0 for a free log */
    int64_t created;     /* clock now at creation */
    bool pill_dec;       /* a pill was taken off pills_left */
    bool converted;      /* a MISSED entry was updated in place */
    hy_log_t original;   /* valid when converted */
    bool had_snooze;     /* the entry dropped a snooze */
    hy_snooze_t snooze;  /* valid when had_snooze */
} undo_rec_t;

static const hy_store_port_t *store;
static void *store_ctx;
static const hy_clock_port_t *clk;
static void *clk_ctx;

static HY_EXT_BSS hy_med_t meds[HY_MAX_STORED];
static HY_EXT_BSS hy_med_t scratch[HY_MAX_STORED];
static size_t n_meds;
static uint32_t next_med_id;   /* id the next new medicine takes; only grows, so a deleted id is not reused */
static HY_EXT_BSS undo_rec_t undo[UNDO_SLOTS];
static size_t undo_next;
static bool inited;
static hy_log_t last_log;       /* the entry the last successful log write left behind */
static bool last_log_ok;
static HY_EXT_BSS hy_log_t qbuf[QUERY_MAX];
static HY_EXT_BSS hy_snooze_t snz[HY_MAX_MEDS];      /* archived medicines have no snooze */
static size_t n_snz;
static HY_EXT_BSS hy_snooze_t snz_out[HY_MAX_MEDS];
static bool snooze_drop(uint32_t med_id, int64_t slot, hy_snooze_t *out);
static void undo_forget(uint32_t log_id);

static void last_log_set(const hy_log_t *e)
{
    last_log = *e;
    last_log_ok = true;
}

static void last_log_clear(uint32_t log_id)
{
    if (last_log_ok && last_log.id == log_id) last_log_ok = false;
}

bool hy_act_last_log(uint32_t log_id, hy_log_t *out)
{
    if (!last_log_ok || last_log.id != log_id) return false;
    if (out) *out = last_log;
    return true;
}

/* Largest id over the medicines in a, plus one (0xFFFFFFFF stays). */
static uint32_t id_after_max(const hy_med_t *a, size_t n)
{
    uint32_t mx = 0;
    for (size_t i = 0; i < n; i++)
        if (a[i].id > mx) mx = a[i].id;
    return mx == 0xFFFFFFFFu ? mx : mx + 1;
}

/* Saves the medicine array with the stored next id (raised past every id in it). next_med_id is not touched:
 * the caller updates it once the save has worked. */
static hy_err_t persist_meds(const hy_med_t *a, size_t n, uint32_t next)
{
    uint32_t floor_id = id_after_max(a, n);
    if (next < floor_id) next = floor_id;
    if (store->save_meds_next) return store->save_meds_next(store_ctx, a, n, next);
    return store->save_meds(store_ctx, a, n);
}

static void raise_next_id(const hy_med_t *a, size_t n)
{
    uint32_t floor_id = id_after_max(a, n);
    if (next_med_id < floor_id) next_med_id = floor_id;
}

static void publish(hy_ev_type_t type, int64_t ts, uint32_t med_id, uint32_t log_id, int64_t slot)
{
    hy_event_t ev;
    ev.type = type; ev.ts = ts; ev.med_id = med_id; ev.log_id = log_id; ev.slot = slot; ev.value = 0;
    hy_ev_publish(&ev);
}

/* Stable insertion sort by `order`. */
static void sort_meds(hy_med_t *a, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        hy_med_t key = a[i];
        size_t j = i;
        while (j > 0 && a[j - 1].order > key.order) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

static hy_med_t *find_med(uint32_t id)
{
    for (size_t i = 0; i < n_meds; i++)
        if (meds[i].id == id) return &meds[i];
    return NULL;
}

hy_err_t hy_actions_init(const hy_store_port_t *s, void *sctx, const hy_clock_port_t *c, void *cctx)
{
    memset(meds, 0, sizeof meds);
    memset(undo, 0, sizeof undo);
    last_log_ok = false;
    n_snz = 0;
    n_meds = 0;
    next_med_id = 0;
    undo_next = 0;
    inited = false;
    store = NULL; clk = NULL; store_ctx = NULL; clk_ctx = NULL;
    if (!s || !c) return HY_E_INVALID;
    size_t n = 0;
    hy_err_t err = s->load_meds(sctx, meds, HY_MAX_STORED, &n);
    if (err != HY_OK) {
        memset(meds, 0, sizeof meds);
        return HY_E_IO;
    }
    if (n > HY_MAX_STORED) n = HY_MAX_STORED;
    n_meds = n;
    sort_meds(meds, n_meds);
    next_med_id = s->load_next_id ? s->load_next_id(sctx) : 0;
    raise_next_id(meds, n_meds);
    store = s; store_ctx = sctx; clk = c; clk_ctx = cctx;
    inited = true;
    return HY_OK;
}

size_t hy_meds(const hy_med_t **out)
{
    if (out) *out = meds;
    return n_meds;
}

size_t hy_meds_active_count(void)
{
    size_t k = 0;
    for (size_t i = 0; i < n_meds; i++)
        if (!meds[i].archived) k++;
    return k;
}

/* ---- medicines ---- */

hy_err_t hy_act_med_upsert(hy_med_t *m)
{
    if (!inited || !m) return HY_E_INVALID;
    hy_err_t err = hy_med_validate(m, meds, n_meds);
    if (err != HY_OK) return err;

    memcpy(scratch, meds, n_meds * sizeof meds[0]);
    size_t n = n_meds;
    bool drop_snooze = false;
    hy_med_t entry = *m;
    if (m->id == 0) {
        if (n_meds >= HY_MAX_STORED) return HY_E_FULL;
        if (!m->archived && hy_meds_active_count() >= HY_MAX_MEDS) return HY_E_FULL;
        if (next_med_id == 0 || next_med_id == 0xFFFFFFFFu) return HY_E_FULL;
        entry.id = next_med_id;
        scratch[n++] = entry;
    } else {
        size_t i;
        for (i = 0; i < n; i++)
            if (scratch[i].id == m->id) break;
        if (i == n) return HY_E_NOT_FOUND;
        if (scratch[i].archived && !entry.archived && hy_meds_active_count() >= HY_MAX_MEDS)
            return HY_E_FULL;
        /* The caller's copy may be stale: the pill count is owned by this module, unless
         * the caller turns counting on, when its value is the starting count. */
        if (!(m->pills_enabled && !scratch[i].pills_enabled))
            entry.pills_left = scratch[i].pills_left;
        const hy_med_t *old = &scratch[i];
        drop_snooze = entry.archived || entry.type != old->type || entry.n_times != old->n_times ||
                      memcmp(entry.times, old->times, entry.n_times * sizeof entry.times[0]) != 0 ||
                      entry.weekdays != old->weekdays || entry.active_from != old->active_from ||
                      entry.active_until != old->active_until;
        scratch[i] = entry;
    }
    sort_meds(scratch, n);
    uint32_t next_after = m->id == 0 ? entry.id + 1 : next_med_id;
    if (persist_meds(scratch, n, next_after) != HY_OK) return HY_E_IO;

    memcpy(meds, scratch, n * sizeof meds[0]);
    n_meds = n;
    next_med_id = next_after;
    raise_next_id(meds, n_meds);
    if (drop_snooze) snooze_drop(entry.id, 0, NULL);
    m->id = entry.id;
    publish(HY_EV_MEDS_CHANGED, clk->valid(clk_ctx) ? clk->now(clk_ctx) : 0, entry.id, 0, 0);
    return HY_OK;
}

hy_err_t hy_act_pills_set(uint32_t med_id, int16_t left)
{
    if (!inited) return HY_E_INVALID;
    if (left < 0 || left > 9999) return HY_E_INVALID;
    hy_med_t *cur = find_med(med_id);
    if (!cur) return HY_E_NOT_FOUND;
    memcpy(scratch, meds, n_meds * sizeof meds[0]);
    for (size_t i = 0; i < n_meds; i++)
        if (scratch[i].id == med_id) scratch[i].pills_left = left;
    if (persist_meds(scratch, n_meds, next_med_id) != HY_OK) return HY_E_IO;
    cur->pills_left = left;
    publish(HY_EV_MEDS_CHANGED, clk->valid(clk_ctx) ? clk->now(clk_ctx) : 0, med_id, 0, 0);
    return HY_OK;
}

hy_err_t hy_act_med_archive(uint32_t id, bool archived)
{
    if (!inited) return HY_E_INVALID;
    hy_med_t *cur = find_med(id);
    if (!cur) return HY_E_NOT_FOUND;
    hy_med_t upd = *cur;
    upd.archived = archived;
    return hy_act_med_upsert(&upd);
}

/* Removes every log entry of a medicine: the port's purge_med when it has one, entry by entry otherwise. *count
 * gets the number removed (also on a partial failure). The undo records of the medicine are dropped. */
static hy_err_t remove_med_logs(uint32_t med_id, size_t *count)
{
    hy_err_t result = HY_OK;
    size_t removed = 0;
    if (store->purge_med) {
        removed = store->purge_med(store_ctx, med_id);
        /* The port reports a count only: a month that failed to rewrite leaves entries behind. */
        if (store->query_log(store_ctx, 0, PURGE_LOG_END, med_id, qbuf, 1) != 0) result = HY_E_IO;
    } else {
        for (;;) {
            size_t q = store->query_log(store_ctx, 0, PURGE_LOG_END, med_id, qbuf, QUERY_MAX);
            if (q == 0) break;
            for (size_t i = 0; i < q; i++) {
                if (store->delete_log(store_ctx, qbuf[i].id, qbuf[i].ts) != HY_OK) {
                    result = HY_E_IO;
                    break;
                }
                removed++;
            }
            if (result != HY_OK) break;
        }
    }
    for (int i = 0; i < UNDO_SLOTS; i++)
        if (undo[i].used && undo[i].med_id == med_id) undo[i].used = false;
    if (last_log_ok && last_log.med_id == med_id) last_log_ok = false;
    *count = removed;
    return result;
}

size_t hy_act_med_log_count(uint32_t id)
{
    if (!inited) return 0;
    size_t total = 0;
    int64_t from = 0;
    int64_t last_ts = 0;
    uint32_t last_id = 0;
    bool have_cursor = false;
    for (;;) {
        size_t q = store->query_log(store_ctx, from, PURGE_LOG_END, id, qbuf, QUERY_MAX);
        size_t fresh = 0;
        for (size_t i = 0; i < q; i++) {
            bool after = !have_cursor || qbuf[i].ts > last_ts || (qbuf[i].ts == last_ts && qbuf[i].id > last_id);
            if (after) fresh++;
        }
        total += fresh;
        if (q < QUERY_MAX || fresh == 0) break;
        last_ts = qbuf[q - 1].ts;
        last_id = qbuf[q - 1].id;
        have_cursor = true;
        from = last_ts;
    }
    return total;
}

hy_err_t hy_act_med_reorder(const uint32_t *ids, size_t n)
{
    if (!inited || (n && !ids)) return HY_E_INVALID;
    if (n != hy_meds_active_count() || n > HY_MAX_MEDS) return HY_E_INVALID;
    memcpy(scratch, meds, n_meds * sizeof meds[0]);
    bool seen[HY_MAX_STORED] = { false };
    for (size_t k = 0; k < n; k++) {
        size_t i;
        for (i = 0; i < n_meds; i++)
            if (scratch[i].id == ids[k]) break;
        if (i == n_meds || scratch[i].archived || seen[i]) return HY_E_INVALID;
        seen[i] = true;
        scratch[i].order = (uint16_t)k;
    }
    /* archived medicines follow, in their previous order (meds is sorted by order) */
    uint16_t next = (uint16_t)n;
    for (size_t i = 0; i < n_meds; i++)
        if (scratch[i].archived) scratch[i].order = next++;
    sort_meds(scratch, n_meds);
    if (persist_meds(scratch, n_meds, next_med_id) != HY_OK) return HY_E_IO;
    memcpy(meds, scratch, n_meds * sizeof meds[0]);
    publish(HY_EV_MEDS_CHANGED, clk->valid(clk_ctx) ? clk->now(clk_ctx) : 0, 0, 0, 0);
    return HY_OK;
}

hy_err_t hy_act_med_delete(uint32_t id, size_t *n_deleted)
{
    if (n_deleted) *n_deleted = 0;
    if (!inited) return HY_E_INVALID;
    hy_med_t *cur = find_med(id);
    if (!cur) return HY_E_NOT_FOUND;
    if (!cur->archived) return HY_E_INVALID;

    size_t removed = 0;
    hy_err_t r = remove_med_logs(id, &removed);
    if (n_deleted) *n_deleted = removed;
    if (r != HY_OK) return r;
    snooze_drop(id, 0, NULL);

    size_t out = 0;
    for (size_t i = 0; i < n_meds; i++)
        if (meds[i].id != id) scratch[out++] = meds[i];
    uint32_t next = next_med_id;
    if (next < cur->id + 1 && cur->id != 0xFFFFFFFFu) next = cur->id + 1;
    int64_t now = clk->valid(clk_ctx) ? clk->now(clk_ctx) : 0;
    if (persist_meds(scratch, out, next) != HY_OK) {
        /* the log is already gone: views of it must refresh */
        if (removed) publish(HY_EV_LOG_EDITED, now, id, 0, 0);
        return HY_E_IO;
    }
    memcpy(meds, scratch, out * sizeof meds[0]);
    memset(&meds[out], 0, (n_meds - out) * sizeof meds[0]);
    n_meds = out;
    next_med_id = next;
    publish(HY_EV_MEDS_CHANGED, now, id, 0, 0);
    publish(HY_EV_LOG_EDITED, now, id, 0, 0);
    return HY_OK;
}

/* Debug only (the console is the sole caller): removes the medicines with the given ids for good. */
hy_err_t hy_act_med_purge(const uint32_t *ids, size_t n_ids, size_t *n_removed, size_t *n_logs)
{
    size_t removed = 0, logs = 0;
    if (n_removed) *n_removed = 0;
    if (n_logs) *n_logs = 0;
    if (!inited || (n_ids && !ids) || n_ids > HY_MAX_STORED) return HY_E_INVALID;
    hy_err_t result = HY_OK;
    uint32_t done[HY_MAX_STORED];
    size_t kept = 0;
    memcpy(scratch, meds, n_meds * sizeof meds[0]);
    for (size_t k = 0; k < n_ids && result == HY_OK; k++) {
        if (!find_med(ids[k])) continue;
        /* Log entries first: a failure leaves an unchanged medicine list, not orphaned references */
        size_t removed_logs = 0;
        result = remove_med_logs(ids[k], &removed_logs);
        logs += removed_logs;
        if (result == HY_OK) {
            snooze_drop(ids[k], 0, NULL);
            done[removed++] = ids[k];
        }
    }
    /* Drop the medicines whose logs are gone */
    size_t out = 0;
    for (size_t i = 0; i < n_meds; i++) {
        bool drop = false;
        for (size_t k = 0; k < removed; k++)
            if (meds[i].id == done[k]) drop = true;
        if (!drop) scratch[out++] = meds[i];
    }
    kept = out;
    if (removed) {
        if (persist_meds(scratch, kept, next_med_id) != HY_OK) {
            result = HY_E_IO;
        } else {
            memcpy(meds, scratch, kept * sizeof meds[0]);
            memset(&meds[kept], 0, (n_meds - kept) * sizeof meds[0]);
            n_meds = kept;
            publish(HY_EV_MEDS_CHANGED, clk->valid(clk_ctx) ? clk->now(clk_ctx) : 0, 0, 0, 0);
        }
    }
    if (n_removed) *n_removed = removed;
    if (n_logs) *n_logs = logs;
    return result;
}

/* ---- undo records ---- */

static undo_rec_t *undo_find(uint32_t log_id)
{
    for (int i = 0; i < UNDO_SLOTS; i++)
        if (undo[i].used && undo[i].log_id == log_id) return &undo[i];
    return NULL;
}

static void undo_forget(uint32_t log_id)
{
    undo_rec_t *r = undo_find(log_id);
    if (r) r->used = false;
}

static undo_rec_t *undo_add(void)
{
    undo_rec_t *r = &undo[undo_next];
    undo_next = (undo_next + 1) % UNDO_SLOTS;
    memset(r, 0, sizeof *r);
    r->used = true;
    return r;
}

/* ---- snoozes ---- */

/* Drops the snooze of med_id (when slot is not 0, only if it is for that slot). Returns
 * true and fills *out when one was dropped. */
static bool snooze_drop(uint32_t med_id, int64_t slot, hy_snooze_t *out)
{
    for (size_t i = 0; i < n_snz; i++) {
        if (snz[i].med_id == med_id && (slot == 0 || snz[i].slot == slot)) {
            if (out) *out = snz[i];
            snz[i] = snz[--n_snz];
            return true;
        }
    }
    return false;
}

/* Sets the snooze of a medicine, replacing an existing one. */
static hy_err_t snooze_set(const hy_snooze_t *s)
{
    size_t i;
    for (i = 0; i < n_snz; i++)
        if (snz[i].med_id == s->med_id) break;
    if (i == n_snz) {
        if (n_snz >= HY_MAX_MEDS) return HY_E_FULL;
        n_snz++;
    }
    snz[i] = *s;
    return HY_OK;
}

/* The clock port's last zone change, or NULL (*buf is the storage). */
static const hy_tzchange_t *last_change_of(hy_tzchange_t *buf)
{
    return (clk->last_change && clk->last_change(clk_ctx, buf)) ? buf : NULL;
}

/* ---- taken and skipped ---- */

/* Common path of log_taken and skip. A skip passes ts 0 (now) and needs a matched slot. */
static hy_err_t write_entry(uint32_t med_id, hy_status_t status, int64_t ts,
                            const char *note, hy_source_t src, uint32_t *out_log_id)
{
    if (!inited) return HY_E_INVALID;
    if (!clk->valid(clk_ctx)) return HY_E_CLOCK;
    hy_med_t *med = find_med(med_id);
    if (!med || med->archived) return HY_E_NOT_FOUND;

    int64_t now = clk->now(clk_ctx);
    if (ts == 0) ts = now;
    /* Copied once: the port's pointer may be replaced by a zone change in another task. */
    char tz[HY_TZ_LEN];
    const char *ptz = clk->tz(clk_ctx);
    strncpy(tz, ptz ? ptz : "UTC0", sizeof tz - 1);
    tz[sizeof tz - 1] = '\0';

    /* As-needed medicines have no slots: hy_match_slot gives 0, so the log is not queried
     * (a long history could otherwise hit the query cap and block logging). */
    size_t n = 0;
    int64_t slot = 0;
    if (med->type == HY_SCHED_FIXED) {
        /* Entries are indexed by their own ts, and a backdated ts can sit before entries
         * written later (a taken log at 09:30, an auto missed entry at slot + 120 min),
         * so the upper bound also covers `now`. */
        int64_t hi = ts + (int64_t)med->early_window_min * 60;
        if (now > hi) hi = now;
        n = store->query_log(store_ctx, ts - LOOKBACK_S, hi + 1, med_id, qbuf, QUERY_MAX);
        if (n >= QUERY_MAX) return HY_E_FULL; /* possibly truncated: do not match on partial data */
        hy_tzchange_t chbuf;
        slot = hy_match_slot(med, ts, tz, qbuf, n, last_change_of(&chbuf));
    }
    if (status == HY_SKIPPED && slot == 0) return HY_E_INVALID;

    hy_log_t *missed = NULL;
    if (slot != 0) {
        for (size_t i = 0; i < n; i++) {
            if (qbuf[i].med_id == med_id && qbuf[i].slot == slot && qbuf[i].status == HY_MISSED) {
                missed = &qbuf[i];
                break;
            }
        }
    }

    hy_log_t e;
    undo_rec_t rec;
    memset(&rec, 0, sizeof rec);
    if (missed) {
        rec.converted = true;
        rec.original = *missed;
        e = *missed;
    } else {
        memset(&e, 0, sizeof e);
        e.med_id = med_id;
    }
    e.ts = ts;
    memcpy(e.tz, tz, sizeof e.tz);
    e.status = status;
    e.slot = slot;
    e.delta_min = slot ? (int32_t)((ts - slot) / 60) : 0;
    memset(e.note, 0, sizeof e.note);
    if (note) strncpy(e.note, note, sizeof e.note - 1);
    e.source = src;

    hy_err_t err = missed ? store->update_log(store_ctx, &e) : store->append_log(store_ctx, &e);
    if (err != HY_OK) return HY_E_IO;

    if (status == HY_TAKEN && med->pills_enabled && med->pills_left > 0) {
        med->pills_left--;
        /* The log write is the commit point: a failed save is not reported. The
         * decremented count stays in memory and the next successful meds save persists it. */
        (void)persist_meds(meds, n_meds, next_med_id);
        rec.pill_dec = true;
    }

    if (slot != 0) rec.had_snooze = snooze_drop(med_id, slot, &rec.snooze);

    undo_rec_t *r = undo_add();
    *r = rec;
    r->used = true;
    r->log_id = e.id;
    r->med_id = med_id;
    r->ts = ts;
    r->slot = slot;
    r->created = now;

    if (out_log_id) *out_log_id = e.id;
    last_log_set(&e);
    publish(status == HY_TAKEN ? HY_EV_TAKEN : HY_EV_SKIPPED, ts, med_id, e.id, slot);
    return HY_OK;
}

hy_err_t hy_act_log_taken(uint32_t med_id, int64_t ts, const char *note,
                          hy_source_t src, uint32_t *out_log_id)
{
    return write_entry(med_id, HY_TAKEN, ts, note, src, out_log_id);
}

hy_err_t hy_act_skip(uint32_t med_id, hy_source_t src, uint32_t *out_log_id)
{
    return write_entry(med_id, HY_SKIPPED, 0, NULL, src, out_log_id);
}

/* Copies the clock port's zone once (see write_entry). */
static void copy_tz(char tz[HY_TZ_LEN])
{
    const char *ptz = clk->tz(clk_ctx);
    strncpy(tz, ptz ? ptz : "UTC0", HY_TZ_LEN - 1);
    tz[HY_TZ_LEN - 1] = '\0';
}

hy_err_t hy_act_mark_missed(uint32_t med_id, int64_t slot, uint32_t *out_log_id)
{
    if (!inited) return HY_E_INVALID;
    if (!clk->valid(clk_ctx)) return HY_E_CLOCK;
    hy_med_t *med = find_med(med_id);
    if (!med || med->archived || med->type != HY_SCHED_FIXED) return HY_E_NOT_FOUND;

    int64_t now = clk->now(clk_ctx);
    char tz[HY_TZ_LEN];
    copy_tz(tz);

    int64_t occ[1];
    if (hy_occurrences(med, slot, slot + 1, tz, occ, 1) != 1 || occ[0] != slot) return HY_E_INVALID;

    /* Any entry for the slot (taken, skipped or missed) means nothing to do. Entries are
     * indexed by their own ts, which can be backdated, so the range reaches back 8 days
     * and forward to the later of now and the end of the early window. */
    int64_t hi = slot + (int64_t)med->early_window_min * 60;
    if (now > hi) hi = now;
    size_t n = store->query_log(store_ctx, slot - LOOKBACK_S, hi + 1, med_id, qbuf, QUERY_MAX);
    if (n >= QUERY_MAX) return HY_E_FULL;
    for (size_t i = 0; i < n; i++) {
        if (qbuf[i].med_id == med_id && qbuf[i].slot == slot) {
            if (out_log_id) *out_log_id = 0;
            return HY_OK;
        }
    }

    hy_log_t e;
    memset(&e, 0, sizeof e);
    e.med_id = med_id;
    e.ts = now;
    memcpy(e.tz, tz, sizeof e.tz);
    e.status = HY_MISSED;
    e.slot = slot;
    e.delta_min = (int32_t)((now - slot) / 60);
    e.source = HY_SRC_AUTO;
    if (store->append_log(store_ctx, &e) != HY_OK) return HY_E_IO;

    snooze_drop(med_id, slot, NULL);
    if (out_log_id) *out_log_id = e.id;
    last_log_set(&e);
    publish(HY_EV_MISSED, now, med_id, e.id, slot);
    return HY_OK;
}

hy_err_t hy_act_snooze(uint32_t med_id, uint16_t minutes, hy_source_t src, int64_t *out_until)
{
    (void)src;
    if (!inited) return HY_E_INVALID;
    if (!clk->valid(clk_ctx)) return HY_E_CLOCK;
    hy_med_t *med = find_med(med_id);
    if (!med || med->archived || med->type != HY_SCHED_FIXED) return HY_E_NOT_FOUND;
    if (minutes < 1 || minutes > 120) return HY_E_INVALID;

    int64_t now = clk->now(clk_ctx);
    char tz[HY_TZ_LEN];
    copy_tz(tz);

    int64_t hi = now + (int64_t)med->early_window_min * 60;
    size_t n = store->query_log(store_ctx, now - LOOKBACK_S, hi + 1, med_id, qbuf, QUERY_MAX);
    if (n >= QUERY_MAX) return HY_E_FULL;
    hy_tzchange_t ch;
    const hy_tzchange_t *lc = last_change_of(&ch);
    int64_t slot = hy_match_slot(med, now, tz, qbuf, n, lc);
    if (slot == 0) return HY_E_INVALID;

    int64_t base = hy_occ_base(med, slot, tz, lc);
    int64_t deadline = base + (int64_t)med->missed_after_min * 60;
    if (now < base || now >= deadline) return HY_E_INVALID;

    int64_t until = now + (int64_t)minutes * 60;
    if (until > deadline) until = deadline;

    hy_snooze_t ns = { med_id, slot, until };
    hy_err_t serr = snooze_set(&ns);
    if (serr != HY_OK) return serr;

    if (out_until) *out_until = until;
    publish(HY_EV_SNOOZED, now, med_id, 0, slot);
    return HY_OK;
}

size_t hy_snoozes(const hy_snooze_t **out)
{
    size_t k = 0;
    if (inited && clk->valid(clk_ctx)) {
        int64_t now = clk->now(clk_ctx);
        for (size_t i = 0; i < n_snz; i++)
            if (snz[i].until > now) {
                const hy_med_t *med = find_med(snz[i].med_id);
                if (med && !med->archived) snz_out[k++] = snz[i];
            }
    }
    if (out) *out = snz_out;
    return k;
}

hy_err_t hy_act_undo(uint32_t log_id)
{
    if (!inited) return HY_E_INVALID;
    undo_rec_t *r = undo_find(log_id);
    if (!r) return HY_E_NOT_FOUND;
    if (!clk->valid(clk_ctx)) return HY_E_CLOCK;
    int64_t now = clk->now(clk_ctx);
    if (now < r->created || now - r->created > HY_UNDO_WINDOW_S) {
        r->used = false;
        return HY_E_UNDO_EXPIRED;
    }

    hy_err_t err = r->converted ? store->update_log(store_ctx, &r->original)
                                : store->delete_log(store_ctx, r->log_id, r->ts);
    if (err != HY_OK) return HY_E_IO; /* record kept, retry possible */

    uint32_t med_id = r->med_id;
    int64_t ts = r->ts;
    int64_t slot = r->slot;
    bool dec = r->pill_dec;
    bool had_snooze = r->had_snooze;
    hy_snooze_t snooze = r->snooze;
    bool converted = r->converted;
    hy_log_t original = r->original;
    r->used = false;
    if (converted) last_log_set(&original);
    else last_log_clear(log_id);

    if (had_snooze && snooze.until > now) (void)snooze_set(&snooze);

    if (dec) {
        hy_med_t *med = find_med(med_id);
        if (med) {
            /* The log undo is the commit point: a failed pill save is not reported;
             * the restored count is persisted by the next successful meds save. */
            med->pills_left++;
            (void)persist_meds(meds, n_meds, next_med_id);
        }
    }
    publish(HY_EV_UNDONE, ts, med_id, log_id, slot);
    return HY_OK;
}

/* ---- log edits ---- */

hy_err_t hy_act_log_edit(const hy_log_t *e)
{
    if (!inited || !e) return HY_E_INVALID;
    hy_log_t upd = *e;
    upd.edited = true;
    hy_err_t ue = store->update_log(store_ctx, &upd);
    if (ue != HY_OK) return (ue == HY_E_INVALID || ue == HY_E_FULL) ? ue : HY_E_IO;
    undo_forget(upd.id);
    last_log_set(&upd);
    publish(HY_EV_LOG_EDITED, upd.ts, upd.med_id, upd.id, upd.slot);
    return HY_OK;
}

hy_err_t hy_act_log_update(uint32_t id, const hy_api_logreq_t *req)
{
    if (!inited || !req || req->orig_ts == 0) return HY_E_INVALID;
    /* One second wide: a month-wide query would be cut at the cap. */
    size_t q = store->query_log(store_ctx, req->orig_ts, req->orig_ts + 1, 0, qbuf, QUERY_MAX);
    hy_log_t e;
    bool found = false;
    for (size_t i = 0; i < q; i++) {
        if (qbuf[i].id == id) {
            e = qbuf[i];
            found = true;
            break;
        }
    }
    if (!found) return HY_E_NOT_FOUND;

    bool clock_ok = clk->valid(clk_ctx);
    int64_t now = clock_ok ? clk->now(clk_ctx) : 0;
    bool retime = false;
    bool was_missed = e.status == HY_MISSED;
    if (req->has_ts) {
        if (req->ts <= 0 || req->ts > LOG_TS_MAX) return HY_E_INVALID;
        if (clock_ok && req->ts > now + 60) return HY_E_INVALID;
        if (req->ts != e.ts) retime = true;
        e.ts = req->ts;
    }
    if (req->has_note) {
        if (memchr(req->note, '\0', sizeof req->note) == NULL) return HY_E_INVALID;
        memset(e.note, 0, sizeof e.note);
        strncpy(e.note, req->note, sizeof e.note - 1);
    }
    if (req->has_status) {
        if ((unsigned)req->status > (unsigned)HY_MISSED) return HY_E_INVALID;
        e.status = req->status;
    }
    if (req->has_med) {
        if (!find_med(req->med_id)) return HY_E_NOT_FOUND;
        if (req->med_id != e.med_id) retime = true;
        e.med_id = req->med_id;
    }

    hy_med_t *med = find_med(e.med_id);
    if (was_missed && e.status != HY_MISSED) retime = true;   /* a missed entry becoming a dose gets its slot */
    if (retime && med && med->type == HY_SCHED_FIXED && (e.status == HY_TAKEN || e.status == HY_SKIPPED)) {
        int64_t hi = e.ts + (int64_t)med->early_window_min * 60;
        if (clock_ok && now > hi) hi = now;
        size_t n = store->query_log(store_ctx, e.ts - LOOKBACK_S, hi + 1, e.med_id, qbuf, QUERY_MAX);
        if (n >= QUERY_MAX) return HY_E_FULL;
        size_t w = 0;
        for (size_t i = 0; i < n; i++)
            if (qbuf[i].id != id) qbuf[w++] = qbuf[i];
        hy_tzchange_t chbuf;
        e.slot = hy_match_slot(med, e.ts, e.tz, qbuf, w, last_change_of(&chbuf));
        e.delta_min = e.slot ? (int32_t)((e.ts - e.slot) / 60) : 0;
    } else if (retime && (!med || med->type != HY_SCHED_FIXED)) {
        e.slot = 0;
        e.delta_min = 0;
    }
    return hy_act_log_edit(&e);
}

hy_err_t hy_act_log_delete(uint32_t log_id, int64_t ts)
{
    if (!inited) return HY_E_INVALID;
    if (store->delete_log(store_ctx, log_id, ts) != HY_OK) return HY_E_IO;
    undo_forget(log_id);
    last_log_clear(log_id);
    publish(HY_EV_LOG_EDITED, ts, 0, log_id, 0);
    return HY_OK;
}

hy_err_t hy_act_record_gap(const hy_gap_t *g, hy_gap_reason_t reason)
{
    if (!inited || !g || !store->append_gap || g->end <= g->start) return HY_E_INVALID;
    if ((unsigned)reason > (unsigned)HY_GAP_JUMP) return HY_E_INVALID;
    if (store->append_gap(store_ctx, g, (uint8_t)reason) != HY_OK) return HY_E_IO;
    hy_event_t ev;
    ev.type = HY_EV_UNTRACKED; ev.ts = g->end; ev.med_id = 0; ev.log_id = 0;
    ev.slot = g->start; ev.value = (int32_t)reason;
    hy_ev_publish(&ev);
    return HY_OK;
}

hy_err_t hy_act_log_import(const hy_log_t *e)
{
    if (!inited || !e) return HY_E_INVALID;
    if ((unsigned)e->status > (unsigned)HY_MISSED || (unsigned)e->source > (unsigned)HY_SRC_AUTO) return HY_E_INVALID;
    if (!find_med(e->med_id)) return HY_E_NOT_FOUND;
    hy_log_t copy = *e;
    copy.id = 0;
    hy_err_t r = store->append_log(store_ctx, &copy);
    if (r == HY_OK) return HY_OK;
    return (r == HY_E_FULL || r == HY_E_INVALID) ? r : HY_E_IO;
}

void hy_act_log_import_end(void)
{
    if (!inited) return;
    publish(HY_EV_LOG_EDITED, clk->valid(clk_ctx) ? clk->now(clk_ctx) : 0, 0, 0, 0);
}
