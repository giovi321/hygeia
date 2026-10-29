#include <string.h>
#include "hy/actions.h"
#include "hy/events.h"
#include "hy/med.h"
#include "hy/occ.h"

#define UNDO_SLOTS 8
#define QUERY_MAX 128
#define LOOKBACK_S (8 * 86400LL)

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

static hy_med_t meds[HY_MAX_MEDS];
static hy_med_t scratch[HY_MAX_MEDS];
static size_t n_meds;
static undo_rec_t undo[UNDO_SLOTS];
static size_t undo_next;
static bool inited;
static hy_log_t qbuf[QUERY_MAX];
static hy_snooze_t snz[HY_MAX_MEDS];
static size_t n_snz;
static hy_snooze_t snz_out[HY_MAX_MEDS];
static bool snooze_drop(uint32_t med_id, int64_t slot, hy_snooze_t *out);

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
    n_snz = 0;
    n_meds = 0;
    undo_next = 0;
    inited = false;
    store = NULL; clk = NULL; store_ctx = NULL; clk_ctx = NULL;
    if (!s || !c) return HY_E_INVALID;
    size_t n = 0;
    hy_err_t err = s->load_meds(sctx, meds, HY_MAX_MEDS, &n);
    if (err != HY_OK) {
        memset(meds, 0, sizeof meds);
        return HY_E_IO;
    }
    if (n > HY_MAX_MEDS) n = HY_MAX_MEDS;
    n_meds = n;
    sort_meds(meds, n_meds);
    store = s; store_ctx = sctx; clk = c; clk_ctx = cctx;
    inited = true;
    return HY_OK;
}

size_t hy_meds(const hy_med_t **out)
{
    if (out) *out = meds;
    return n_meds;
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
        if (n_meds >= HY_MAX_MEDS) return HY_E_FULL;
        uint32_t max_id = 0;
        for (size_t i = 0; i < n_meds; i++)
            if (meds[i].id > max_id) max_id = meds[i].id;
        entry.id = max_id + 1;
        scratch[n++] = entry;
    } else {
        size_t i;
        for (i = 0; i < n; i++)
            if (scratch[i].id == m->id) break;
        if (i == n) return HY_E_NOT_FOUND;
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
    if (store->save_meds(store_ctx, scratch, n) != HY_OK) return HY_E_IO;

    memcpy(meds, scratch, n * sizeof meds[0]);
    n_meds = n;
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
    if (store->save_meds(store_ctx, scratch, n_meds) != HY_OK) return HY_E_IO;
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
        (void)store->save_meds(store_ctx, meds, n_meds);
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
    r->used = false;

    if (had_snooze && snooze.until > now) (void)snooze_set(&snooze);

    if (dec) {
        hy_med_t *med = find_med(med_id);
        if (med) {
            /* The log undo is the commit point: a failed pill save is not reported;
             * the restored count is persisted by the next successful meds save. */
            med->pills_left++;
            (void)store->save_meds(store_ctx, meds, n_meds);
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
    if (store->update_log(store_ctx, &upd) != HY_OK) return HY_E_IO;
    undo_forget(upd.id);
    publish(HY_EV_LOG_EDITED, upd.ts, upd.med_id, upd.id, upd.slot);
    return HY_OK;
}

hy_err_t hy_act_log_delete(uint32_t log_id, int64_t ts)
{
    if (!inited) return HY_E_INVALID;
    if (store->delete_log(store_ctx, log_id, ts) != HY_OK) return HY_E_IO;
    undo_forget(log_id);
    publish(HY_EV_LOG_EDITED, ts, 0, log_id, 0);
    return HY_OK;
}
