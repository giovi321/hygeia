#include "unity.h"
#include "hy/actions.h"
#include "hy/toast_stroke.h"

void setUp(void) {}
void tearDown(void) {}

/* args: toast_showing, gesture, on_undo, waking, overlay */

static void test_no_toast_passes_every_stroke(void)
{
    for (int g = HY_G_NONE; g <= HY_G_EDGE_RIGHT; g++) {
        TEST_ASSERT_EQUAL(HY_TS_PASS, hy_toast_stroke(false, (hy_gesture_t)g, false, false, false));
    }
}

static void test_tap_on_undo_leaves_the_toast_to_undo(void)
{
    TEST_ASSERT_EQUAL(HY_TS_CONSUME, hy_toast_stroke(true, HY_G_TAP, true, false, false));
}

static void test_tap_outside_closes_and_is_consumed(void)
{
    TEST_ASSERT_EQUAL(HY_TS_CLOSE, hy_toast_stroke(true, HY_G_TAP, false, false, false));
}

static void test_next_tap_after_close_is_normal(void)
{
    /* after the close the toast is not showing: the next stroke is not the toast's */
    TEST_ASSERT_EQUAL(HY_TS_PASS, hy_toast_stroke(false, HY_G_TAP, false, false, false));
}

static void test_other_strokes_are_consumed_and_keep_the_toast(void)
{
    hy_gesture_t gs[] = {HY_G_NONE, HY_G_LONG_PRESS, HY_G_SWIPE_LEFT, HY_G_SWIPE_RIGHT, HY_G_SWIPE_UP,
                         HY_G_SWIPE_DOWN, HY_G_EDGE_DOWN, HY_G_EDGE_UP, HY_G_EDGE_RIGHT};
    for (unsigned i = 0; i < sizeof gs / sizeof gs[0]; i++) {
        TEST_ASSERT_EQUAL(HY_TS_CONSUME, hy_toast_stroke(true, gs[i], false, false, false));
        TEST_ASSERT_EQUAL(HY_TS_CONSUME, hy_toast_stroke(true, gs[i], true, false, false));
    }
}

static void test_overlay_or_waking_stroke_does_not_close(void)
{
    /* the power-off or pairing overlay, or a stroke that only woke the screen: consumed as before, toast stays */
    TEST_ASSERT_EQUAL(HY_TS_CONSUME, hy_toast_stroke(true, HY_G_TAP, false, false, true));
    TEST_ASSERT_EQUAL(HY_TS_CONSUME, hy_toast_stroke(true, HY_G_TAP, false, true, false));
}

static void test_too_late_info_toast_is_not_the_logged_toast(void)
{
    /* "Too late to undo" is an info toast: the logged toast is not showing, so its strokes pass */
    TEST_ASSERT_EQUAL(HY_TS_PASS, hy_toast_stroke(false, HY_G_TAP, false, false, false));
    TEST_ASSERT_EQUAL(5, HY_UNDO_WINDOW_S);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_toast_passes_every_stroke);
    RUN_TEST(test_tap_on_undo_leaves_the_toast_to_undo);
    RUN_TEST(test_tap_outside_closes_and_is_consumed);
    RUN_TEST(test_next_tap_after_close_is_normal);
    RUN_TEST(test_other_strokes_are_consumed_and_keep_the_toast);
    RUN_TEST(test_overlay_or_waking_stroke_does_not_close);
    RUN_TEST(test_too_late_info_toast_is_not_the_logged_toast);
    return UNITY_END();
}
