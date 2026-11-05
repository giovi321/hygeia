#include "unity.h"
#include "hy/cabretry.h"

void setUp(void) {}
void tearDown(void) {}

#define MIN 60000LL

static void test_first_wake_retries(void)
{
    hy_cabretry_t r;
    hy_cabretry_init(&r);
    int64_t w = -1;
    TEST_ASSERT_TRUE(hy_cabretry_wake(&r, 5000, &w));
    TEST_ASSERT_EQUAL_INT64(0, w);
    TEST_ASSERT_EQUAL_UINT8(1, hy_cabretry_count(&r, 5000));
}

static void test_a_second_wake_inside_the_gap_waits(void)
{
    hy_cabretry_t r;
    hy_cabretry_init(&r);
    TEST_ASSERT_TRUE(hy_cabretry_wake(&r, 1000, NULL));
    int64_t w = 0;
    TEST_ASSERT_FALSE(hy_cabretry_wake(&r, 1000 + 20000, &w));
    TEST_ASSERT_EQUAL_INT64(HY_CABRETRY_GAP_MS - 20000, w);
    TEST_ASSERT_EQUAL_UINT8(1, hy_cabretry_count(&r, 30000));   /* the refusal records nothing */
    TEST_ASSERT_TRUE(hy_cabretry_wake(&r, 1000 + HY_CABRETRY_GAP_MS, NULL));
}

static void test_the_hourly_limit(void)
{
    hy_cabretry_t r;
    hy_cabretry_init(&r);
    int64_t t = 0;
    for (int i = 0; i < HY_CABRETRY_PER_HOUR; i++) {
        TEST_ASSERT_TRUE(hy_cabretry_wake(&r, t, NULL));
        t += 2 * MIN;
    }
    int64_t w = 0;
    TEST_ASSERT_FALSE(hy_cabretry_wake(&r, t, &w));
    /* the oldest retry (t = 0) leaves the window at 60 min */
    TEST_ASSERT_EQUAL_INT64(60 * MIN - t, w);
    TEST_ASSERT_FALSE(hy_cabretry_wake(&r, 59 * MIN, NULL));
    TEST_ASSERT_TRUE(hy_cabretry_wake(&r, 60 * MIN, NULL));
    TEST_ASSERT_EQUAL_UINT8(HY_CABRETRY_PER_HOUR, hy_cabretry_count(&r, 60 * MIN));
}

static void test_old_retries_expire(void)
{
    hy_cabretry_t r;
    hy_cabretry_init(&r);
    for (int i = 0; i < HY_CABRETRY_PER_HOUR; i++) {
        TEST_ASSERT_TRUE(hy_cabretry_wake(&r, i * 2 * MIN, NULL));
    }
    TEST_ASSERT_EQUAL_UINT8(0, hy_cabretry_count(&r, 3 * 60 * MIN));
    TEST_ASSERT_TRUE(hy_cabretry_wake(&r, 3 * 60 * MIN, NULL));
}

static void test_a_clock_that_goes_back_does_not_block_for_good(void)
{
    hy_cabretry_t r;
    hy_cabretry_init(&r);
    TEST_ASSERT_TRUE(hy_cabretry_wake(&r, 10 * MIN, NULL));
    TEST_ASSERT_TRUE(hy_cabretry_wake(&r, 1 * MIN, NULL));
}

static void test_null_wait_is_accepted(void)
{
    hy_cabretry_t r;
    hy_cabretry_init(&r);
    TEST_ASSERT_TRUE(hy_cabretry_wake(&r, 0, NULL));
    TEST_ASSERT_FALSE(hy_cabretry_wake(&r, 1, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_wake_retries);
    RUN_TEST(test_a_second_wake_inside_the_gap_waits);
    RUN_TEST(test_the_hourly_limit);
    RUN_TEST(test_old_retries_expire);
    RUN_TEST(test_a_clock_that_goes_back_does_not_block_for_good);
    RUN_TEST(test_null_wait_is_accepted);
    return UNITY_END();
}
