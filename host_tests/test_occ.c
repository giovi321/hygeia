#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "unity.h"
#include "hy/occ.h"
#include "hy/med.h"

#define TZ_ZURICH "CET-1CEST,M3.5.0,M10.5.0/3"
#define TZ_NY     "EST5EDT,M3.2.0,M11.1.0"

/* Literal UTC epochs (computed independently with `date -u -d ... +%s`). */
#define T_2026_11_02_LOCAL_MIDNIGHT 1793574000LL /* 2026-11-01T23:00:00Z */
#define T_2026_11_02_0700Z          1793602800LL
#define T_2026_03_29_LOCAL_MIDNIGHT 1774738800LL /* 2026-03-28T23:00:00Z */
#define T_2026_03_29_0100Z          1774746000LL
#define T_2026_10_25_LOCAL_MIDNIGHT 1792879200LL /* 2026-10-24T22:00:00Z */
#define T_2026_10_25_0030Z          1792888200LL
#define T_2026_07_15_0600Z          1784095200LL /* 08:00 CEST Zurich */
#define T_2026_07_15_1000Z          1784109600LL /* 06:00 EDT New York, 12:00 CEST Zurich */
#define T_2026_11_02_1300Z          1793624400LL /* 08:00 EST New York */
#define T_2026_11_03_1300Z          1793710800LL /* 08:00 EST New York */
#define T_2026_10_27_2259Z          1793141940LL /* Tue 27 Oct 23:59 CET Zurich */
#define DAY 86400LL
#define MIN 60LL
#define HOUR 3600LL

void setUp(void) {}
void tearDown(void) {}

static void make_med(hy_med_t *m, uint16_t t0, uint16_t t1)
{
    hy_med_defaults(m);
    m->id = 1;
    strcpy(m->code, "A");
    strcpy(m->name, "Antibiotic");
    m->type = HY_SCHED_FIXED;
    m->times[0] = t0;
    m->n_times = 1;
    if (t1 != 0xFFFF) {
        m->times[1] = t1;
        m->n_times = 2;
    }
}

static void test_daily_times_zurich(void)
{
    hy_med_t m;
    int64_t out[16];
    make_med(&m, 8 * 60, 20 * 60);
    size_t n = hy_occurrences(&m, T_2026_11_02_LOCAL_MIDNIGHT,
                              T_2026_11_02_LOCAL_MIDNIGHT + 2 * DAY, TZ_ZURICH, out, 16);
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z, out[0]);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z + 12 * 3600, out[1]);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z + DAY, out[2]);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z + DAY + 12 * 3600, out[3]);
}

static void test_unsorted_times_sorted_output(void)
{
    hy_med_t m;
    int64_t out[16];
    make_med(&m, 20 * 60, 8 * 60);
    size_t n = hy_occurrences(&m, T_2026_11_02_LOCAL_MIDNIGHT,
                              T_2026_11_02_LOCAL_MIDNIGHT + DAY, TZ_ZURICH, out, 16);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z, out[0]);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z + 12 * 3600, out[1]);
}

static void test_range_is_half_open_and_max(void)
{
    hy_med_t m;
    int64_t out[16];
    make_med(&m, 8 * 60, 0xFFFF);
    /* from == occurrence is included, to == occurrence is excluded */
    size_t n = hy_occurrences(&m, T_2026_11_02_0700Z, T_2026_11_02_0700Z + DAY, TZ_ZURICH, out, 16);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z, out[0]);
    /* max truncates, keeping the earliest */
    n = hy_occurrences(&m, T_2026_11_02_LOCAL_MIDNIGHT, T_2026_11_02_LOCAL_MIDNIGHT + 7 * DAY,
                       TZ_ZURICH, out, 3);
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z + 2 * DAY, out[2]);
}

static void test_weekdays_mask(void)
{
    hy_med_t m;
    int64_t out[32];
    make_med(&m, 8 * 60, 0xFFFF);
    m.weekdays = 0x01; /* Monday only; 2026-11-02 is a Monday */
    size_t n = hy_occurrences(&m, T_2026_11_02_LOCAL_MIDNIGHT,
                              T_2026_11_02_LOCAL_MIDNIGHT + 14 * DAY, TZ_ZURICH, out, 32);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z, out[0]);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z + 7 * DAY, out[1]);

    m.weekdays = 0x40; /* Sunday only: 2026-11-08 */
    n = hy_occurrences(&m, T_2026_11_02_LOCAL_MIDNIGHT,
                       T_2026_11_02_LOCAL_MIDNIGHT + 14 * DAY, TZ_ZURICH, out, 32);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z + 6 * DAY, out[0]);
}

static void test_active_range(void)
{
    hy_med_t m;
    int64_t out[32];
    make_med(&m, 8 * 60, 0xFFFF);
    m.active_from = 20261103;
    m.active_until = 20261104;
    size_t n = hy_occurrences(&m, T_2026_11_02_LOCAL_MIDNIGHT,
                              T_2026_11_02_LOCAL_MIDNIGHT + 7 * DAY, TZ_ZURICH, out, 32);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z + DAY, out[0]);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z + 2 * DAY, out[1]);
}

static void test_archived_has_none(void)
{
    hy_med_t m;
    int64_t out[8];
    make_med(&m, 8 * 60, 0xFFFF);
    m.archived = true;
    TEST_ASSERT_EQUAL_UINT(0, hy_occurrences(&m, T_2026_11_02_LOCAL_MIDNIGHT,
                                             T_2026_11_02_LOCAL_MIDNIGHT + 7 * DAY,
                                             TZ_ZURICH, out, 8));
}

static void test_dst_spring_gap(void)
{
    hy_med_t m;
    int64_t out[8];
    make_med(&m, 2 * 60 + 30, 0xFFFF); /* 02:30 does not exist on 2026-03-29 in Zurich */
    size_t n = hy_occurrences(&m, T_2026_03_29_LOCAL_MIDNIGHT,
                              T_2026_03_29_LOCAL_MIDNIGHT + DAY - 3600, TZ_ZURICH, out, 8);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_INT64(T_2026_03_29_0100Z, out[0]); /* 03:00 CEST */
}

static void test_dst_fall_repeat(void)
{
    hy_med_t m;
    int64_t out[8];
    make_med(&m, 2 * 60 + 30, 0xFFFF); /* 02:30 occurs twice on 2026-10-25 in Zurich */
    size_t n = hy_occurrences(&m, T_2026_10_25_LOCAL_MIDNIGHT,
                              T_2026_10_25_LOCAL_MIDNIGHT + DAY + 3600, TZ_ZURICH, out, 8);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_INT64(T_2026_10_25_0030Z, out[0]); /* first 02:30, CEST */
}

static void test_as_needed_has_none(void)
{
    hy_med_t m;
    int64_t out[8];
    make_med(&m, 8 * 60, 0xFFFF);
    m.type = HY_SCHED_AS_NEEDED;
    TEST_ASSERT_EQUAL_UINT(0, hy_occurrences(&m, T_2026_11_02_LOCAL_MIDNIGHT,
                                             T_2026_11_02_LOCAL_MIDNIGHT + 7 * DAY,
                                             TZ_ZURICH, out, 8));
}

static void test_other_zone_new_york(void)
{
    hy_med_t m;
    int64_t out[8];
    make_med(&m, 8 * 60, 0xFFFF);
    /* 2026-11-02 08:00 EST = 13:00Z */
    size_t n = hy_occurrences(&m, T_2026_11_02_0700Z, T_2026_11_02_0700Z + DAY, TZ_NY, out, 8);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z + 6 * 3600, out[0]);
}

static void test_restores_tz(void)
{
    hy_med_t m;
    int64_t out[8];
    make_med(&m, 8 * 60, 0xFFFF);

    setenv("TZ", "UTC0", 1);
    tzset();
    hy_occurrences(&m, T_2026_11_02_LOCAL_MIDNIGHT, T_2026_11_02_LOCAL_MIDNIGHT + DAY,
                   TZ_ZURICH, out, 8);
    TEST_ASSERT_NOT_NULL(getenv("TZ"));
    TEST_ASSERT_EQUAL_STRING("UTC0", getenv("TZ"));

    unsetenv("TZ");
    tzset();
    hy_occurrences(&m, T_2026_11_02_LOCAL_MIDNIGHT, T_2026_11_02_LOCAL_MIDNIGHT + DAY,
                   TZ_ZURICH, out, 8);
    TEST_ASSERT_NULL(getenv("TZ"));
}

/* ---- slot states, dose matching, travel rules ---- */

static void make_log(hy_log_t *l, uint32_t id, uint32_t med_id, int64_t ts, const char *tz,
                     hy_status_t status, int64_t slot)
{
    memset(l, 0, sizeof *l);
    l->id = id;
    l->med_id = med_id;
    l->ts = ts;
    strcpy(l->tz, tz);
    l->status = status;
    l->slot = slot;
}

/* 2026-11-02 (Monday), Zurich CET: 08:00 local = 07:00Z. */
static void test_states_over_time(void)
{
    hy_med_t m;
    make_med(&m, 8 * 60, 0xFFFF);
    int64_t s = T_2026_11_02_0700Z, next = s + DAY;
    TEST_ASSERT_EQUAL_INT(HY_OCC_UPCOMING,
                          hy_occ_state(&m, s, next, s - MIN, TZ_ZURICH, NULL, 0, NULL, 0, NULL));
    TEST_ASSERT_EQUAL_INT(HY_OCC_DUE,
                          hy_occ_state(&m, s, next, s, TZ_ZURICH, NULL, 0, NULL, 0, NULL));
    TEST_ASSERT_EQUAL_INT(HY_OCC_DUE,
                          hy_occ_state(&m, s, next, s + 30 * MIN - 1, TZ_ZURICH, NULL, 0, NULL, 0, NULL));
    TEST_ASSERT_EQUAL_INT(HY_OCC_LATE,
                          hy_occ_state(&m, s, next, s + 30 * MIN, TZ_ZURICH, NULL, 0, NULL, 0, NULL));
    TEST_ASSERT_EQUAL_INT(HY_OCC_MISSED,
                          hy_occ_state(&m, s, next, s + 120 * MIN, TZ_ZURICH, NULL, 0, NULL, 0, NULL));
}

/* 2026-11-02 Zurich; active_from is that day so no previous-day slot exists. */
static void test_early_window(void)
{
    hy_med_t m;
    make_med(&m, 8 * 60, 0xFFFF);
    m.active_from = 20261102;
    int64_t s = T_2026_11_02_0700Z;
    int64_t ts = s - 60 * MIN; /* 07:00 local */
    TEST_ASSERT_EQUAL_INT64(s, hy_match_slot(&m, ts, TZ_ZURICH, NULL, 0));
    TEST_ASSERT_EQUAL_INT64(-60, (ts - s) / 60);
    TEST_ASSERT_EQUAL_INT64(0, hy_match_slot(&m, s - 61 * MIN, TZ_ZURICH, NULL, 0)); /* 06:59 */
}

/* 2026-11-02 Zurich, slots 08:00 and 14:00. */
static void test_earliest_unsatisfied(void)
{
    hy_med_t m;
    hy_log_t logs[1];
    make_med(&m, 8 * 60, 14 * 60);
    int64_t s8 = T_2026_11_02_0700Z, s14 = s8 + 6 * HOUR;
    TEST_ASSERT_EQUAL_INT64(s8, hy_match_slot(&m, s8 + 5 * MIN, TZ_ZURICH, NULL, 0));
    make_log(&logs[0], 1, m.id, s8 + 5 * MIN, TZ_ZURICH, HY_TAKEN, s8);
    TEST_ASSERT_EQUAL_INT64(0, hy_match_slot(&m, s8 + 10 * MIN, TZ_ZURICH, logs, 1));
    TEST_ASSERT_EQUAL_INT64(s14, hy_match_slot(&m, s8 + 5 * HOUR + 30 * MIN, TZ_ZURICH, logs, 1));
}

/* 2026-11-02 Zurich, slots 08:00 and 20:00. */
static void test_taken_after_missed(void)
{
    hy_med_t m;
    hy_log_t logs[2];
    make_med(&m, 8 * 60, 20 * 60);
    int64_t s = T_2026_11_02_0700Z, next = s + 12 * HOUR;
    make_log(&logs[0], 1, m.id, s + 2 * HOUR, TZ_ZURICH, HY_MISSED, s);
    /* the missed log does not satisfy the slot, so an 11:00 dose still matches it */
    TEST_ASSERT_EQUAL_INT64(s, hy_match_slot(&m, s + 3 * HOUR, TZ_ZURICH, logs, 1));
    TEST_ASSERT_EQUAL_INT(HY_OCC_MISSED,
                          hy_occ_state(&m, s, next, s + 3 * HOUR, TZ_ZURICH, logs, 1, NULL, 0, NULL));
    make_log(&logs[1], 2, m.id, s + 3 * HOUR, TZ_ZURICH, HY_TAKEN, s);
    TEST_ASSERT_EQUAL_INT(HY_OCC_TAKEN,
                          hy_occ_state(&m, s, next, s + 3 * HOUR, TZ_ZURICH, logs, 2, NULL, 0, NULL));
}

/* Skipped log gives SKIPPED; a taken log for the same slot wins. 2026-11-02 Zurich. */
static void test_skipped_state(void)
{
    hy_med_t m;
    hy_log_t logs[2];
    make_med(&m, 8 * 60, 0xFFFF);
    int64_t s = T_2026_11_02_0700Z;
    make_log(&logs[0], 1, m.id, s + 10 * MIN, TZ_ZURICH, HY_SKIPPED, s);
    TEST_ASSERT_EQUAL_INT(HY_OCC_SKIPPED,
                          hy_occ_state(&m, s, s + DAY, s + 3 * HOUR, TZ_ZURICH, logs, 1, NULL, 0, NULL));
    make_log(&logs[1], 2, m.id, s + 20 * MIN, TZ_ZURICH, HY_TAKEN, s);
    TEST_ASSERT_EQUAL_INT(HY_OCC_TAKEN,
                          hy_occ_state(&m, s, s + DAY, s + 3 * HOUR, TZ_ZURICH, logs, 2, NULL, 0, NULL));
    /* logs for another medicine do not count */
    logs[0].med_id = m.id + 1;
    logs[1].med_id = m.id + 1;
    TEST_ASSERT_EQUAL_INT(HY_OCC_MISSED,
                          hy_occ_state(&m, s, s + DAY, s + 3 * HOUR, TZ_ZURICH, logs, 2, NULL, 0, NULL));
}

/* 2026-11-02 Zurich, 08:00 slot, window closes at 10:00 local. */
static void test_untracked_gap(void)
{
    hy_med_t m;
    make_med(&m, 8 * 60, 0xFFFF);
    int64_t s = T_2026_11_02_0700Z;
    hy_gap_t g1 = { s - HOUR, s + 4 * HOUR }; /* 07:00-12:00 local */
    TEST_ASSERT_EQUAL_INT(HY_OCC_UNTRACKED,
                          hy_occ_state(&m, s, s + DAY, s + 5 * HOUR, TZ_ZURICH, NULL, 0, &g1, 1, NULL));
    hy_gap_t g2 = { s - HOUR, s + HOUR };     /* 07:00-09:00 local */
    TEST_ASSERT_EQUAL_INT(HY_OCC_LATE,
                          hy_occ_state(&m, s, s + DAY, s + HOUR, TZ_ZURICH, NULL, 0, &g2, 1, NULL));
}

/* 2026-07-15: change at 10:00Z from New York (06:00 EDT) to Zurich (12:00 CEST).
 * The Zurich 08:00 slot (06:00Z) lies in the skipped wall-clock interval. */
static void test_east_jump_due_on_arrival(void)
{
    hy_med_t m;
    make_med(&m, 8 * 60, 0xFFFF);
    hy_tzchange_t ch;
    memset(&ch, 0, sizeof ch);
    ch.at = T_2026_07_15_1000Z;
    strcpy(ch.old_tz, TZ_NY);
    strcpy(ch.new_tz, TZ_ZURICH);
    int64_t s = T_2026_07_15_0600Z;
    TEST_ASSERT_EQUAL_INT(HY_OCC_DUE,
                          hy_occ_state(&m, s, s + DAY, ch.at, TZ_ZURICH, NULL, 0, NULL, 0, &ch));
    TEST_ASSERT_EQUAL_INT(HY_OCC_LATE,
                          hy_occ_state(&m, s, s + DAY, ch.at + 30 * MIN, TZ_ZURICH, NULL, 0, NULL, 0, &ch));
    TEST_ASSERT_EQUAL_INT(HY_OCC_MISSED,
                          hy_occ_state(&m, s, s + DAY, ch.at, TZ_ZURICH, NULL, 0, NULL, 0, NULL));
}

/* 2026-11-02: taken 08:05 Zurich for the 08:00 slot, then change to New York;
 * the New York 08:00 slot of the same calendar day is already taken. */
static void test_west_no_repeat(void)
{
    hy_med_t m;
    hy_log_t logs[1];
    make_med(&m, 8 * 60, 0xFFFF);
    make_log(&logs[0], 1, m.id, T_2026_11_02_0700Z + 5 * MIN, TZ_ZURICH, HY_TAKEN,
             T_2026_11_02_0700Z);
    int64_t ny = T_2026_11_02_1300Z;
    TEST_ASSERT_EQUAL_INT(HY_OCC_TAKEN,
                          hy_occ_state(&m, ny, ny + DAY, ny, TZ_NY, logs, 1, NULL, 0, NULL));
}

/* West rule negative: the New York 08:00 slot of the next day (2026-11-03) is not taken. */
static void test_west_rule_other_day(void)
{
    hy_med_t m;
    hy_log_t logs[1];
    make_med(&m, 8 * 60, 0xFFFF);
    make_log(&logs[0], 1, m.id, T_2026_11_02_0700Z + 5 * MIN, TZ_ZURICH, HY_TAKEN,
             T_2026_11_02_0700Z);
    int64_t ny = T_2026_11_03_1300Z;
    TEST_ASSERT_EQUAL_INT(HY_OCC_DUE,
                          hy_occ_state(&m, ny, ny + DAY, ny, TZ_NY, logs, 1, NULL, 0, NULL));
}

/* As-needed, dated 2026-11-02. */
static void test_min_interval(void)
{
    hy_med_t m;
    hy_log_t logs[1];
    make_med(&m, 8 * 60, 0xFFFF);
    m.type = HY_SCHED_AS_NEEDED;
    m.n_times = 0;
    m.min_interval_min = 240;
    int64_t now = T_2026_11_02_0700Z;
    int32_t since = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_min_interval_check(&m, now, NULL, 0, &since));
    TEST_ASSERT_EQUAL_INT32(-1, since);
    make_log(&logs[0], 1, m.id, now - 120 * MIN, TZ_ZURICH, HY_TAKEN, 0);
    TEST_ASSERT_EQUAL_INT(HY_W_TOO_SOON, hy_min_interval_check(&m, now, logs, 1, &since));
    TEST_ASSERT_EQUAL_INT32(120, since);
    logs[0].ts = now - 240 * MIN;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_min_interval_check(&m, now, logs, 1, &since));
    TEST_ASSERT_EQUAL_INT32(240, since);
}

/* 2026-10-27 22:59Z = Tue 27 Oct 23:59 CET in Zurich, 18:59 EDT in New York. */
static void test_format_local_uses_log_tz(void)
{
    char buf[32];
    setenv("TZ", TZ_NY, 1);
    tzset();
    hy_format_local(T_2026_10_27_2259Z, TZ_ZURICH, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("Tue 27 Oct 23:59", buf);
    TEST_ASSERT_EQUAL_STRING(TZ_NY, getenv("TZ"));
    hy_format_local(T_2026_10_27_2259Z, TZ_ZURICH, buf, 6);
    TEST_ASSERT_EQUAL_STRING("Tue 2", buf);
    unsetenv("TZ");
    tzset();
}

static void test_match_free_for_as_needed_and_archived(void)
{
    hy_med_t m;
    make_med(&m, 8 * 60, 0xFFFF);
    m.type = HY_SCHED_AS_NEEDED;
    TEST_ASSERT_EQUAL_INT64(0, hy_match_slot(&m, T_2026_11_02_0700Z, TZ_ZURICH, NULL, 0));
    make_med(&m, 8 * 60, 0xFFFF);
    m.archived = true;
    TEST_ASSERT_EQUAL_INT64(0, hy_match_slot(&m, T_2026_11_02_0700Z, TZ_ZURICH, NULL, 0));
}

/* A saved TZ too long for the fixed save buffer is never clobbered. */
static void test_long_tz_left_untouched(void)
{
    char longtz[101];
    memset(longtz, 'X', 100);
    longtz[100] = '\0';
    memcpy(longtz, "UTC0", 4);
    hy_med_t m;
    int64_t out[8];
    make_med(&m, 8 * 60, 0xFFFF);
    setenv("TZ", longtz, 1);
    tzset();
    hy_occurrences(&m, T_2026_11_02_LOCAL_MIDNIGHT, T_2026_11_02_LOCAL_MIDNIGHT + DAY,
                   TZ_ZURICH, out, 8);
    TEST_ASSERT_EQUAL_STRING(longtz, getenv("TZ"));
    unsetenv("TZ");
    tzset();
}

/* Daily 08:00 Zurich; yesterday's (2026-11-01, CET) 08:00 slot has a MISSED log.
 * A dose today (2026-11-02) at 07:45 goes to today's 08:00 slot. */
static void test_match_prefers_upcoming_over_missed_daily(void)
{
    hy_med_t m;
    hy_log_t logs[1];
    make_med(&m, 8 * 60, 0xFFFF);
    int64_t today = T_2026_11_02_0700Z, yday = today - DAY;
    make_log(&logs[0], 1, m.id, yday + 2 * HOUR, TZ_ZURICH, HY_MISSED, yday);
    TEST_ASSERT_EQUAL_INT64(today, hy_match_slot(&m, today - 15 * MIN, TZ_ZURICH, logs, 1));
}

/* 2026-11-02 Zurich, slots 08:00 and 14:00, the 08:00 slot MISSED. A dose at
 * 13:30 goes to 14:00; a dose at 11:00 (before 14:00's early window) still
 * converts the missed 08:00. */
static void test_match_prefers_upcoming_over_missed_same_day(void)
{
    hy_med_t m;
    hy_log_t logs[1];
    make_med(&m, 8 * 60, 14 * 60);
    int64_t s8 = T_2026_11_02_0700Z, s14 = s8 + 6 * HOUR;
    make_log(&logs[0], 1, m.id, s8 + 2 * HOUR, TZ_ZURICH, HY_MISSED, s8);
    TEST_ASSERT_EQUAL_INT64(s14, hy_match_slot(&m, s8 + 5 * HOUR + 30 * MIN, TZ_ZURICH, logs, 1));
    TEST_ASSERT_EQUAL_INT64(s8, hy_match_slot(&m, s8 + 3 * HOUR, TZ_ZURICH, logs, 1));
}

/* Same slots, missed_after_min 360 (08:00 deadline 14:00): at 13:30 the 08:00
 * slot is not yet missed, so the earliest unsatisfied slot still wins. */
static void test_match_earliest_wins_before_missed_deadline(void)
{
    hy_med_t m;
    make_med(&m, 8 * 60, 14 * 60);
    m.missed_after_min = 360;
    int64_t s8 = T_2026_11_02_0700Z;
    TEST_ASSERT_EQUAL_INT64(s8, hy_match_slot(&m, s8 + 5 * HOUR + 30 * MIN, TZ_ZURICH, NULL, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_daily_times_zurich);
    RUN_TEST(test_unsorted_times_sorted_output);
    RUN_TEST(test_range_is_half_open_and_max);
    RUN_TEST(test_weekdays_mask);
    RUN_TEST(test_active_range);
    RUN_TEST(test_archived_has_none);
    RUN_TEST(test_dst_spring_gap);
    RUN_TEST(test_dst_fall_repeat);
    RUN_TEST(test_as_needed_has_none);
    RUN_TEST(test_other_zone_new_york);
    RUN_TEST(test_restores_tz);
    RUN_TEST(test_states_over_time);
    RUN_TEST(test_early_window);
    RUN_TEST(test_earliest_unsatisfied);
    RUN_TEST(test_taken_after_missed);
    RUN_TEST(test_skipped_state);
    RUN_TEST(test_untracked_gap);
    RUN_TEST(test_east_jump_due_on_arrival);
    RUN_TEST(test_west_no_repeat);
    RUN_TEST(test_west_rule_other_day);
    RUN_TEST(test_min_interval);
    RUN_TEST(test_format_local_uses_log_tz);
    RUN_TEST(test_match_free_for_as_needed_and_archived);
    RUN_TEST(test_long_tz_left_untouched);
    RUN_TEST(test_match_prefers_upcoming_over_missed_daily);
    RUN_TEST(test_match_prefers_upcoming_over_missed_same_day);
    RUN_TEST(test_match_earliest_wins_before_missed_deadline);
    return UNITY_END();
}
