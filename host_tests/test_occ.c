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
#define DAY 86400LL

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
    return UNITY_END();
}
