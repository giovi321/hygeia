#include "unity.h"
#include "hy/types.h"

void setUp(void) {}
void tearDown(void) {}

static void test_defaults_match_spec(void)
{
    hy_med_t m;
    hy_med_defaults(&m);
    TEST_ASSERT_EQUAL_HEX8(0x7F, m.weekdays);
    TEST_ASSERT_EQUAL_UINT16(60, m.early_window_min);
    TEST_ASSERT_EQUAL_UINT16(30, m.late_after_min);
    TEST_ASSERT_EQUAL_UINT16(120, m.missed_after_min);
    TEST_ASSERT_EQUAL_INT(HY_SCHED_FIXED, m.type);
    TEST_ASSERT_EQUAL_UINT8(0, m.n_times);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_match_spec);
    return UNITY_END();
}
