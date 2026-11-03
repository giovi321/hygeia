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

static void test_sample_double_tap(void)
{
    hy_wake_gate_t g;
    hy_wake_gate_begin(&g, 1000);
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_sample(&g, 1005, true, true, 100, 100));
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_sample(&g, 1020, true, true, 101, 100));
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_sample(&g, 1065, true, false, 0, 0));   /* first tap up */
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_sample(&g, 1100, true, false, 0, 0));
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_sample(&g, 1200, true, true, 105, 100));
    TEST_ASSERT_EQUAL(HY_WG_WAKE, hy_wake_gate_sample(&g, 1260, true, false, 0, 0));
}

static void test_sample_noise_and_failures(void)
{
    hy_wake_gate_t g;
    /* an interrupt with no finger: noise */
    hy_wake_gate_begin(&g, 1000);
    TEST_ASSERT_EQUAL(HY_WG_SLEEP, hy_wake_gate_sample(&g, 1005, true, false, 0, 0));
    /* a controller that never answers: the second failed poll before any touch ends it, the first is retried */
    hy_wake_gate_begin(&g, 1000);
    TEST_ASSERT_EQUAL(HY_WG_NONE, hy_wake_gate_sample(&g, 1005, false, false, 0, 0));
    TEST_ASSERT_EQUAL(HY_WG_SLEEP, hy_wake_gate_sample(&g, 1020, false, false, 0, 0));
    /* the first read after standby fails, the retry sees the finger */
    hy_wake_gate_begin(&g, 1000);
    TEST_ASSERT_EQUAL(HY_WG_NONE, hy_wake_gate_sample(&g, 1005, false, false, 0, 0));
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_sample(&g, 1020, true, true, 60, 60));

    /* it answers once, then fails while the finger is down: the long press rule still ends it */
    hy_wake_gate_begin(&g, 1000);
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_sample(&g, 1005, true, true, 50, 50));
    TEST_ASSERT_EQUAL(HY_WG_WAIT, hy_wake_gate_sample(&g, 1100, false, false, 0, 0));
    TEST_ASSERT_EQUAL(HY_WG_SLEEP, hy_wake_gate_sample(&g, 1005 + HY_LONG_PRESS_MS, false, false, 0, 0));
}

static void test_sample_cap(void)
{
    hy_wake_gate_t g;
    /* Taps that alternate between two places 150 px apart, every 150 ms: each one is a new first tap, so neither the
     * gap tick nor the long-press tick ever ends the judgment. Only the cap does. */
    hy_wake_gate_begin(&g, 1000);
    for (int i = 0; i < 20; i++) {
        uint32_t t = 1000 + 150u * (uint32_t)i;
        int16_t x = (i % 2) ? 250 : 100;
        hy_wg_act_t want = (t - 1000 >= HY_WG_MAX_MS) ? HY_WG_SLEEP : HY_WG_WAIT;
        hy_wg_act_t a1 = hy_wake_gate_sample(&g, t, true, true, x, 100);
        TEST_ASSERT_EQUAL_MESSAGE(want, a1, "down");
        if (want == HY_WG_SLEEP) {
            TEST_ASSERT_EQUAL(HY_WG_NONE, hy_wake_gate_tick(&g, t + 1));
            return;
        }
        TEST_ASSERT_EQUAL_MESSAGE(HY_WG_WAIT, hy_wake_gate_sample(&g, t + 40, true, false, 0, 0), "up");
    }
    TEST_FAIL_MESSAGE("the cap never ended the judgment");
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
    RUN_TEST(test_sample_double_tap);
    RUN_TEST(test_sample_noise_and_failures);
    RUN_TEST(test_sample_cap);
    return UNITY_END();
}
