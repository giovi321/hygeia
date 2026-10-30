#include <string.h>
#include "unity.h"
#include "hy/summary.h"

#define TZ_ZURICH "CET-1CEST,M3.5.0,M10.5.0/3"
#define T_2026_11_02_LOCAL_MIDNIGHT 1793574000LL /* 2026-11-01T23:00:00Z, Monday, CET */
#define DAY 86400LL
#define MIN 60LL
#define HOUR 3600LL
#define L(h, m) (T_2026_11_02_LOCAL_MIDNIGHT + (h) * HOUR + (m) * MIN)
/* 2026-10-25 00:00 local (CEST) */
#define TRACK_START (L(0, 0) - 8 * DAY - HOUR)
#define D_TODAY 20261102

void setUp(void) {}
void tearDown(void) {}

static hy_sched_t s;
static hy_med_t meds[4];
static size_t n_meds;
static hy_log_t logs[16];
static size_t n_logs;
static hy_snooze_t snoozes[4];
static size_t n_snoozes;

static void reset(void)
{
    hy_sched_init(&s);
    memset(meds, 0, sizeof meds);
    memset(logs, 0, sizeof logs);
    memset(snoozes, 0, sizeof snoozes);
    n_meds = n_logs = n_snoozes = 0;
}

static hy_med_t *add_a(void)
{
    hy_med_t *m = &meds[n_meds++];
    hy_med_defaults(m);
    m->id = 1; strcpy(m->code, "A"); strcpy(m->name, "Antibiotic");
    m->type = HY_SCHED_FIXED;
    m->times[0] = 6 * 60; m->times[1] = 14 * 60; m->times[2] = 22 * 60; m->n_times = 3;
    m->order = 0;
    return m;
}

static hy_med_t *add_v(void)
{
    hy_med_t *m = &meds[n_meds++];
    hy_med_defaults(m);
    m->id = 2; strcpy(m->code, "V"); strcpy(m->name, "Vitamin D");
    m->type = HY_SCHED_FIXED;
    m->times[0] = 8 * 60 + 30; m->n_times = 1;
    m->order = 1;
    return m;
}

static hy_med_t *add_m(void)
{
    hy_med_t *m = &meds[n_meds++];
    hy_med_defaults(m);
    m->id = 3; strcpy(m->code, "M"); strcpy(m->name, "Magnesium");
    m->type = HY_SCHED_AS_NEEDED;
    m->min_interval_min = 240;
    m->order = 2;
    return m;
}

static void add_log(uint32_t med_id, int64_t ts, hy_status_t st, int64_t slot)
{
    hy_log_t *l = &logs[n_logs];
    memset(l, 0, sizeof *l);
    l->id = (uint32_t)(n_logs + 1);
    l->ts = ts; strcpy(l->tz, TZ_ZURICH);
    l->med_id = med_id; l->status = st; l->slot = slot;
    l->delta_min = slot ? (int32_t)((ts - slot) / 60) : 0;
    n_logs++;
}

static hy_sched_in_t input(int64_t now)
{
    hy_sched_in_t in;
    memset(&in, 0, sizeof in);
    in.meds = meds; in.n_meds = n_meds;
    in.logs = logs; in.n_logs = n_logs;
    in.snoozes = snoozes; in.n_snoozes = n_snoozes;
    in.tz = TZ_ZURICH; in.now = now;
    return in;
}

/* Tracking from 2026-10-25 00:00 local: start, then one eval at that instant so every
 * medicine's since = tracking start. Queries may then use any now. */
static void track_at(int64_t start)
{
    hy_sched_start_tracking(&s, start);
    hy_sched_in_t in = input(start);
    in.logs = NULL; in.n_logs = 0;
    hy_sched_out_t o;
    hy_sched_eval(&s, &in, &o);
}

static void track(void)
{
    track_at(TRACK_START);
}

static hy_tile_view_t tile(const hy_med_t *m, int64_t now)
{
    hy_sched_in_t in = input(now);
    hy_tile_view_t v;
    memset(&v, 0x5A, sizeof v);
    hy_tile_view(m, &in, &s, &v);
    return v;
}

static void test_tile_matches_plan1_cases(void)
{
    reset();
    hy_med_t *a = add_a();
    hy_med_t *v = add_v();
    v->active_from = 20261103; /* no previous slot yet: no dose */
    track();
    hy_tile_view_t t = tile(v, L(7, 0));
    TEST_ASSERT_EQUAL_INT(HY_TILE_NO_DOSE, t.kind);
    TEST_ASSERT_EQUAL_INT32(0, t.ring);
    TEST_ASSERT_EQUAL_INT64(0, t.last_taken);

    add_log(1, L(6, 10), HY_TAKEN, L(6, 0));
    t = tile(a, L(10, 10));
    TEST_ASSERT_EQUAL_INT(HY_TILE_SINCE, t.kind);
    TEST_ASSERT_EQUAL_INT32(510, t.ring);
    TEST_ASSERT_EQUAL_INT64(L(6, 10), t.last_taken);

    v->active_from = 0;
    t = tile(v, L(8, 30));
    TEST_ASSERT_EQUAL_INT(HY_TILE_DUE, t.kind);
    TEST_ASSERT_EQUAL_INT32(HY_RING_MAX, t.ring);
    t = tile(v, L(9, 0));
    TEST_ASSERT_EQUAL_INT(HY_TILE_LATE, t.kind);
    TEST_ASSERT_EQUAL_INT32(HY_RING_MAX, t.ring);
    t = tile(v, L(10, 30));
    TEST_ASSERT_EQUAL_INT(HY_TILE_MISSED, t.kind);
    TEST_ASSERT_EQUAL_INT32(HY_RING_MAX, t.ring);

    snoozes[n_snoozes].med_id = 2; snoozes[n_snoozes].slot = L(8, 30); snoozes[n_snoozes].until = L(8, 50);
    n_snoozes++;
    t = tile(v, L(8, 40));
    TEST_ASSERT_EQUAL_INT(HY_TILE_SNOOZED, t.kind);
    TEST_ASSERT_EQUAL_INT64(L(8, 50), t.snoozed_until);
    TEST_ASSERT_EQUAL_INT32(HY_RING_MAX, t.ring);
    t = tile(v, L(8, 55)); /* snooze expired */
    TEST_ASSERT_EQUAL_INT(HY_TILE_DUE, t.kind);
    TEST_ASSERT_EQUAL_INT64(0, t.snoozed_until);
}

static void test_tile_as_needed_ring(void)
{
    reset();
    hy_med_t *m = add_m();
    track();
    hy_tile_view_t t = tile(m, L(10, 0));
    TEST_ASSERT_EQUAL_INT(HY_TILE_AS_NEEDED, t.kind);
    TEST_ASSERT_EQUAL_INT32(0, t.ring);
    add_log(3, L(8, 0), HY_TAKEN, 0);
    t = tile(m, L(10, 0));
    TEST_ASSERT_EQUAL_INT(HY_TILE_AS_NEEDED, t.kind);
    TEST_ASSERT_EQUAL_INT32(500, t.ring);
    TEST_ASSERT_EQUAL_INT64(L(8, 0), t.last_taken);
}

static void test_today_rows_and_count(void)
{
    reset();
    add_a(); add_v(); add_m();
    track();
    add_log(1, L(6, 28), HY_TAKEN, L(6, 0));
    hy_sched_in_t in = input(L(8, 42));
    hy_today_row_t rows[16];
    uint16_t taken = 99, total = 99;
    size_t n = hy_today_rows(&in, &s, rows, 16, &taken, &total);
    TEST_ASSERT_EQUAL_UINT(5, n);
    TEST_ASSERT_EQUAL_UINT32(1, rows[0].med_id);
    TEST_ASSERT_EQUAL_INT64(L(6, 0), rows[0].slot);
    TEST_ASSERT_EQUAL_INT64(L(6, 28), rows[0].ts);
    TEST_ASSERT_EQUAL_INT(HY_OCC_TAKEN, rows[0].state);
    TEST_ASSERT_EQUAL_UINT32(2, rows[1].med_id);
    TEST_ASSERT_EQUAL_INT64(L(8, 30), rows[1].slot);
    TEST_ASSERT_EQUAL_INT64(0, rows[1].ts);
    TEST_ASSERT_EQUAL_INT(HY_OCC_DUE, rows[1].state);
    TEST_ASSERT_EQUAL_INT64(L(14, 0), rows[2].slot);
    TEST_ASSERT_EQUAL_INT(HY_OCC_UPCOMING, rows[2].state);
    TEST_ASSERT_EQUAL_INT64(L(22, 0), rows[3].slot);
    TEST_ASSERT_EQUAL_INT(HY_OCC_UPCOMING, rows[3].state);
    TEST_ASSERT_EQUAL_UINT32(3, rows[4].med_id);
    TEST_ASSERT_TRUE(rows[4].as_needed);
    TEST_ASSERT_EQUAL_UINT16(0, rows[4].taken_today);
    TEST_ASSERT_FALSE(rows[0].as_needed);
    TEST_ASSERT_EQUAL_UINT16(1, taken);
    TEST_ASSERT_EQUAL_UINT16(4, total);

    /* max truncates, counts still cover all fixed rows */
    TEST_ASSERT_EQUAL_UINT(2, hy_today_rows(&in, &s, rows, 2, &taken, &total));
    TEST_ASSERT_EQUAL_UINT16(4, total);

    /* an as-needed dose today */
    add_log(3, L(7, 0), HY_TAKEN, 0);
    in = input(L(8, 42));
    n = hy_today_rows(&in, &s, rows, 16, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT(5, n);
    TEST_ASSERT_EQUAL_UINT16(1, rows[4].taken_today);
    TEST_ASSERT_EQUAL_INT64(L(7, 0), rows[4].ts);
}

static void test_day_mark_priority(void)
{
    reset();
    hy_med_t *a = add_a();
    track();
    /* 2026-10-30: one on time, one late (40 min >= 30), one on time */
    add_log(1, L(6, 5) - 3 * DAY, HY_TAKEN, L(6, 0) - 3 * DAY);
    add_log(1, L(14, 40) - 3 * DAY, HY_TAKEN, L(14, 0) - 3 * DAY);
    add_log(1, L(22, 0) - 3 * DAY, HY_TAKEN, L(22, 0) - 3 * DAY);
    /* 2026-10-29: second slot missed (no log, window passed) */
    add_log(1, L(6, 0) - 4 * DAY, HY_TAKEN, L(6, 0) - 4 * DAY);
    add_log(1, L(22, 0) - 4 * DAY, HY_TAKEN, L(22, 0) - 4 * DAY);
    /* 2026-10-28: all skipped */
    add_log(1, L(6, 1) - 5 * DAY, HY_SKIPPED, L(6, 0) - 5 * DAY);
    add_log(1, L(14, 1) - 5 * DAY, HY_SKIPPED, L(14, 0) - 5 * DAY);
    add_log(1, L(22, 1) - 5 * DAY, HY_SKIPPED, L(22, 0) - 5 * DAY);
    /* 2026-10-27: all on time */
    add_log(1, L(6, 0) - 6 * DAY, HY_TAKEN, L(6, 0) - 6 * DAY);
    add_log(1, L(14, 0) - 6 * DAY, HY_TAKEN, L(14, 0) - 6 * DAY);
    add_log(1, L(22, 0) - 6 * DAY, HY_TAKEN, L(22, 0) - 6 * DAY);
    hy_sched_in_t in = input(L(8, 0));
    bool prog = true;
    TEST_ASSERT_EQUAL_INT(HY_MARK_LATE, hy_day_mark(a, 20261030, &in, &s, &prog));
    TEST_ASSERT_FALSE(prog);
    TEST_ASSERT_EQUAL_INT(HY_MARK_MISSED, hy_day_mark(a, 20261029, &in, &s, &prog));
    TEST_ASSERT_EQUAL_INT(HY_MARK_SKIPPED, hy_day_mark(a, 20261028, &in, &s, &prog));
    TEST_ASSERT_EQUAL_INT(HY_MARK_ON_TIME, hy_day_mark(a, 20261027, &in, &s, &prog));
    TEST_ASSERT_EQUAL_INT(HY_MARK_UNTRACKED, hy_day_mark(a, 20261024, &in, &s, &prog));

    hy_med_t *v = add_v();
    v->weekdays = 0x01; /* Monday only */
    in = input(L(8, 0));
    TEST_ASSERT_EQUAL_INT(HY_MARK_NONE, hy_day_mark(v, 20261103, &in, &s, &prog));

    hy_med_t *m = add_m();
    add_log(3, L(9, 0) - 2 * DAY, HY_TAKEN, 0);
    in = input(L(8, 0));
    TEST_ASSERT_EQUAL_INT(HY_MARK_AS_NEEDED_TAKEN, hy_day_mark(m, 20261031, &in, &s, &prog));
    TEST_ASSERT_EQUAL_INT(HY_MARK_NONE, hy_day_mark(m, 20261030, &in, &s, &prog));
}

static void test_day_mark_in_progress(void)
{
    reset();
    hy_med_t *a = add_a();
    track();
    add_log(1, L(6, 2), HY_TAKEN, L(6, 0));
    hy_sched_in_t in = input(L(8, 0));
    bool prog = false;
    hy_mark_t k = hy_day_mark(a, D_TODAY, &in, &s, &prog);
    TEST_ASSERT_EQUAL_INT(HY_MARK_ON_TIME, k);
    TEST_ASSERT_TRUE(prog);
    /* everything resolved by 23:00 */
    add_log(1, L(14, 0), HY_TAKEN, L(14, 0));
    add_log(1, L(22, 0), HY_TAKEN, L(22, 0));
    in = input(L(23, 0));
    prog = true;
    hy_day_mark(a, D_TODAY, &in, &s, &prog);
    TEST_ASSERT_FALSE(prog);
}

static void test_adherence_excludes_untracked_and_open(void)
{
    reset();
    add_v();
    track();
    add_log(2, L(8, 30) - 8 * DAY, HY_TAKEN, L(8, 30) - 8 * DAY); /* 10-25 */
    add_log(2, L(8, 30) - 7 * DAY, HY_TAKEN, L(8, 30) - 7 * DAY); /* 10-26 */
    add_log(2, L(8, 40) - 6 * DAY, HY_TAKEN, L(8, 30) - 6 * DAY); /* 10-27 */
    /* 10-28 missed (no log) */
    add_log(2, L(8, 31) - 4 * DAY, HY_SKIPPED, L(8, 30) - 4 * DAY); /* 10-29 */
    /* 10-30 08:40: slot DUE; 10-23, 10-24 before tracking */
    hy_sched_in_t in = input(L(8, 40) - 3 * DAY);
    hy_adherence_t a;
    memset(&a, 0xFF, sizeof a);
    hy_adherence(2, 20261023, 8, &in, &s, &a);
    TEST_ASSERT_EQUAL_UINT16(3, a.taken);
    TEST_ASSERT_EQUAL_UINT16(5, a.resolved);
    memset(&a, 0xFF, sizeof a);
    hy_adherence(0, 20261023, 8, &in, &s, &a);
    TEST_ASSERT_EQUAL_UINT16(3, a.taken);
    TEST_ASSERT_EQUAL_UINT16(5, a.resolved);
    hy_adherence(99, 20261023, 8, &in, &s, &a);
    TEST_ASSERT_EQUAL_UINT16(0, a.resolved);
}

static void test_tile_no_dose_ring_from_skipped_slot(void)
{
    reset();
    hy_med_t *a = add_a();
    track();
    add_log(1, L(6, 1), HY_SKIPPED, L(6, 0));
    hy_tile_view_t t = tile(a, L(10, 0));
    TEST_ASSERT_EQUAL_INT(HY_TILE_NO_DOSE, t.kind);
    TEST_ASSERT_EQUAL_INT32(500, t.ring); /* (10:00 - 06:00) / (14:00 - 06:00) */
    TEST_ASSERT_EQUAL_INT64(0, t.last_taken);
}

static void test_tile_snoozed_on_older_open_slot(void)
{
    reset();
    hy_med_t *v = add_v();
    v->times[0] = 8 * 60; v->times[1] = 9 * 60; v->n_times = 2; /* closer than missed_after_min */
    track();
    snoozes[n_snoozes].med_id = 2; snoozes[n_snoozes].slot = L(8, 0); snoozes[n_snoozes].until = L(9, 30);
    n_snoozes++;
    hy_tile_view_t t = tile(v, L(9, 10)); /* 08:00 LATE (open, snoozed), 09:00 DUE */
    TEST_ASSERT_EQUAL_INT(HY_TILE_SNOOZED, t.kind);
    TEST_ASSERT_EQUAL_INT64(L(9, 30), t.snoozed_until);
}

static void test_today_rows_tie_break_by_order(void)
{
    reset();
    hy_med_t *v = add_v();   /* id 2, 08:30 */
    v->order = 5;
    hy_med_t *a = add_a();   /* id 1 */
    a->times[0] = 8 * 60 + 30; a->n_times = 1;
    a->order = 0;
    track();
    hy_sched_in_t in = input(L(7, 0));
    hy_today_row_t rows[4];
    TEST_ASSERT_EQUAL_UINT(2, hy_today_rows(&in, &s, rows, 4, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, rows[0].med_id); /* order 0 first although listed second */
    TEST_ASSERT_EQUAL_UINT32(2, rows[1].med_id);
}

/* 2026-03-29, Zurich: 23 h day, 02:30 does not exist and moves to 03:00 CEST */
static void test_rows_and_mark_on_23h_day(void)
{
    reset();
    hy_med_t *a = add_a();
    a->times[0] = 2 * 60 + 30; a->times[1] = 8 * 60; a->n_times = 2;
    const int64_t mid = 1774738800LL; /* 2026-03-28T23:00Z = 03-29 00:00 CET */
    const int64_t s1 = mid + 2 * HOUR, s2 = mid + 7 * HOUR; /* 03:00 CEST, 08:00 CEST */
    track_at(mid - 9 * DAY);
    hy_sched_in_t in = input(mid + 11 * HOUR); /* 12:00 CEST */
    bool prog = true;
    TEST_ASSERT_EQUAL_INT(HY_MARK_MISSED, hy_day_mark(a, 20260329, &in, &s, &prog));
    add_log(1, s1 + 60, HY_TAKEN, s1);
    add_log(1, s2 + 60, HY_TAKEN, s2);
    in = input(mid + 11 * HOUR);
    TEST_ASSERT_EQUAL_INT(HY_MARK_ON_TIME, hy_day_mark(a, 20260329, &in, &s, &prog));
    TEST_ASSERT_FALSE(prog);
    hy_today_row_t rows[4];
    uint16_t taken = 0, total = 0;
    TEST_ASSERT_EQUAL_UINT(2, hy_today_rows(&in, &s, rows, 4, &taken, &total));
    TEST_ASSERT_EQUAL_INT64(s1, rows[0].slot);
    TEST_ASSERT_EQUAL_INT64(s2, rows[1].slot);
    TEST_ASSERT_EQUAL_UINT16(2, taken);
    TEST_ASSERT_EQUAL_UINT16(2, total);
}

/* 2026-10-25, Zurich: 25 h day, 02:30 occurs twice and is scheduled once, at the first occurrence */
static void test_rows_and_mark_on_25h_day(void)
{
    reset();
    hy_med_t *a = add_a();
    a->times[0] = 2 * 60 + 30; a->times[1] = 6 * 60; a->times[2] = 22 * 60; a->n_times = 3;
    const int64_t mid = 1792879200LL; /* 2026-10-24T22:00Z = 10-25 00:00 CEST */
    const int64_t s1 = mid + 2 * HOUR + 30 * MIN; /* 02:30 CEST */
    const int64_t s2 = mid + 7 * HOUR;            /* 06:00 CET */
    const int64_t s3 = mid + 23 * HOUR;           /* 22:00 CET */
    track_at(mid - 7 * DAY);
    add_log(1, s1 + 60, HY_TAKEN, s1);
    add_log(1, s2 + 60, HY_TAKEN, s2);
    add_log(1, s3 + 60, HY_TAKEN, s3);
    hy_sched_in_t in = input(mid + 24 * HOUR + 30 * MIN); /* 23:30 CET */
    bool prog = true;
    TEST_ASSERT_EQUAL_INT(HY_MARK_ON_TIME, hy_day_mark(a, 20261025, &in, &s, &prog));
    TEST_ASSERT_FALSE(prog);
    hy_today_row_t rows[6];
    uint16_t taken = 0, total = 0;
    TEST_ASSERT_EQUAL_UINT(3, hy_today_rows(&in, &s, rows, 6, &taken, &total));
    TEST_ASSERT_EQUAL_INT64(s1, rows[0].slot);
    TEST_ASSERT_EQUAL_INT64(s2, rows[1].slot);
    TEST_ASSERT_EQUAL_INT64(s3, rows[2].slot);
    TEST_ASSERT_EQUAL_UINT16(3, taken);
    TEST_ASSERT_EQUAL_UINT16(3, total);
}

static void test_delta_text(void)
{
    char b[24];
    hy_delta_text(0, b, sizeof b);   TEST_ASSERT_EQUAL_STRING("on time", b);
    hy_delta_text(4, b, sizeof b);   TEST_ASSERT_EQUAL_STRING("on time", b);
    hy_delta_text(-4, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("on time", b);
    hy_delta_text(5, b, sizeof b);   TEST_ASSERT_EQUAL_STRING("+5 min", b);
    hy_delta_text(28, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("+28 min", b);
    hy_delta_text(-12, b, sizeof b); TEST_ASSERT_EQUAL_STRING("\xE2\x88\x92" "12 min", b);
    hy_delta_text(119, b, sizeof b); TEST_ASSERT_EQUAL_STRING("+119 min", b);
    hy_delta_text(125, b, sizeof b); TEST_ASSERT_EQUAL_STRING("+2 h 05 min", b);
    hy_delta_text(-125, b, sizeof b); TEST_ASSERT_EQUAL_STRING("\xE2\x88\x92" "2 h 05 min", b);
    hy_delta_text(125, b, 5);        TEST_ASSERT_EQUAL_STRING("+2 h", b);
}

static void test_local_date_across_dst(void)
{
    /* 2026-10-25 00:30Z, 02:30 CEST (fall-back day) */
    TEST_ASSERT_EQUAL_INT32(20261025, hy_local_date(1792888200LL, TZ_ZURICH));
    TEST_ASSERT_EQUAL_INT32(20261025, hy_local_date(1792888200LL - 2 * HOUR, TZ_ZURICH)); /* 00:30 CEST */
    TEST_ASSERT_EQUAL_INT32(20261024, hy_local_date(1792888200LL - 3 * HOUR, TZ_ZURICH)); /* 23:30 CEST */
    TEST_ASSERT_EQUAL_INT32(20261102, hy_local_date(L(23, 59), TZ_ZURICH));
    TEST_ASSERT_EQUAL_INT32(20261103, hy_local_date(L(24, 0), TZ_ZURICH));
    TEST_ASSERT_EQUAL_INT32(20270101, hy_date_add(20261231, 1));
    TEST_ASSERT_EQUAL_INT32(20240229, hy_date_add(20240301, -1));
    TEST_ASSERT_EQUAL_INT32(20261102, hy_date_add(20261102, 0));
    TEST_ASSERT_EQUAL_INT32(20261025, hy_date_add(20261102, -8));
}

static void test_due_within(void)
{
    reset();
    hy_med_t *a = add_a();
    a->times[0] = 8 * 60; a->n_times = 1;
    TEST_ASSERT_TRUE(hy_due_within(meds, n_meds, TZ_ZURICH, L(7, 0), 24 * HOUR));
    TEST_ASSERT_FALSE(hy_due_within(meds, n_meds, TZ_ZURICH, L(8, 0), HOUR));
    TEST_ASSERT_TRUE(hy_due_within(meds, n_meds, TZ_ZURICH, L(7, 0), HOUR));
    TEST_ASSERT_FALSE(hy_due_within(meds, n_meds, TZ_ZURICH, L(7, 0), HOUR - 1));
    TEST_ASSERT_FALSE(hy_due_within(meds, 0, TZ_ZURICH, L(7, 0), 24 * HOUR));

    reset();
    hy_med_t *m = add_m();
    TEST_ASSERT_EQUAL(HY_SCHED_AS_NEEDED, m->type);
    TEST_ASSERT_FALSE(hy_due_within(meds, n_meds, TZ_ZURICH, L(7, 0), 24 * HOUR));

    reset();
    a = add_a(); a->times[0] = 8 * 60; a->n_times = 1; a->archived = true;
    TEST_ASSERT_FALSE(hy_due_within(meds, n_meds, TZ_ZURICH, L(7, 0), 24 * HOUR));

    reset();
    a = add_a(); a->times[0] = 8 * 60; a->n_times = 1; a->active_until = 20261101;
    TEST_ASSERT_FALSE(hy_due_within(meds, n_meds, TZ_ZURICH, L(7, 0), 24 * HOUR));

    /* 2026-11-02 is a Monday (bit0), 2026-11-03 a Tuesday (bit1): Wednesday and Thursday only */
    reset();
    a = add_a(); a->times[0] = 8 * 60; a->n_times = 1; a->weekdays = (1u << 2) | (1u << 3);
    TEST_ASSERT_FALSE(hy_due_within(meds, n_meds, TZ_ZURICH, L(9, 0), 24 * HOUR));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tile_matches_plan1_cases);
    RUN_TEST(test_tile_as_needed_ring);
    RUN_TEST(test_today_rows_and_count);
    RUN_TEST(test_day_mark_priority);
    RUN_TEST(test_day_mark_in_progress);
    RUN_TEST(test_adherence_excludes_untracked_and_open);
    RUN_TEST(test_tile_no_dose_ring_from_skipped_slot);
    RUN_TEST(test_tile_snoozed_on_older_open_slot);
    RUN_TEST(test_today_rows_tie_break_by_order);
    RUN_TEST(test_rows_and_mark_on_23h_day);
    RUN_TEST(test_rows_and_mark_on_25h_day);
    RUN_TEST(test_delta_text);
    RUN_TEST(test_due_within);
    RUN_TEST(test_local_date_across_dst);
    return UNITY_END();
}
