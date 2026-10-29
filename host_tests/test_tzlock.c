#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "unity.h"
#include "hy/med.h"
#include "hy/occ.h"
#include "hy/tzlock.h"

#define TZ_ZURICH "CET-1CEST,M3.5.0,M10.5.0/3"
#define TZ_NY     "EST5EDT,M3.2.0,M11.1.0"
#define T_2026_11_02_0700Z 1793602800LL
#define DAY 86400LL

/* Counting hooks: depth goes up on lock and down on unlock. */
static struct {
    int depth;
    int max_depth;
    int total;
    char tz_at_unlock[64];
} h;

static void hook_lock(void *ctx)
{
    (void)ctx;
    h.depth++;
    h.total++;
    if (h.depth > h.max_depth) h.max_depth = h.depth;
}

static void hook_unlock(void *ctx)
{
    (void)ctx;
    h.depth--;
    const char *tz = getenv("TZ");
    strncpy(h.tz_at_unlock, tz ? tz : "(unset)", sizeof h.tz_at_unlock - 1);
    h.tz_at_unlock[sizeof h.tz_at_unlock - 1] = '\0';
}

static void reset_hooks(void)
{
    memset(&h, 0, sizeof h);
}

void setUp(void)
{
    reset_hooks();
    hy_tz_set_lock(hook_lock, hook_unlock, NULL);
}

void tearDown(void)
{
    hy_tz_set_lock(NULL, NULL, NULL);
}

static void make_med(hy_med_t *m)
{
    hy_med_defaults(m);
    m->id = 1;
    strcpy(m->code, "A");
    strcpy(m->name, "Antibiotic");
    m->type = HY_SCHED_FIXED;
    m->times[0] = 8 * 60;
    m->n_times = 1;
}

#define ASSERT_LOCKED_AND_BALANCED(what)                       \
    do {                                                       \
        TEST_ASSERT_TRUE_MESSAGE(h.total >= 1, what);          \
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, h.depth, what);       \
        reset_hooks();                                         \
    } while (0)

static void test_occ_functions_take_the_lock(void)
{
    hy_med_t m;
    make_med(&m);
    int64_t out[8];
    hy_log_t log;
    memset(&log, 0, sizeof log);
    log.id = 1;
    log.med_id = 1;
    log.ts = T_2026_11_02_0700Z;
    log.slot = T_2026_11_02_0700Z;
    log.status = HY_TAKEN;
    strcpy(log.tz, TZ_NY);
    hy_tzchange_t ch = { .at = T_2026_11_02_0700Z - DAY, .old_tz = TZ_ZURICH, .new_tz = TZ_NY };
    char buf[32];

    hy_occurrences(&m, T_2026_11_02_0700Z - DAY, T_2026_11_02_0700Z + DAY, TZ_ZURICH, out, 8);
    ASSERT_LOCKED_AND_BALANCED("hy_occurrences");

    hy_prev_occurrence(&m, T_2026_11_02_0700Z + 60, TZ_ZURICH);
    ASSERT_LOCKED_AND_BALANCED("hy_prev_occurrence");

    hy_match_slot(&m, T_2026_11_02_0700Z, TZ_ZURICH, NULL, 0, NULL);
    ASSERT_LOCKED_AND_BALANCED("hy_match_slot");

    hy_occ_state(&m, T_2026_11_02_0700Z + DAY, 0, T_2026_11_02_0700Z + DAY, TZ_ZURICH,
                 &log, 1, NULL, 0, &ch);
    ASSERT_LOCKED_AND_BALANCED("hy_occ_state");

    /* hy_min_interval_check works on UTC instants only and touches no TZ: it must leave the
     * lock balanced; it has no reason to take it. */
    int32_t since;
    hy_min_interval_check(&m, T_2026_11_02_0700Z + 60, &log, 1, &since);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, h.depth, "hy_min_interval_check");
    reset_hooks();

    hy_format_local(T_2026_11_02_0700Z, TZ_ZURICH, buf, sizeof buf);
    ASSERT_LOCKED_AND_BALANCED("hy_format_local");
}

static void test_tz_restored_inside_lock(void)
{
    setenv("TZ", "UTC0", 1);
    char buf[32];
    hy_format_local(T_2026_11_02_0700Z, TZ_NY, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("Mon 02 Nov 02:00", buf);
    TEST_ASSERT_EQUAL_STRING("UTC0", getenv("TZ"));
    TEST_ASSERT_EQUAL_INT(1, h.total);
    TEST_ASSERT_EQUAL_INT(0, h.depth);
    /* The last unlock saw the restored TZ, so the restore happened inside the lock. */
    TEST_ASSERT_EQUAL_STRING("UTC0", h.tz_at_unlock);
}

/* The zone already in force is not set again: the environment entry stays the caller's own string
 * (putenv), which a setenv would replace. A different zone is still applied and restored. */
static void test_same_zone_leaves_env_alone(void)
{
    static char env_tz[] = "TZ=" TZ_ZURICH;
    putenv(env_tz);
    tzset();
    const char *before = getenv("TZ");
    char buf[32];
    hy_format_local(T_2026_11_02_0700Z, TZ_ZURICH, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("Mon 02 Nov 08:00", buf);
    TEST_ASSERT_EQUAL_PTR(before, getenv("TZ"));
    hy_format_local(T_2026_11_02_0700Z, TZ_NY, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("Mon 02 Nov 02:00", buf);
    TEST_ASSERT_EQUAL_STRING(TZ_ZURICH, getenv("TZ"));
    TEST_ASSERT_EQUAL_INT(0, h.depth);
    setenv("TZ", "UTC0", 1);   /* stop using the static string */
    tzset();
}

static void test_no_lock_registered_is_safe(void)
{
    hy_tz_set_lock(NULL, NULL, NULL);
    hy_tz_lock();   /* no-ops */
    hy_tz_unlock();
    hy_med_t m;
    make_med(&m);
    int64_t out[8];
    size_t n = hy_occurrences(&m, T_2026_11_02_0700Z, T_2026_11_02_0700Z + DAY, TZ_ZURICH, out, 8);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_INT64(T_2026_11_02_0700Z, out[0]);
    char buf[32];
    hy_format_local(T_2026_11_02_0700Z, TZ_ZURICH, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("Mon 02 Nov 08:00", buf);
    TEST_ASSERT_EQUAL_INT(0, h.total);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_occ_functions_take_the_lock);
    RUN_TEST(test_tz_restored_inside_lock);
    RUN_TEST(test_same_zone_leaves_env_alone);
    RUN_TEST(test_no_lock_registered_is_safe);
    return UNITY_END();
}
