#include <string.h>
#include "unity.h"
#include "hy/actions.h"
#include "hy/events.h"
#include "hy/gap.h"
#include "hy/med.h"
#include "hy/occ.h"

#define D0 1793145600LL /* 2026-10-28 00:00 UTC */
#define H(h, m) ((int64_t)((h) * 3600 + (m) * 60))

/* ---- in-memory store port that records calls ---- */
#define MAXLOG 200
static struct {
    hy_med_t meds[HY_MAX_STORED]; size_t n_meds;
    hy_log_t logs[MAXLOG]; size_t n_logs; uint32_t next_id;
    int saves, appends, append_attempts, updates, deletes, loads;
    bool fail_load, fail_save, fail_append, fail_update, fail_delete;
    hy_gap_t gaps[8]; uint8_t gap_reasons[8]; int n_gaps;
} st;

static hy_err_t s_load(void *c, hy_med_t *out, size_t max, size_t *n)
{
    (void)c; st.loads++;
    if (st.fail_load) return HY_E_IO;
    *n = st.n_meds < max ? st.n_meds : max;
    memcpy(out, st.meds, *n * sizeof *out);
    return HY_OK;
}
static hy_err_t s_save(void *c, const hy_med_t *m, size_t n)
{
    (void)c; st.saves++;
    if (st.fail_save) return HY_E_IO;
    memcpy(st.meds, m, n * sizeof *m); st.n_meds = n;
    return HY_OK;
}
static hy_err_t s_append(void *c, hy_log_t *e)
{
    (void)c; st.append_attempts++;
    if (st.fail_append) return HY_E_IO;
    e->id = st.next_id++;
    st.logs[st.n_logs++] = *e; st.appends++;
    return HY_OK;
}
static hy_err_t s_update(void *c, const hy_log_t *e)
{
    (void)c; st.updates++;
    if (st.fail_update) return HY_E_IO;
    for (size_t i = 0; i < st.n_logs; i++)
        if (st.logs[i].id == e->id) { st.logs[i] = *e; return HY_OK; }
    return HY_E_NOT_FOUND;
}
static hy_err_t s_delete(void *c, uint32_t id, int64_t ts)
{
    (void)c; (void)ts; st.deletes++;
    if (st.fail_delete) return HY_E_IO;
    for (size_t i = 0; i < st.n_logs; i++)
        if (st.logs[i].id == id) {
            st.logs[i] = st.logs[--st.n_logs];
            return HY_OK;
        }
    return HY_E_NOT_FOUND;
}
static size_t s_query(void *c, int64_t from, int64_t to, uint32_t med_id, hy_log_t *out, size_t max)
{
    (void)c; size_t n = 0;
    for (size_t i = 0; i < st.n_logs && n < max; i++)
        if (st.logs[i].ts >= from && st.logs[i].ts < to && (!med_id || st.logs[i].med_id == med_id))
            out[n++] = st.logs[i];
    return n;
}
static hy_err_t s_append_gap(void *c, const hy_gap_t *g, uint8_t reason)
{
    (void)c;
    if (st.n_gaps < 8) { st.gaps[st.n_gaps] = *g; st.gap_reasons[st.n_gaps] = reason; }
    st.n_gaps++;
    return HY_OK;
}
static size_t s_query_gaps(void *c, int64_t from, int64_t to, hy_gap_t *out, uint8_t *reasons, size_t max)
{
    (void)c; (void)from; (void)to; (void)out; (void)reasons; (void)max;
    return 0;
}
static const hy_store_port_t store_port = { s_load, s_save, s_append, s_update, s_delete, s_query,
                                            s_append_gap, s_query_gaps };

/* ---- clock port ---- */
static struct { int64_t now; bool valid; } clk;
static int64_t c_now(void *c) { (void)c; return clk.now; }
static const char *c_tz(void *c) { (void)c; return "UTC0"; }
static bool c_valid(void *c) { (void)c; return clk.valid; }
static const hy_clock_port_t clock_port = { c_now, c_tz, c_valid, NULL };
/* Zurich clock with a recorded change from New York (plan-1 R6 setup, 2026-07-15). */
#define TZ_ZURICH "CET-1CEST,M3.5.0,M10.5.0/3"
#define TZ_NY     "EST5EDT,M3.2.0,M11.1.0"
#define T_R6_0600Z 1784095200LL /* 08:00 CEST Zurich */
#define T_R6_1000Z 1784109600LL /* change instant */
static const char *cz_tz(void *c) { (void)c; return TZ_ZURICH; }
static bool cz_last(void *c, hy_tzchange_t *out)
{
    (void)c;
    memset(out, 0, sizeof *out);
    out->at = T_R6_1000Z;
    strcpy(out->old_tz, TZ_NY);
    strcpy(out->new_tz, TZ_ZURICH);
    return true;
}
static const hy_clock_port_t clock_port_zurich = { c_now, cz_tz, c_valid, cz_last };

/* ---- event recorder ---- */
static hy_event_t evs[32]; static int n_evs; static int sub;
static void on_ev(const hy_event_t *e, void *ctx) { (void)ctx; if (n_evs < 32) evs[n_evs++] = *e; }

static void fixed_med(hy_med_t *m, uint32_t id, const char *code, const char *name, uint16_t order)
{
    memset(m, 0, sizeof *m);
    hy_med_defaults(m);
    m->id = id; m->order = order;
    strcpy(m->code, code); strcpy(m->name, name);
    m->type = HY_SCHED_FIXED; m->times[0] = 8 * 60; m->n_times = 1;
}

/* ids: A = 1, V = 2 (pills 12), M = 3 */
void setUp(void)
{
    memset(&st, 0, sizeof st);
    st.next_id = 100;
    fixed_med(&st.meds[0], 1, "A", "Antibiotic", 0);
    fixed_med(&st.meds[1], 2, "V", "Vitamin D", 1);
    st.meds[1].pills_enabled = true; st.meds[1].pills_left = 12; st.meds[1].pills_alert = 3;
    fixed_med(&st.meds[2], 3, "M", "Magnesium", 2);
    st.n_meds = 3;
    clk.now = D0 + H(8, 5); clk.valid = true;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &clock_port, NULL));
    n_evs = 0;
    sub = hy_ev_subscribe(on_ev, NULL);
}
void tearDown(void) { hy_ev_unsubscribe(sub); }

static const hy_med_t *med_by_id(uint32_t id)
{
    const hy_med_t *m; size_t n = hy_meds(&m);
    for (size_t i = 0; i < n; i++) if (m[i].id == id) return &m[i];
    return NULL;
}

static void test_events_subscribe_unsubscribe(void)
{
    int h[HY_EV_MAX_SUBSCRIBERS]; int extra;
    for (int i = 0; i < HY_EV_MAX_SUBSCRIBERS - 1; i++) { /* sub holds one slot */
        h[i] = hy_ev_subscribe(on_ev, NULL);
        TEST_ASSERT_TRUE(h[i] >= 0);
    }
    TEST_ASSERT_EQUAL_INT(-1, hy_ev_subscribe(on_ev, NULL));
    hy_ev_unsubscribe(h[3]);
    extra = hy_ev_subscribe(on_ev, NULL);
    TEST_ASSERT_EQUAL_INT(h[3], extra);
    hy_event_t e = { HY_EV_DUE, 1, 2, 3, 0, 0 };
    n_evs = 0;
    hy_ev_publish(&e);
    TEST_ASSERT_EQUAL_INT(HY_EV_MAX_SUBSCRIBERS, n_evs);
    hy_ev_unsubscribe(extra);
    for (int i = 0; i < HY_EV_MAX_SUBSCRIBERS - 1; i++) if (i != 3) hy_ev_unsubscribe(h[i]);
    n_evs = 0;
    hy_ev_publish(&e);
    TEST_ASSERT_EQUAL_INT(1, n_evs);
}

static void test_log_taken_writes_and_publishes(void)
{
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, "with food", HY_SRC_SCREEN, &id));
    TEST_ASSERT_EQUAL_INT(1, st.appends);
    TEST_ASSERT_EQUAL_UINT32(100, id);
    hy_log_t *e = &st.logs[0];
    TEST_ASSERT_EQUAL_STRING("UTC0", e->tz);
    TEST_ASSERT_EQUAL_INT(HY_SRC_SCREEN, e->source);
    TEST_ASSERT_EQUAL_INT(HY_TAKEN, e->status);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), e->slot);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 5), e->ts);
    TEST_ASSERT_EQUAL_INT32(5, e->delta_min);
    TEST_ASSERT_EQUAL_STRING("with food", e->note);
    TEST_ASSERT_FALSE(e->edited);
    TEST_ASSERT_EQUAL_INT(1, n_evs);
    TEST_ASSERT_EQUAL_INT(HY_EV_TAKEN, evs[0].type);
    TEST_ASSERT_EQUAL_UINT32(1, evs[0].med_id);
    TEST_ASSERT_EQUAL_UINT32(100, evs[0].log_id);
}

static void test_early_dose_has_negative_delta(void)
{
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, D0 + H(7, 30), NULL, HY_SRC_WEB, NULL));
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), st.logs[0].slot);
    TEST_ASSERT_EQUAL_INT32(-30, st.logs[0].delta_min);
}

static void test_free_log_for_as_needed(void)
{
    st.meds[2].type = HY_SCHED_AS_NEEDED; st.meds[2].n_times = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &clock_port, NULL));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(3, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT64(0, st.logs[0].slot);
    TEST_ASSERT_EQUAL_INT32(0, st.logs[0].delta_min);
}

static void test_log_blocked_when_clock_invalid(void)
{
    clk.valid = false;
    TEST_ASSERT_EQUAL_INT(HY_E_CLOCK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_CLOCK, hy_act_skip(1, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(0, st.appends);
    TEST_ASSERT_EQUAL_INT(0, st.updates);
    TEST_ASSERT_EQUAL_INT(0, st.saves);
    TEST_ASSERT_EQUAL_INT(0, n_evs);
}

static void test_unknown_or_archived_med(void)
{
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_log_taken(99, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_archive(3, true));
    st.saves = 0; n_evs = 0;
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_log_taken(3, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_skip(3, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_skip(99, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(0, st.appends + st.updates + st.deletes + st.saves);
    TEST_ASSERT_EQUAL_INT(0, n_evs);
}

static void test_pill_count_and_undo(void)
{
    uint32_t id;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(2, 0, NULL, HY_SRC_SCREEN, &id));
    TEST_ASSERT_EQUAL_INT16(11, med_by_id(2)->pills_left);
    TEST_ASSERT_EQUAL_INT16(11, st.meds[1].pills_left); /* saved */
    n_evs = 0;
    clk.now += 3;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(id));
    TEST_ASSERT_EQUAL_INT16(12, med_by_id(2)->pills_left);
    TEST_ASSERT_EQUAL_INT16(12, st.meds[1].pills_left);
    TEST_ASSERT_EQUAL_size_t(0, st.n_logs);
    TEST_ASSERT_EQUAL_INT(1, n_evs);
    TEST_ASSERT_EQUAL_INT(HY_EV_UNDONE, evs[0].type);
    TEST_ASSERT_EQUAL_UINT32(id, evs[0].log_id);
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_undo(id));
    TEST_ASSERT_EQUAL_INT16(12, med_by_id(2)->pills_left);
}

static void test_no_pill_change_at_zero_or_disabled(void)
{
    st.meds[1].pills_left = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &clock_port, NULL));
    st.saves = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(2, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT16(0, med_by_id(2)->pills_left);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(0, st.saves);
}

static void test_undo_expired(void)
{
    uint32_t id;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(2, 0, NULL, HY_SRC_SCREEN, &id));
    clk.now += 6;
    TEST_ASSERT_EQUAL_INT(HY_E_UNDO_EXPIRED, hy_act_undo(id));
    TEST_ASSERT_EQUAL_size_t(1, st.n_logs);
    TEST_ASSERT_EQUAL_INT16(11, med_by_id(2)->pills_left);
    clk.now -= 4; /* even back inside the window a later undo must not succeed */
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_undo(id));
    TEST_ASSERT_EQUAL_size_t(1, st.n_logs);
}

static void test_undo_at_exactly_5s(void)
{
    uint32_t id;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, &id));
    clk.now += 5;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(id));
}

static void test_taken_converts_missed(void)
{
    hy_log_t m;
    memset(&m, 0, sizeof m);
    m.id = 50; m.ts = D0 + H(10, 0); strcpy(m.tz, "UTC0"); m.med_id = 1;
    m.status = HY_MISSED; m.slot = D0 + H(8, 0); m.source = HY_SRC_AUTO;
    st.logs[st.n_logs++] = m;
    clk.now = D0 + H(10, 30);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_WEB, &id));
    TEST_ASSERT_EQUAL_UINT32(50, id);
    TEST_ASSERT_EQUAL_size_t(1, st.n_logs);
    TEST_ASSERT_EQUAL_INT(0, st.appends);
    TEST_ASSERT_EQUAL_INT(1, st.updates);
    TEST_ASSERT_EQUAL_INT(HY_TAKEN, st.logs[0].status);
    TEST_ASSERT_EQUAL_INT64(D0 + H(10, 30), st.logs[0].ts);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), st.logs[0].slot);
    TEST_ASSERT_EQUAL_INT32(150, st.logs[0].delta_min);
    TEST_ASSERT_EQUAL_INT(HY_SRC_WEB, st.logs[0].source);
    TEST_ASSERT_FALSE(st.logs[0].edited);

    /* undo restores the original missed entry */
    clk.now += 2;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(50));
    TEST_ASSERT_EQUAL_size_t(1, st.n_logs);
    TEST_ASSERT_EQUAL_INT(HY_MISSED, st.logs[0].status);
    TEST_ASSERT_EQUAL_INT64(D0 + H(10, 0), st.logs[0].ts);
    TEST_ASSERT_EQUAL_INT(HY_SRC_AUTO, st.logs[0].source);
    TEST_ASSERT_EQUAL_INT(0, st.deletes);
}

static void test_upsert_validates(void)
{
    hy_med_t m; fixed_med(&m, 0, "A", "Another", 5);
    TEST_ASSERT_EQUAL_INT(HY_E_CODE_DUP, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_INT(0, st.saves);
    TEST_ASSERT_EQUAL_INT(0, n_evs);
    TEST_ASSERT_EQUAL_UINT32(0, m.id);

    strcpy(m.code, "N");
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_UINT32(4, m.id);
    TEST_ASSERT_EQUAL_INT(1, st.saves);
    TEST_ASSERT_EQUAL_size_t(4, st.n_meds);
    TEST_ASSERT_EQUAL_INT(1, n_evs);
    TEST_ASSERT_EQUAL_INT(HY_EV_MEDS_CHANGED, evs[0].type);
}

static void test_upsert_id_uses_max_including_archived(void)
{
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_archive(3, true));
    hy_med_t m; fixed_med(&m, 0, "N", "New", 9);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_UINT32(4, m.id);
}

/* Distinct 2-character code for filler medicine i (0..287): Q0..QZ, R0..RZ, ... */
static void filler_code(int i, char code[3])
{
    static const char alnum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    code[0] = "QRSTUWXY"[i / 36];
    code[1] = alnum[i % 36];
    code[2] = 0;
}

/* Replaces the store with n_active active then n_archived archived medicines (ids 1..n, distinct
 * codes) and re-initialises the module. */
static void load_fixture(int n_active, int n_archived)
{
    memset(st.meds, 0, sizeof st.meds);
    int n = n_active + n_archived;
    for (int i = 0; i < n; i++) {
        char code[3]; filler_code(i, code);
        fixed_med(&st.meds[i], (uint32_t)(i + 1), code, "Filler", (uint16_t)i);
        st.meds[i].archived = i >= n_active;
    }
    st.n_meds = (size_t)n;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &clock_port, NULL));
    st.saves = 0;
}

static void test_upsert_unknown_id_and_full(void)
{
    hy_med_t m; fixed_med(&m, 77, "N", "New", 9);
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_INT(0, st.saves);
    /* fill to the limit with active medicines (3 from setUp) */
    for (int i = 3; i < HY_MAX_MEDS; i++) {
        hy_med_t f; char code[3]; filler_code(i, code);
        fixed_med(&f, 0, code, "Filler", 10);
        TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&f));
    }
    TEST_ASSERT_EQUAL_size_t(HY_MAX_MEDS, hy_meds_active_count());
    fixed_med(&m, 0, "N", "New", 9);
    st.saves = 0;
    TEST_ASSERT_EQUAL_INT(HY_E_FULL, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_INT(0, st.saves);
    /* a new archived medicine does not count toward the active limit */
    m.archived = true;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_size_t(HY_MAX_MEDS, hy_meds_active_count());
}

static void test_full_counts_active_only(void)
{
    load_fixture(50, 0);
    hy_med_t m; fixed_med(&m, 0, "N", "New", 99);
    TEST_ASSERT_EQUAL_INT(HY_E_FULL, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_INT(0, st.saves);
    TEST_ASSERT_EQUAL_size_t(50, st.n_meds);
    const hy_med_t *p;
    TEST_ASSERT_EQUAL_size_t(50, hy_meds(&p));

    load_fixture(49, 30);
    fixed_med(&m, 0, "N", "New", 99);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_UINT32(80, m.id);   /* max id 79, archived included */
    TEST_ASSERT_EQUAL_INT(1, st.saves);
    TEST_ASSERT_EQUAL_size_t(80, st.n_meds);
    TEST_ASSERT_EQUAL_size_t(50, hy_meds_active_count());
}

static void test_stored_cap(void)
{
    load_fixture(40, 60);
    hy_med_t m; fixed_med(&m, 0, "N", "New", 99);
    TEST_ASSERT_EQUAL_INT(HY_E_FULL, hy_act_med_upsert(&m));
    m.archived = true;
    TEST_ASSERT_EQUAL_INT(HY_E_FULL, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_INT(0, st.saves);
    TEST_ASSERT_EQUAL_size_t(100, st.n_meds);
    /* editing an existing medicine at the cap still works */
    fixed_med(&m, 1, "Q0", "Renamed", 0);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&m));
}

static void test_unarchive_full(void)
{
    load_fixture(50, 1);   /* id 51 archived */
    TEST_ASSERT_EQUAL_INT(HY_E_FULL, hy_act_med_archive(51, false));
    TEST_ASSERT_EQUAL_INT(0, st.saves);
    TEST_ASSERT_TRUE(med_by_id(51)->archived);
    /* the same through a plain upsert of the stored record */
    hy_med_t m = *med_by_id(51);
    m.archived = false;
    TEST_ASSERT_EQUAL_INT(HY_E_FULL, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_INT(0, st.saves);

    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_archive(1, true));   /* 49 active */
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_archive(51, false));
    TEST_ASSERT_FALSE(med_by_id(51)->archived);
    TEST_ASSERT_EQUAL_size_t(50, hy_meds_active_count());
}

static void test_load_stored_100(void)
{
    load_fixture(37, 63);
    const hy_med_t *p;
    TEST_ASSERT_EQUAL_size_t(100, hy_meds(&p));
    TEST_ASSERT_EQUAL_size_t(37, hy_meds_active_count());
    TEST_ASSERT_EQUAL_UINT32(100, med_by_id(100)->id);
}

static void test_upsert_save_failure_keeps_state(void)
{
    st.fail_save = true;
    hy_med_t m; fixed_med(&m, 0, "N", "New", 9);
    TEST_ASSERT_EQUAL_INT(HY_E_IO, hy_act_med_upsert(&m));
    const hy_med_t *p;
    TEST_ASSERT_EQUAL_size_t(3, hy_meds(&p));
    TEST_ASSERT_EQUAL_INT(0, n_evs);
    fixed_med(&m, 1, "A", "Renamed", 0);
    TEST_ASSERT_EQUAL_INT(HY_E_IO, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_STRING("Antibiotic", med_by_id(1)->name);
}

static void test_upsert_sorts_by_order_stably(void)
{
    hy_med_t m; fixed_med(&m, 0, "N", "New", 1); /* ties with V (order 1): V stays first */
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&m));
    const hy_med_t *p; size_t n = hy_meds(&p);
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_UINT32(1, p[0].id);
    TEST_ASSERT_EQUAL_UINT32(2, p[1].id);
    TEST_ASSERT_EQUAL_UINT32(4, p[2].id);
    TEST_ASSERT_EQUAL_UINT32(3, p[3].id);
    fixed_med(&m, 3, "M", "Magnesium", 0); /* moves to the front, after A (tie, stable) */
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&m));
    n = hy_meds(&p);
    TEST_ASSERT_EQUAL_UINT32(1, p[0].id);
    TEST_ASSERT_EQUAL_UINT32(3, p[1].id);
}

static void test_archive(void)
{
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_med_archive(99, true));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_archive(1, true));
    TEST_ASSERT_TRUE(med_by_id(1)->archived);
    TEST_ASSERT_TRUE(st.meds[0].archived);
    TEST_ASSERT_EQUAL_INT(HY_EV_MEDS_CHANGED, evs[n_evs - 1].type);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_archive(1, false));
    TEST_ASSERT_FALSE(med_by_id(1)->archived);
    st.fail_save = true;
    n_evs = 0;
    TEST_ASSERT_EQUAL_INT(HY_E_IO, hy_act_med_archive(1, true));
    TEST_ASSERT_FALSE(med_by_id(1)->archived);
    TEST_ASSERT_EQUAL_INT(0, n_evs);
}

static void test_skip(void)
{
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_skip(2, HY_SRC_WEB, &id));
    TEST_ASSERT_EQUAL_size_t(1, st.n_logs);
    TEST_ASSERT_EQUAL_INT(HY_SKIPPED, st.logs[0].status);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), st.logs[0].slot);
    TEST_ASSERT_EQUAL_INT(HY_SRC_WEB, st.logs[0].source);
    TEST_ASSERT_EQUAL_INT16(12, med_by_id(2)->pills_left);
    TEST_ASSERT_EQUAL_INT(1, n_evs);
    TEST_ASSERT_EQUAL_INT(HY_EV_SKIPPED, evs[0].type);
    TEST_ASSERT_EQUAL_UINT32(id, evs[0].log_id);
    /* skips are undoable */
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(id));
    TEST_ASSERT_EQUAL_size_t(0, st.n_logs);
    TEST_ASSERT_EQUAL_INT16(12, med_by_id(2)->pills_left);
}

static void test_skip_without_slot_is_invalid(void)
{
    st.meds[2].type = HY_SCHED_AS_NEEDED; st.meds[2].n_times = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &clock_port, NULL));
    n_evs = 0;
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_skip(3, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(0, st.appends + st.updates);
    TEST_ASSERT_EQUAL_INT(0, n_evs);
}

static void test_skip_converts_missed(void)
{
    hy_log_t m;
    memset(&m, 0, sizeof m);
    m.id = 60; m.ts = D0 + H(10, 0); strcpy(m.tz, "UTC0"); m.med_id = 1;
    m.status = HY_MISSED; m.slot = D0 + H(8, 0);
    st.logs[st.n_logs++] = m;
    clk.now = D0 + H(10, 30);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_skip(1, HY_SRC_SCREEN, &id));
    TEST_ASSERT_EQUAL_UINT32(60, id);
    TEST_ASSERT_EQUAL_size_t(1, st.n_logs);
    TEST_ASSERT_EQUAL_INT(HY_SKIPPED, st.logs[0].status);
}

static void test_log_write_failure_changes_nothing(void)
{
    st.fail_append = true;
    TEST_ASSERT_EQUAL_INT(HY_E_IO, hy_act_log_taken(2, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(1, st.append_attempts);
    TEST_ASSERT_EQUAL_INT(0, st.appends);
    TEST_ASSERT_EQUAL_INT16(12, med_by_id(2)->pills_left);
    TEST_ASSERT_EQUAL_INT(0, st.saves);
    TEST_ASSERT_EQUAL_INT(0, n_evs);
}

static void test_pill_save_failure_still_succeeds(void)
{
    uint32_t id = 0;
    st.fail_save = true;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(2, 0, NULL, HY_SRC_SCREEN, &id));
    TEST_ASSERT_EQUAL_size_t(1, st.n_logs);
    TEST_ASSERT_EQUAL_INT16(11, med_by_id(2)->pills_left); /* stays decremented in memory */
    TEST_ASSERT_EQUAL_INT(1, n_evs);
    TEST_ASSERT_EQUAL_INT(HY_EV_TAKEN, evs[0].type);
    st.fail_save = false;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(id)); /* undo record exists and restores once */
    TEST_ASSERT_EQUAL_INT16(12, med_by_id(2)->pills_left);
    TEST_ASSERT_EQUAL_INT16(12, st.meds[1].pills_left);
}

static void test_undo_pill_save_failure_still_succeeds(void)
{
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(2, 0, NULL, HY_SRC_SCREEN, &id));
    st.fail_save = true;
    n_evs = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(id));
    TEST_ASSERT_EQUAL_size_t(0, st.n_logs);
    TEST_ASSERT_EQUAL_INT16(12, med_by_id(2)->pills_left);
    TEST_ASSERT_EQUAL_INT(1, n_evs);
    TEST_ASSERT_EQUAL_INT(HY_EV_UNDONE, evs[0].type);
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_undo(id));
    TEST_ASSERT_EQUAL_INT16(12, med_by_id(2)->pills_left);
}

static void test_backdated_does_not_duplicate_taken(void)
{
    clk.now = D0 + H(9, 30);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), st.logs[0].slot);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, D0 + H(8, 5), NULL, HY_SRC_WEB, NULL));
    int for_slot = 0;
    for (size_t i = 0; i < st.n_logs; i++)
        if (st.logs[i].slot == D0 + H(8, 0) && st.logs[i].status == HY_TAKEN) for_slot++;
    TEST_ASSERT_EQUAL_INT(1, for_slot);
}

static void test_backdated_converts_later_missed(void)
{
    hy_log_t m;
    memset(&m, 0, sizeof m);
    m.id = 50; m.ts = D0 + H(10, 0); strcpy(m.tz, "UTC0"); m.med_id = 1;
    m.status = HY_MISSED; m.slot = D0 + H(8, 0); m.source = HY_SRC_AUTO;
    st.logs[st.n_logs++] = m;
    clk.now = D0 + H(10, 30);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, D0 + H(8, 5), NULL, HY_SRC_WEB, &id));
    TEST_ASSERT_EQUAL_UINT32(50, id);
    TEST_ASSERT_EQUAL_size_t(1, st.n_logs);
    TEST_ASSERT_EQUAL_INT(0, st.appends);
    TEST_ASSERT_EQUAL_INT(HY_TAKEN, st.logs[0].status);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 5), st.logs[0].ts);
}

static void test_truncated_query_is_full(void)
{
    for (int i = 0; i < 128; i++) {
        hy_log_t f;
        memset(&f, 0, sizeof f);
        f.id = 1000 + (uint32_t)i; f.ts = D0 - H(1, 0) - i; f.med_id = 3; f.status = HY_TAKEN;
        st.logs[st.n_logs++] = f;
    }
    size_t before = st.n_logs;
    TEST_ASSERT_EQUAL_INT(HY_E_FULL, hy_act_log_taken(3, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(0, st.append_attempts + st.updates + st.saves);
    TEST_ASSERT_EQUAL_size_t(before, st.n_logs);
    TEST_ASSERT_EQUAL_INT(0, n_evs);
}

static void test_as_needed_logs_despite_full_history(void)
{
    hy_med_t m; hy_med_defaults(&m);
    m.id = 0; m.order = 9; strcpy(m.code, "P"); strcpy(m.name, "Painkiller");
    m.type = HY_SCHED_AS_NEEDED; m.min_interval_min = 240;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&m));
    uint32_t pid = m.id;
    /* 130 doses within the 8-day look-back, the newest 5 hours before now */
    for (int i = 0; i < 130; i++) {
        hy_log_t f;
        memset(&f, 0, sizeof f);
        f.id = 1000 + (uint32_t)i; f.ts = clk.now - H(5, 0) - (int64_t)i * 1200;
        f.med_id = pid; f.status = HY_TAKEN;
        st.logs[st.n_logs++] = f;
    }
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(pid, 0, NULL, HY_SRC_SCREEN, &id));
    TEST_ASSERT_EQUAL_size_t(131, st.n_logs);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(id));
    /* the home screen reads [now - max(interval, 1 day), now + 1): 128 cap keeps the newest
     * dose visible because the window holds about 58 of these doses (1 day / 20 min, newest 5 h back) */
    hy_log_t win[128];
    size_t n = s_query(NULL, clk.now - 86400, clk.now + 1, pid, win, 128);
    TEST_ASSERT_TRUE(n < 128);
    int32_t since = -1;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_min_interval_check(med_by_id(pid), clk.now, win, n, &since));
    TEST_ASSERT_EQUAL_INT32(300, since);
    /* a fresh dose inside the window is seen and is too soon */
    hy_log_t f;
    memset(&f, 0, sizeof f);
    f.id = 5000; f.ts = clk.now - 600; f.med_id = pid; f.status = HY_TAKEN;
    st.logs[st.n_logs++] = f;
    n = s_query(NULL, clk.now - 86400, clk.now + 1, pid, win, 128);
    TEST_ASSERT_EQUAL_INT(HY_W_TOO_SOON, hy_min_interval_check(med_by_id(pid), clk.now, win, n, &since));
    TEST_ASSERT_EQUAL_INT32(10, since);
}

static void test_undo_with_clock_stepped_back(void)
{
    uint32_t id;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, &id));
    clk.now -= 1;
    TEST_ASSERT_EQUAL_INT(HY_E_UNDO_EXPIRED, hy_act_undo(id));
    TEST_ASSERT_EQUAL_size_t(1, st.n_logs);
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_undo(id));
}

static void test_uninitialised_and_failed_init(void)
{
    hy_med_t m; fixed_med(&m, 0, "N", "New", 9);
    st.fail_load = true;
    TEST_ASSERT_EQUAL_INT(HY_E_IO, hy_actions_init(&store_port, NULL, &clock_port, NULL));
    const hy_med_t *p;
    TEST_ASSERT_EQUAL_size_t(0, hy_meds(&p));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_skip(1, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_undo(100));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_med_archive(1, true));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_log_edit(&(hy_log_t){0}));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_log_delete(1, 0));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_actions_init(NULL, NULL, &clock_port, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(0, st.appends + st.saves);
}

static const char *c_tz_null(void *c) { (void)c; return NULL; }

static void test_null_tz_is_utc(void)
{
    hy_clock_port_t cp = { c_now, c_tz_null, c_valid, NULL };
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &cp, NULL));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_STRING("UTC0", st.logs[0].tz);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &clock_port, NULL));
}

static void test_undo_log_failure_keeps_record(void)
{
    uint32_t id;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, &id));
    st.fail_delete = true;
    n_evs = 0;
    TEST_ASSERT_EQUAL_INT(HY_E_IO, hy_act_undo(id));
    TEST_ASSERT_EQUAL_INT(0, n_evs);
    st.fail_delete = false;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(id));
}

static void test_undo_ring_keeps_eight(void)
{
    uint32_t ids[9];
    for (int i = 0; i < 9; i++) {
        /* distinct slots on distinct days so each is a fresh entry */
        clk.now = D0 + (int64_t)i * 86400 + H(8, 5);
        TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, &ids[i]));
    }
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_undo(ids[0]));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(ids[8]));
}

static void test_log_edit_and_delete(void)
{
    clk.valid = false; /* neither needs a valid clock */
    hy_log_t e;
    memset(&e, 0, sizeof e);
    e.id = 7; e.ts = D0 + H(9, 0); e.med_id = 1; e.status = HY_TAKEN;
    st.logs[st.n_logs++] = e;
    e.note[0] = 'x'; e.note[1] = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_edit(&e));
    TEST_ASSERT_TRUE(st.logs[0].edited);
    TEST_ASSERT_EQUAL_STRING("x", st.logs[0].note);
    TEST_ASSERT_FALSE(e.edited); /* caller's struct untouched */
    TEST_ASSERT_EQUAL_INT(1, n_evs);
    TEST_ASSERT_EQUAL_INT(HY_EV_LOG_EDITED, evs[0].type);
    TEST_ASSERT_EQUAL_UINT32(7, evs[0].log_id);
    TEST_ASSERT_EQUAL_UINT32(1, evs[0].med_id);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_delete(7, e.ts));
    TEST_ASSERT_EQUAL_size_t(0, st.n_logs);
    TEST_ASSERT_EQUAL_INT(2, n_evs);
    TEST_ASSERT_EQUAL_INT(HY_EV_LOG_EDITED, evs[1].type);
    st.fail_delete = true;
    TEST_ASSERT_EQUAL_INT(HY_E_IO, hy_act_log_delete(7, e.ts));
    TEST_ASSERT_EQUAL_INT(2, n_evs);
}

static void test_upsert_keeps_decremented_pills(void)
{
    hy_med_t stale = *med_by_id(2);                    /* pills_left 12 */
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(2, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT16(11, med_by_id(2)->pills_left);
    strcpy(stale.name, "Vitamin D3");
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&stale));
    TEST_ASSERT_EQUAL_STRING("Vitamin D3", med_by_id(2)->name);
    TEST_ASSERT_EQUAL_INT16(11, med_by_id(2)->pills_left);
    TEST_ASSERT_EQUAL_INT16(11, st.meds[1].pills_left); /* saved */
}

static void test_upsert_enabling_pills_takes_caller_count(void)
{
    hy_med_t m = *med_by_id(1);                        /* pills disabled */
    TEST_ASSERT_FALSE(m.pills_enabled);
    m.pills_enabled = true; m.pills_left = 30; m.pills_alert = 5;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&m));
    TEST_ASSERT_EQUAL_INT16(30, med_by_id(1)->pills_left);
}

static void test_pills_set(void)
{
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(2, 0, NULL, HY_SRC_SCREEN, NULL));
    n_evs = 0; st.saves = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_pills_set(2, 40));
    TEST_ASSERT_EQUAL_INT16(40, med_by_id(2)->pills_left);
    TEST_ASSERT_EQUAL_INT16(40, st.meds[1].pills_left);
    TEST_ASSERT_EQUAL_INT(1, st.saves);
    TEST_ASSERT_EQUAL_INT(1, n_evs);
    TEST_ASSERT_EQUAL_INT(HY_EV_MEDS_CHANGED, evs[0].type);
    TEST_ASSERT_EQUAL_UINT32(2, evs[0].med_id);
    n_evs = 0;
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_pills_set(2, -1));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_pills_set(2, 10000));
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_pills_set(99, 5));
    TEST_ASSERT_EQUAL_INT(0, n_evs);
    TEST_ASSERT_EQUAL_INT16(40, med_by_id(2)->pills_left);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_pills_set(2, 9999));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_pills_set(2, 0));
    st.fail_save = true;
    TEST_ASSERT_EQUAL_INT(HY_E_IO, hy_act_pills_set(2, 7));
    TEST_ASSERT_EQUAL_INT16(0, med_by_id(2)->pills_left);
}

static void test_events_carry_slot(void)
{
    uint32_t id;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, &id));
    TEST_ASSERT_EQUAL_INT(HY_EV_TAKEN, evs[0].type);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), evs[0].slot);
    TEST_ASSERT_EQUAL_INT64(st.logs[0].slot, evs[0].slot);
    n_evs = 0;
    clk.now += 2;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(id));
    TEST_ASSERT_EQUAL_INT(HY_EV_UNDONE, evs[0].type);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), evs[0].slot);

    st.meds[2].type = HY_SCHED_AS_NEEDED; st.meds[2].n_times = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &clock_port, NULL));
    n_evs = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(3, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(HY_EV_TAKEN, evs[0].type);
    TEST_ASSERT_EQUAL_INT64(0, evs[0].slot);
}


/* ---- missed and snooze ---- */

static void test_mark_missed_writes_auto_entry(void)
{
    clk.now = D0 + H(10, 0);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_mark_missed(1, D0 + H(8, 0), &id));
    TEST_ASSERT_EQUAL_size_t(1, st.n_logs);
    TEST_ASSERT_TRUE(id != 0);
    TEST_ASSERT_EQUAL_UINT32(id, st.logs[0].id);
    TEST_ASSERT_EQUAL_INT(HY_MISSED, st.logs[0].status);
    TEST_ASSERT_EQUAL_INT(HY_SRC_AUTO, st.logs[0].source);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), st.logs[0].slot);
    TEST_ASSERT_EQUAL_INT64(D0 + H(10, 0), st.logs[0].ts);
    TEST_ASSERT_EQUAL_INT32(120, st.logs[0].delta_min);
    TEST_ASSERT_EQUAL_STRING("UTC0", st.logs[0].tz);
    TEST_ASSERT_EQUAL_INT(1, n_evs);
    TEST_ASSERT_EQUAL_INT(HY_EV_MISSED, evs[0].type);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), evs[0].slot);
    TEST_ASSERT_EQUAL_UINT32(1, evs[0].med_id);
}

static void test_mark_missed_idempotent(void)
{
    clk.now = D0 + H(10, 0);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_mark_missed(1, D0 + H(8, 0), &id));
    n_evs = 0;
    id = 77;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_mark_missed(1, D0 + H(8, 0), &id));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    TEST_ASSERT_EQUAL_size_t(1, st.n_logs);
    TEST_ASSERT_EQUAL_INT(0, n_evs);

    /* same when a taken entry exists for the slot */
    clk.now = D0 + H(8, 30);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(3, 0, NULL, HY_SRC_SCREEN, NULL));
    n_evs = 0;
    id = 77;
    clk.now = D0 + H(10, 0);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_mark_missed(3, D0 + H(8, 0), &id));
    TEST_ASSERT_EQUAL_UINT32(0, id);
    TEST_ASSERT_EQUAL_size_t(2, st.n_logs);
    TEST_ASSERT_EQUAL_INT(0, n_evs);
}

static void test_mark_missed_rejects_non_slot(void)
{
    clk.now = D0 + H(10, 0);
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_mark_missed(1, D0 + H(8, 0) + 60, NULL));
    TEST_ASSERT_EQUAL_size_t(0, st.n_logs);
    TEST_ASSERT_EQUAL_INT(0, n_evs);
}

static void test_mark_missed_errors(void)
{
    clk.now = D0 + H(10, 0);
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_mark_missed(99, D0 + H(8, 0), NULL));
    st.meds[2].type = HY_SCHED_AS_NEEDED; st.meds[2].n_times = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &clock_port, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_mark_missed(3, D0 + H(8, 0), NULL));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_archive(1, true));
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_mark_missed(1, D0 + H(8, 0), NULL));
    clk.valid = false;
    TEST_ASSERT_EQUAL_INT(HY_E_CLOCK, hy_act_mark_missed(2, D0 + H(8, 0), NULL));
    TEST_ASSERT_EQUAL_size_t(0, st.n_logs);
}

static void test_snooze_basic(void)
{
    int64_t until = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(1, 15, HY_SRC_SCREEN, &until));
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 20), until);
    const hy_snooze_t *sn;
    TEST_ASSERT_EQUAL_size_t(1, hy_snoozes(&sn));
    TEST_ASSERT_EQUAL_UINT32(1, sn[0].med_id);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), sn[0].slot);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 20), sn[0].until);
    TEST_ASSERT_EQUAL_INT(1, n_evs);
    TEST_ASSERT_EQUAL_INT(HY_EV_SNOOZED, evs[0].type);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), evs[0].slot);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 5), evs[0].ts);
    TEST_ASSERT_EQUAL_size_t(0, st.n_logs);

    /* replaces the existing snooze, and expires */
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(1, 30, HY_SRC_SCREEN, &until));
    TEST_ASSERT_EQUAL_size_t(1, hy_snoozes(&sn));
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 35), sn[0].until);
    clk.now = D0 + H(8, 35);
    TEST_ASSERT_EQUAL_size_t(0, hy_snoozes(&sn));
}

static void test_snooze_bad_args(void)
{
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_snooze(1, 0, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_snooze(1, 121, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_snooze(99, 10, HY_SRC_SCREEN, NULL));
    st.meds[2].type = HY_SCHED_AS_NEEDED; st.meds[2].n_times = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &clock_port, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_act_snooze(3, 10, HY_SRC_SCREEN, NULL));
    clk.valid = false;
    TEST_ASSERT_EQUAL_INT(HY_E_CLOCK, hy_act_snooze(1, 10, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(0, n_evs);
}

static void test_snooze_clipped_at_missed(void)
{
    clk.now = D0 + H(9, 50);
    int64_t until = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(1, 15, HY_SRC_SCREEN, &until));
    TEST_ASSERT_EQUAL_INT64(D0 + H(10, 0), until);
    const hy_snooze_t *sn;
    TEST_ASSERT_EQUAL_size_t(1, hy_snoozes(&sn));
    TEST_ASSERT_EQUAL_INT64(D0 + H(10, 0), sn[0].until);
    TEST_ASSERT_EQUAL_INT(HY_EV_SNOOZED, evs[0].type);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), evs[0].slot);
}

static void test_snooze_rejected_when_upcoming_or_missed(void)
{
    clk.now = D0 + H(7, 30);
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_snooze(1, 15, HY_SRC_SCREEN, NULL));
    clk.now = D0 + H(10, 0);
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_act_snooze(1, 15, HY_SRC_SCREEN, NULL));
    const hy_snooze_t *sn;
    TEST_ASSERT_EQUAL_size_t(0, hy_snoozes(&sn));
    TEST_ASSERT_EQUAL_INT(0, n_evs);
}

static void test_taken_drops_snooze(void)
{
    const hy_snooze_t *sn;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(1, 15, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_size_t(1, hy_snoozes(&sn));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_size_t(0, hy_snoozes(&sn));
    /* skipped drops it too */
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(3, 15, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_size_t(1, hy_snoozes(&sn));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_skip(3, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_size_t(0, hy_snoozes(&sn));
}

static void test_mark_missed_drops_snooze(void)
{
    const hy_snooze_t *sn;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(1, 15, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(3, 15, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_size_t(2, hy_snoozes(&sn));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_mark_missed(1, D0 + H(8, 0), NULL));
    TEST_ASSERT_EQUAL_size_t(1, hy_snoozes(&sn));
    TEST_ASSERT_EQUAL_UINT32(3, sn[0].med_id);
}

static void test_snooze_after_east_jump(void)
{
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &clock_port_zurich, NULL));
    clk.now = T_R6_1000Z + 5 * 60;
    int64_t until = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(1, 15, HY_SRC_SCREEN, &until));
    TEST_ASSERT_EQUAL_INT64(clk.now + 15 * 60, until);
    TEST_ASSERT_EQUAL_INT64(T_R6_0600Z, evs[0].slot);
}


static void init_zurich_two_slots(void)
{
    st.meds[0].times[1] = 13 * 60;
    st.meds[0].n_times = 2;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &clock_port_zurich, NULL));
}

static void test_snooze_east_two_slots(void)
{
    init_zurich_two_slots();
    clk.now = T_R6_1000Z + 5 * 60;
    int64_t until = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(1, 15, HY_SRC_SCREEN, &until));
    TEST_ASSERT_EQUAL_INT64(clk.now + 15 * 60, until);
    TEST_ASSERT_EQUAL_INT64(T_R6_0600Z, evs[0].slot);
}

static void test_taken_east_two_slots(void)
{
    init_zurich_two_slots();
    clk.now = T_R6_1000Z + 5 * 60;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT64(T_R6_0600Z, st.logs[0].slot);
}

static void test_snooze_east_clipped(void)
{
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&store_port, NULL, &clock_port_zurich, NULL));
    clk.now = T_R6_1000Z + 110 * 60;
    int64_t until = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(1, 15, HY_SRC_SCREEN, &until));
    TEST_ASSERT_EQUAL_INT64(T_R6_1000Z + 120 * 60, until);
}

static void test_undo_restores_snooze(void)
{
    const hy_snooze_t *sn;
    int64_t until = 0;
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(1, 15, HY_SRC_SCREEN, &until));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, &id));
    TEST_ASSERT_EQUAL_size_t(0, hy_snoozes(&sn));
    clk.now += 2;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(id));
    TEST_ASSERT_EQUAL_size_t(1, hy_snoozes(&sn));
    TEST_ASSERT_EQUAL_UINT32(1, sn[0].med_id);
    TEST_ASSERT_EQUAL_INT64(D0 + H(8, 0), sn[0].slot);
    TEST_ASSERT_EQUAL_INT64(until, sn[0].until);

    /* skipped, then undone */
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_skip(1, HY_SRC_SCREEN, &id));
    TEST_ASSERT_EQUAL_size_t(0, hy_snoozes(&sn));
    clk.now += 2;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(id));
    TEST_ASSERT_EQUAL_size_t(1, hy_snoozes(&sn));
}

static void test_undo_does_not_restore_expired_snooze(void)
{
    const hy_snooze_t *sn;
    uint32_t id = 0;
    clk.now = D0 + H(8, 5);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(1, 1, HY_SRC_SCREEN, NULL)); /* until 08:06:00 */
    clk.now = D0 + H(8, 5) + 58;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_log_taken(1, 0, NULL, HY_SRC_SCREEN, &id));
    clk.now += 4; /* 08:06:02, inside the undo window, past the snooze */
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_undo(id));
    TEST_ASSERT_EQUAL_size_t(0, hy_snoozes(&sn));
}

static void test_archive_and_schedule_change_drop_snooze(void)
{
    const hy_snooze_t *sn;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(1, 15, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_snooze(3, 15, HY_SRC_SCREEN, NULL));
    TEST_ASSERT_EQUAL_size_t(2, hy_snoozes(&sn));

    /* a name change keeps the snooze */
    hy_med_t upd = *med_by_id(1);
    strcpy(upd.name, "Antibiotic 2");
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&upd));
    TEST_ASSERT_EQUAL_size_t(2, hy_snoozes(&sn));

    /* a time change drops it */
    upd.times[0] = 9 * 60;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_upsert(&upd));
    TEST_ASSERT_EQUAL_size_t(1, hy_snoozes(&sn));
    TEST_ASSERT_EQUAL_UINT32(3, sn[0].med_id);

    /* archiving drops it */
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_archive(3, true));
    TEST_ASSERT_EQUAL_size_t(0, hy_snoozes(&sn));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_act_med_archive(3, false));
    TEST_ASSERT_EQUAL_size_t(0, hy_snoozes(&sn));
}

/* ---- untracked gaps ---- */
static void test_record_gap_publishes_once(void)
{
    hy_gap_t g = { D0 + 1000, D0 + 5000 };
    n_evs = 0;
    TEST_ASSERT_EQUAL(HY_OK, hy_act_record_gap(&g, HY_GAP_LOST));
    TEST_ASSERT_EQUAL_INT(1, st.n_gaps);
    TEST_ASSERT_EQUAL_INT64(g.start, st.gaps[0].start);
    TEST_ASSERT_EQUAL_INT64(g.end, st.gaps[0].end);
    TEST_ASSERT_EQUAL_UINT8(HY_GAP_LOST, st.gap_reasons[0]);
    TEST_ASSERT_EQUAL_INT(1, n_evs);
    TEST_ASSERT_EQUAL(HY_EV_UNTRACKED, evs[0].type);
    TEST_ASSERT_EQUAL_INT64(g.start, evs[0].slot);
    TEST_ASSERT_EQUAL_INT64(g.end, evs[0].ts);
    TEST_ASSERT_EQUAL_INT32(HY_GAP_LOST, evs[0].value);

    /* needs no valid clock */
    clk.valid = false;
    n_evs = 0;
    TEST_ASSERT_EQUAL(HY_OK, hy_act_record_gap(&g, HY_GAP_CLOCK));
    TEST_ASSERT_EQUAL_INT(1, n_evs);
    clk.valid = true;

    hy_gap_t bad = { 500, 500 };
    n_evs = 0;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_act_record_gap(&bad, HY_GAP_OFF));
    bad.end = 400;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_act_record_gap(&bad, HY_GAP_OFF));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_act_record_gap(NULL, HY_GAP_OFF));
    TEST_ASSERT_EQUAL_INT(2, st.n_gaps);
    TEST_ASSERT_EQUAL_INT(0, n_evs);
}

static void test_record_gap_without_port_member(void)
{
    static hy_store_port_t bare;
    bare = store_port;
    bare.append_gap = NULL;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_actions_init(&bare, NULL, &clock_port, NULL));
    n_evs = 0;
    hy_gap_t g = { D0, D0 + 600 };
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_act_record_gap(&g, HY_GAP_OFF));
    TEST_ASSERT_EQUAL_INT(0, n_evs);
    TEST_ASSERT_EQUAL_INT(0, st.n_gaps);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_events_subscribe_unsubscribe);
    RUN_TEST(test_log_taken_writes_and_publishes);
    RUN_TEST(test_early_dose_has_negative_delta);
    RUN_TEST(test_free_log_for_as_needed);
    RUN_TEST(test_log_blocked_when_clock_invalid);
    RUN_TEST(test_unknown_or_archived_med);
    RUN_TEST(test_pill_count_and_undo);
    RUN_TEST(test_no_pill_change_at_zero_or_disabled);
    RUN_TEST(test_undo_expired);
    RUN_TEST(test_undo_at_exactly_5s);
    RUN_TEST(test_taken_converts_missed);
    RUN_TEST(test_upsert_validates);
    RUN_TEST(test_upsert_id_uses_max_including_archived);
    RUN_TEST(test_upsert_unknown_id_and_full);
    RUN_TEST(test_full_counts_active_only);
    RUN_TEST(test_stored_cap);
    RUN_TEST(test_unarchive_full);
    RUN_TEST(test_load_stored_100);
    RUN_TEST(test_upsert_save_failure_keeps_state);
    RUN_TEST(test_upsert_sorts_by_order_stably);
    RUN_TEST(test_archive);
    RUN_TEST(test_skip);
    RUN_TEST(test_skip_without_slot_is_invalid);
    RUN_TEST(test_skip_converts_missed);
    RUN_TEST(test_log_write_failure_changes_nothing);
    RUN_TEST(test_pill_save_failure_still_succeeds);
    RUN_TEST(test_undo_pill_save_failure_still_succeeds);
    RUN_TEST(test_backdated_does_not_duplicate_taken);
    RUN_TEST(test_backdated_converts_later_missed);
    RUN_TEST(test_truncated_query_is_full);
    RUN_TEST(test_as_needed_logs_despite_full_history);
    RUN_TEST(test_undo_with_clock_stepped_back);
    RUN_TEST(test_uninitialised_and_failed_init);
    RUN_TEST(test_null_tz_is_utc);
    RUN_TEST(test_undo_log_failure_keeps_record);
    RUN_TEST(test_undo_ring_keeps_eight);
    RUN_TEST(test_log_edit_and_delete);
    RUN_TEST(test_upsert_keeps_decremented_pills);
    RUN_TEST(test_upsert_enabling_pills_takes_caller_count);
    RUN_TEST(test_pills_set);
    RUN_TEST(test_events_carry_slot);
    RUN_TEST(test_mark_missed_writes_auto_entry);
    RUN_TEST(test_mark_missed_idempotent);
    RUN_TEST(test_mark_missed_rejects_non_slot);
    RUN_TEST(test_mark_missed_errors);
    RUN_TEST(test_snooze_basic);
    RUN_TEST(test_snooze_bad_args);
    RUN_TEST(test_snooze_clipped_at_missed);
    RUN_TEST(test_snooze_rejected_when_upcoming_or_missed);
    RUN_TEST(test_taken_drops_snooze);
    RUN_TEST(test_mark_missed_drops_snooze);
    RUN_TEST(test_snooze_after_east_jump);
    RUN_TEST(test_snooze_east_two_slots);
    RUN_TEST(test_taken_east_two_slots);
    RUN_TEST(test_snooze_east_clipped);
    RUN_TEST(test_undo_restores_snooze);
    RUN_TEST(test_undo_does_not_restore_expired_snooze);
    RUN_TEST(test_archive_and_schedule_change_drop_snooze);
    RUN_TEST(test_record_gap_publishes_once);
    RUN_TEST(test_record_gap_without_port_member);
    return UNITY_END();
}
