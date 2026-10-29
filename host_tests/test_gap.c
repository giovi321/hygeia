#include "unity.h"
#include "hy/gap.h"

void setUp(void) {}
void tearDown(void) {}

static void test_gap_on_start_clean_off(void)
{
    hy_alive_t a = { 1000, true };
    hy_gap_t g = { 0, 0 };
    hy_gap_reason_t why = HY_GAP_JUMP;
    TEST_ASSERT_TRUE(hy_gap_on_start(&a, 5000, false, &g, &why));
    TEST_ASSERT_EQUAL_INT64(1000, g.start);
    TEST_ASSERT_EQUAL_INT64(5000, g.end);
    TEST_ASSERT_EQUAL(HY_GAP_OFF, why);
}

static void test_gap_on_start_lost(void)
{
    hy_alive_t a = { 1000, false };
    hy_gap_t g;
    hy_gap_reason_t why = HY_GAP_OFF;
    TEST_ASSERT_TRUE(hy_gap_on_start(&a, 5000, false, &g, &why));
    TEST_ASSERT_EQUAL(HY_GAP_LOST, why);
    TEST_ASSERT_TRUE(hy_gap_on_start(&a, 5000, true, &g, &why));
    TEST_ASSERT_EQUAL(HY_GAP_CLOCK, why);
    a.clean = true;
    TEST_ASSERT_TRUE(hy_gap_on_start(&a, 5000, true, &g, &why));
    TEST_ASSERT_EQUAL(HY_GAP_CLOCK, why);
}

static void test_gap_on_start_none(void)
{
    hy_gap_t g;
    hy_gap_reason_t why;
    hy_alive_t a = { 0, true };
    TEST_ASSERT_FALSE(hy_gap_on_start(&a, 5000, false, &g, &why));
    a.alive = 1000;
    TEST_ASSERT_FALSE(hy_gap_on_start(&a, 1000 + 119, false, &g, &why));
    TEST_ASSERT_TRUE(hy_gap_on_start(&a, 1000 + 120, false, &g, &why));
    TEST_ASSERT_EQUAL_INT64(1120, g.end);
    TEST_ASSERT_FALSE(hy_gap_on_start(&a, 900, false, &g, &why));
    TEST_ASSERT_FALSE(hy_gap_on_start(NULL, 5000, false, &g, &why));
}

static void test_gap_on_jump(void)
{
    hy_gap_t g = { 0, 0 };
    TEST_ASSERT_TRUE(hy_gap_on_jump(1000, 1200, &g));
    TEST_ASSERT_EQUAL_INT64(1000, g.start);
    TEST_ASSERT_EQUAL_INT64(1200, g.end);
    TEST_ASSERT_FALSE(hy_gap_on_jump(1000, 1100, &g));
    TEST_ASSERT_FALSE(hy_gap_on_jump(1000, 900, &g));
}

static void test_gap_reason_str(void)
{
    TEST_ASSERT_EQUAL_STRING("off", hy_gap_reason_str(HY_GAP_OFF));
    TEST_ASSERT_EQUAL_STRING("lost", hy_gap_reason_str(HY_GAP_LOST));
    TEST_ASSERT_EQUAL_STRING("clock", hy_gap_reason_str(HY_GAP_CLOCK));
    TEST_ASSERT_EQUAL_STRING("jump", hy_gap_reason_str(HY_GAP_JUMP));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gap_on_start_clean_off);
    RUN_TEST(test_gap_on_start_lost);
    RUN_TEST(test_gap_on_start_none);
    RUN_TEST(test_gap_on_jump);
    RUN_TEST(test_gap_reason_str);
    return UNITY_END();
}
