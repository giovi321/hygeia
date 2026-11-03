#include "unity.h"
#include "hy/nav.h"
#include "hy/wake_gate.h"

void setUp(void) {}
void tearDown(void) {}

static void test_single_tap_off_waits_then_sleeps(void)
{
    hy_wake_gate_t g;
    hy_wake_gate_reset(&g);
    TEST_ASSERT_EQUAL(HY_WG_NONE, hy_wake_gate_tick(&g, 0));
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_down(&g, false, 1000, 200, 200));
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_tick(&g, 1030));
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_up(&g, 1060, 200, 200));   /* first tap: wait for a second */
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_tick(&g, 1060 + HY_DOUBLE_TAP_MS));
    TEST_ASSERT_EQUAL(HY_WG_SLEEP, hy_wake_gate_tick(&g, 1060 + HY_DOUBLE_TAP_MS + 1));
    TEST_ASSERT_EQUAL(HY_WG_NONE, hy_wake_gate_tick(&g, 5000));   /* over: nothing more to judge */
}

static void test_double_tap_wakes(void)
{
    hy_wake_gate_t g;
    hy_wake_gate_reset(&g);
    hy_wake_gate_down(&g, false, 1000, 200, 200);
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_up(&g, 1060, 200, 200));
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_down(&g, false, 1260, 210, 205));
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_tick(&g, 1280));
    TEST_ASSERT_EQUAL(HY_WG_WAKE, hy_wake_gate_up(&g, 1320, 210, 205));
    TEST_ASSERT_EQUAL(HY_WG_NONE, hy_wake_gate_tick(&g, 9000));   /* reset after the wake */
}

static void test_double_tap_limits(void)
{
    hy_wake_gate_t g;
    /* exactly 300 ms between release and second press, exactly 40 px: wakes */
    hy_wake_gate_reset(&g);
    hy_wake_gate_down(&g, false, 1000, 100, 100);
    hy_wake_gate_up(&g, 1060, 100, 100);
    hy_wake_gate_down(&g, false, 1060 + HY_DOUBLE_TAP_MS, 140, 100);
    TEST_ASSERT_EQUAL(HY_WG_WAKE, hy_wake_gate_up(&g, 1500, 140, 100));
    /* 301 ms: the second touch is a new first touch, no wake */
    hy_wake_gate_reset(&g);
    hy_wake_gate_down(&g, false, 1000, 100, 100);
    hy_wake_gate_up(&g, 1060, 100, 100);
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_down(&g, false, 1060 + HY_DOUBLE_TAP_MS + 1, 100, 100));
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_up(&g, 1700, 100, 100));
    TEST_ASSERT_EQUAL(HY_WG_SLEEP, hy_wake_gate_tick(&g, 1700 + HY_DOUBLE_TAP_MS + 1));
    /* 41 px away: no wake */
    hy_wake_gate_reset(&g);
    hy_wake_gate_down(&g, false, 1000, 100, 100);
    hy_wake_gate_up(&g, 1060, 100, 100);
    hy_wake_gate_down(&g, false, 1200, 141, 100);
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_up(&g, 1260, 141, 100));   /* only a new first tap */
    TEST_ASSERT_EQUAL(HY_WG_SLEEP, hy_wake_gate_tick(&g, 1260 + HY_DOUBLE_TAP_MS + 1));
}

static void test_second_touch_not_a_tap(void)
{
    hy_wake_gate_t g;
    hy_wake_gate_reset(&g);
    hy_wake_gate_down(&g, false, 1000, 200, 200);
    hy_wake_gate_up(&g, 1060, 200, 200);
    hy_wake_gate_down(&g, false, 1200, 200, 200);
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_tick(&g, 1200 + HY_LONG_PRESS_MS - 1));
    TEST_ASSERT_EQUAL(HY_WG_SLEEP, hy_wake_gate_tick(&g, 1200 + HY_LONG_PRESS_MS));   /* held: a long press */
    /* second touch is a swipe */
    hy_wake_gate_reset(&g);
    hy_wake_gate_down(&g, false, 1000, 200, 200);
    hy_wake_gate_up(&g, 1060, 200, 200);
    hy_wake_gate_down(&g, false, 1200, 200, 200);
    hy_wake_gate_move(&g, 200, 250);
    TEST_ASSERT_EQUAL(HY_WG_SLEEP, hy_wake_gate_up(&g, 1260, 200, 290));
}

static void test_single_tap_on_wakes(void)
{
    hy_wake_gate_t g;
    hy_wake_gate_reset(&g);
    TEST_ASSERT_EQUAL(HY_WG_WAKE, hy_wake_gate_down(&g, true, 1000, 200, 200));
    TEST_ASSERT_EQUAL(HY_WG_NONE, hy_wake_gate_up(&g, 1060, 200, 200));
    /* the setting flips while a first tap waits: the next touch wakes at once */
    hy_wake_gate_reset(&g);
    hy_wake_gate_down(&g, false, 1000, 200, 200);
    hy_wake_gate_up(&g, 1060, 200, 200);
    TEST_ASSERT_EQUAL(HY_WG_WAKE, hy_wake_gate_down(&g, true, 5000, 20, 20));
}

static void test_first_touch_long_press_or_swipe(void)
{
    hy_wake_gate_t g;
    /* a long press as the first touch: decided while the finger is still down, no wait for the release */
    hy_wake_gate_reset(&g);
    hy_wake_gate_down(&g, false, 1000, 200, 200);
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_tick(&g, 1000 + HY_LONG_PRESS_MS - 1));
    TEST_ASSERT_EQUAL(HY_WG_SLEEP, hy_wake_gate_tick(&g, 1000 + HY_LONG_PRESS_MS));
    TEST_ASSERT_EQUAL(HY_WG_NONE, hy_wake_gate_up(&g, 1700, 200, 200));   /* the late release is ignored */
    /* a released long press that was never ticked */
    hy_wake_gate_reset(&g);
    hy_wake_gate_down(&g, false, 1000, 200, 200);
    TEST_ASSERT_EQUAL(HY_WG_SLEEP, hy_wake_gate_up(&g, 1600, 200, 200));
    /* a swipe as the first touch: the sequence ends at once */
    hy_wake_gate_reset(&g);
    hy_wake_gate_down(&g, false, 1000, 200, 100);
    hy_wake_gate_move(&g, 200, 160);
    TEST_ASSERT_EQUAL(HY_WG_SLEEP, hy_wake_gate_up(&g, 1100, 200, 220));
    /* a drag that returns to its start is not a tap either */
    hy_wake_gate_reset(&g);
    hy_wake_gate_down(&g, false, 1000, 200, 200);
    hy_wake_gate_move(&g, 260, 200);
    hy_wake_gate_move(&g, 202, 200);
    TEST_ASSERT_EQUAL(HY_WG_SLEEP, hy_wake_gate_up(&g, 1100, 202, 200));
}

static void test_clock_wraps(void)
{
    hy_wake_gate_t g;
    hy_wake_gate_reset(&g);
    uint32_t t = 0xFFFFFFF0u;
    hy_wake_gate_down(&g, false, t, 50, 50);
    hy_wake_gate_up(&g, t + 40, 50, 50);   /* wraps to 0x18 */
    hy_wake_gate_down(&g, false, t + 200, 52, 50);
    TEST_ASSERT_EQUAL(HY_WG_WAKE, hy_wake_gate_up(&g, t + 250, 52, 50));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_single_tap_off_waits_then_sleeps);
    RUN_TEST(test_double_tap_wakes);
    RUN_TEST(test_double_tap_limits);
    RUN_TEST(test_second_touch_not_a_tap);
    RUN_TEST(test_single_tap_on_wakes);
    RUN_TEST(test_first_touch_long_press_or_swipe);
    RUN_TEST(test_clock_wraps);
    return UNITY_END();
}
