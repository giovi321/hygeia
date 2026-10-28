#include "unity.h"
#include "hy/clock_valid.h"

#define BUILD 1761600000LL /* arbitrary build epoch for the tests, UTC seconds */
#define DAY 86400LL

void setUp(void) {}
void tearDown(void) {}

static void test_osc_stopped_is_implausible(void)
{
    TEST_ASSERT_FALSE(hy_clock_is_plausible(BUILD + DAY, true, BUILD));
}

static void test_before_build_epoch_is_implausible(void)
{
    TEST_ASSERT_FALSE(hy_clock_is_plausible(BUILD - 1, false, BUILD));
    TEST_ASSERT_FALSE(hy_clock_is_plausible(0, false, BUILD));
}

static void test_equal_to_build_epoch_is_plausible(void)
{
    TEST_ASSERT_TRUE(hy_clock_is_plausible(BUILD, false, BUILD));
}

static void test_after_build_epoch_without_flag_is_plausible(void)
{
    TEST_ASSERT_TRUE(hy_clock_is_plausible(BUILD + DAY, false, BUILD));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_osc_stopped_is_implausible);
    RUN_TEST(test_before_build_epoch_is_implausible);
    RUN_TEST(test_equal_to_build_epoch_is_plausible);
    RUN_TEST(test_after_build_epoch_without_flag_is_plausible);
    return UNITY_END();
}
