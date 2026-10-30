#include <string.h>
#include "hy/sched.h"

#define SCHED_WINDOW_S (8 * 86400LL)  /* look-back and look-ahead span, covers a weekly schedule */
#define SCHED_OCC_MAX  96             /* HY_MAX_TIMES per day over 9 calendar days, plus slack */

static bool tracked_med(const hy_med_t *m)
{
    return m->type == HY_SCHED_FIXED && !m->archived;
}

static int seen_index(const hy_sched_t *s, uint32_t med_id)
{
    for (size_t i = 0; i < s->n_seen; i++)
        if (s->seen[i].med_id == med_id) return (int)i;
    return -1;
}

static bool med_listed(const hy_med_t *meds, size_t n_meds, uint32_t med_id)
{
    for (size_t i = 0; i < n_meds; i++)
        if (meds[i].id == med_id && tracked_med(&meds[i])) return true;
    return false;
}

/* FNV-1a over what decides when a medicine is due: fixed times, weekdays, active dates. */
static void sig_mix(uint32_t *h, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        *h ^= (v >> (8 * i)) & 0xFFu;
        *h *= 16777619u;
    }
}

static uint32_t sched_sig(const hy_med_t *m)
{
    uint32_t h = 2166136261u;
    uint8_t n = m->n_times > HY_MAX_TIMES ? HY_MAX_TIMES : m->n_times;
    sig_mix(&h, n);
    for (uint8_t i = 0; i < n; i++) sig_mix(&h, m->times[i]);
    sig_mix(&h, m->weekdays);
    sig_mix(&h, (uint32_t)m->active_from);
    sig_mix(&h, (uint32_t)m->active_until);
    return h;
}

/* Drops entries for medicines that are no longer eligible, then adds new ones and
 * restarts since = now for those whose schedule signature changed. */
static void seen_update(hy_sched_t *s, const hy_med_t *meds, size_t n_meds, int64_t now)
{
    size_t w = 0;
    for (size_t r = 0; r < s->n_seen; r++)
        if (med_listed(meds, n_meds, s->seen[r].med_id)) s->seen[w++] = s->seen[r];
    s->n_seen = w;

    for (size_t i = 0; i < n_meds; i++) {
        const hy_med_t *m = &meds[i];
        if (!tracked_med(m)) continue;
        uint32_t sig = sched_sig(m);
        int k = seen_index(s, m->id);
        if (k >= 0) {
            if (s->seen[k].sig != sig) {
                s->seen[k].since = now;
                s->seen[k].sig = sig;
            }
        } else if (s->n_seen < HY_MAX_MEDS) {
            s->seen[s->n_seen].med_id = m->id;
            s->seen[s->n_seen].since = now;
            s->seen[s->n_seen].sig = sig;
            s->n_seen++;
        }
    }
}

static bool any_log_for(const hy_sched_in_t *in, uint32_t med_id, int64_t slot)
{
    for (size_t i = 0; i < in->n_logs; i++)
        if (in->logs[i].med_id == med_id && in->logs[i].slot == slot) return true;
    return false;
}

static bool ref_in(const hy_slot_ref_t *l, size_t n, uint32_t med_id, int64_t slot)
{
    for (size_t i = 0; i < n; i++)
        if (l[i].med_id == med_id && l[i].slot == slot) return true;
    return false;
}

static void wake_min(int64_t *wake, int64_t v, int64_t now)
{
    if (v > now && (*wake == 0 || v < *wake)) *wake = v;
}

void hy_sched_init(hy_sched_t *s)
{
    if (s) memset(s, 0, sizeof *s);
}

void hy_sched_start_tracking(hy_sched_t *s, int64_t now)
{
    if (!s) return;
    s->tracking_since = now;
    s->n_seen = 0; /* a jump in either direction restarts every medicine at the next eval */
}

void hy_sched_stop_tracking(hy_sched_t *s)
{
    if (s) s->tracking_since = 0;
}

int64_t hy_sched_tracked_since(const hy_sched_t *s, uint32_t med_id)
{
    if (!s || s->tracking_since == 0) return 0;
    int i = seen_index(s, med_id);
    if (i < 0) return s->tracking_since;
    return s->seen[i].since > s->tracking_since ? s->seen[i].since : s->tracking_since;
}

typedef struct { hy_open_t o; uint16_t order; size_t idx; } cand_t;

static bool cand_before(const cand_t *a, const cand_t *b)
{
    if (a->o.base != b->o.base) return a->o.base < b->o.base;
    if (a->order != b->order) return a->order < b->order;
    return a->idx < b->idx;
}

void hy_sched_eval(hy_sched_t *s, const hy_sched_in_t *in, hy_sched_out_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!s || !in || s->tracking_since == 0) return;

    const int64_t now = in->now;
    const char *tz = in->tz ? in->tz : "UTC0";
    const hy_med_t *meds = in->meds;
    const size_t n_meds = meds ? in->n_meds : 0;
    int64_t wake = 0;

    static HY_EXT_BSS cand_t cand[HY_MAX_MEDS];
    size_t n_cand = 0;
    static int64_t occ[SCHED_OCC_MAX];

    seen_update(s, meds, n_meds, now);

    for (size_t i = 0; i < n_meds; i++) {
        const hy_med_t *m = &meds[i];
        if (!tracked_med(m)) continue;

        hy_gap_t gap = { 0, hy_sched_tracked_since(s, m->id) };
        size_t n = hy_occurrences(m, now - SCHED_WINDOW_S, now + 1, tz, occ, SCHED_OCC_MAX);
        bool have_open = false;
        for (size_t k = 0; k < n; k++) {
            int64_t slot = occ[k];
            int64_t next = k + 1 < n ? occ[k + 1] : 0;
            hy_occ_state_t st = hy_occ_state(m, slot, next, now, tz, in->logs, in->n_logs,
                                             &gap, 1, in->last_change);
            if ((st == HY_OCC_DUE || st == HY_OCC_LATE) && !have_open && n_cand < HY_MAX_MEDS) {
                cand_t *c = &cand[n_cand++];
                memset(c, 0, sizeof *c);
                c->o.med_id = m->id;
                c->o.slot = slot;
                c->o.base = hy_occ_base(m, slot, tz, in->last_change);
                c->o.state = st;
                c->order = m->order;
                c->idx = i;
                have_open = true;
            } else if (st == HY_OCC_MISSED && !any_log_for(in, m->id, slot) &&
                       out->n_to_miss < HY_MAX_MEDS) {
                out->to_miss[out->n_to_miss].med_id = m->id;
                out->to_miss[out->n_to_miss].slot = slot;
                out->n_to_miss++;
            }
        }

        int64_t nx;
        if (hy_occurrences(m, now + 1, now + 1 + SCHED_WINDOW_S, tz, &nx, 1) == 1)
            wake_min(&wake, hy_occ_base(m, nx, tz, in->last_change), now);
    }

    /* Insertion sort: base, then medicine order, then input position. */
    for (size_t i = 1; i < n_cand; i++) {
        cand_t c = cand[i];
        size_t j = i;
        while (j > 0 && cand_before(&c, &cand[j - 1])) {
            cand[j] = cand[j - 1];
            j--;
        }
        cand[j] = c;
    }

    hy_slot_ref_t showing[HY_SCHED_MAX_OPEN];
    size_t n_showing = 0;
    for (size_t i = 0; i < n_cand && out->n_open < HY_SCHED_MAX_OPEN; i++) {
        hy_open_t *o = &out->open[out->n_open++];
        *o = cand[i].o;
        for (size_t k = 0; in->snoozes && k < in->n_snoozes; k++) {
            const hy_snooze_t *z = &in->snoozes[k];
            if (z->med_id == o->med_id && z->slot == o->slot && z->until > now) {
                o->snoozed_until = z->until;
                break;
            }
        }
        if (o->snoozed_until == 0) {
            showing[n_showing].med_id = o->med_id;
            showing[n_showing].slot = o->slot;
            n_showing++;
        }

        const hy_med_t *m = &meds[cand[i].idx];
        wake_min(&wake, o->base + (int64_t)m->late_after_min * 60, now);
        wake_min(&wake, o->base + (int64_t)m->missed_after_min * 60, now);

        if (!ref_in(s->announced, sizeof s->announced / sizeof s->announced[0], o->med_id, o->slot)) {
            out->newly_due[out->n_newly_due].med_id = o->med_id;
            out->newly_due[out->n_newly_due].slot = o->slot;
            out->n_newly_due++;
            size_t r = s->announced_next % (sizeof s->announced / sizeof s->announced[0]);
            s->announced[r].med_id = o->med_id;
            s->announced[r].slot = o->slot;
            s->announced_next = (r + 1) % (sizeof s->announced / sizeof s->announced[0]);
        }
    }
    out->n_showing = n_showing;

    bool new_pair = false;
    for (size_t i = 0; i < n_showing; i++)
        if (!ref_in(s->showing, s->n_showing_prev, showing[i].med_id, showing[i].slot)) new_pair = true;
    if (new_pair) {
        out->chime = true;
        s->chime_plays = 1;
        s->chime_next = now + HY_CHIME_REPEAT_S;
    } else if (n_showing > 0 && s->chime_plays < HY_CHIME_MAX_PLAYS && now >= s->chime_next) {
        out->chime = true;
        s->chime_plays++;
        s->chime_next = now + HY_CHIME_REPEAT_S;
    }
    if (n_showing == 0) {
        s->chime_plays = 0;
        s->chime_next = 0;
    }
    memcpy(s->showing, showing, n_showing * sizeof showing[0]);
    s->n_showing_prev = n_showing;

    for (size_t k = 0; in->snoozes && k < in->n_snoozes; k++)
        wake_min(&wake, in->snoozes[k].until, now);
    if (s->chime_plays > 0 && s->chime_plays < HY_CHIME_MAX_PLAYS)
        wake_min(&wake, s->chime_next, now);

    out->next_wake = wake;
    s->last_eval = now;
}
