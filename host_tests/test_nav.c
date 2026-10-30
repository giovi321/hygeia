#include "unity.h"
#include "hy/nav.h"

void setUp(void) {}
void tearDown(void) {}

static hy_stroke_t stroke(int x0, int y0, int dx, int dy, uint32_t ms)
{
    hy_stroke_t s = { (int16_t)x0, (int16_t)y0, (int16_t)(x0 + dx), (int16_t)(y0 + dy), 1000, 1000 + ms, 0 };
    return s;
}

static void test_classify_table(void)
{
    hy_stroke_t s;
    s = stroke(200, 200, 16, -16, 499);  TEST_ASSERT_EQUAL(HY_G_TAP, hy_gesture_classify(&s));
    s = stroke(200, 200, 0, 0, 0);       TEST_ASSERT_EQUAL(HY_G_TAP, hy_gesture_classify(&s));
    s = stroke(200, 200, 17, 0, 100);    TEST_ASSERT_EQUAL(HY_G_NONE, hy_gesture_classify(&s));
    s = stroke(200, 200, 0, -17, 100);   TEST_ASSERT_EQUAL(HY_G_NONE, hy_gesture_classify(&s));
    s = stroke(200, 200, 16, 16, 500);   TEST_ASSERT_EQUAL(HY_G_LONG_PRESS, hy_gesture_classify(&s));
    s = stroke(200, 200, 0, 0, 2000);    TEST_ASSERT_EQUAL(HY_G_LONG_PRESS, hy_gesture_classify(&s));
    s = stroke(200, 200, -60, 39, 200);  TEST_ASSERT_EQUAL(HY_G_SWIPE_LEFT, hy_gesture_classify(&s));
    s = stroke(200, 200, -60, 41, 200);  TEST_ASSERT_EQUAL(HY_G_NONE, hy_gesture_classify(&s));
    s = stroke(200, 200, 60, 0, 200);    TEST_ASSERT_EQUAL(HY_G_SWIPE_RIGHT, hy_gesture_classify(&s));
    s = stroke(200, 200, 59, 0, 200);    TEST_ASSERT_EQUAL(HY_G_NONE, hy_gesture_classify(&s));
    s = stroke(200, 200, 0, -60, 200);   TEST_ASSERT_EQUAL(HY_G_SWIPE_UP, hy_gesture_classify(&s));
    s = stroke(200, 200, 39, -60, 200);  TEST_ASSERT_EQUAL(HY_G_SWIPE_UP, hy_gesture_classify(&s));
    s = stroke(200, 200, 41, -60, 200);  TEST_ASSERT_EQUAL(HY_G_NONE, hy_gesture_classify(&s));
    s = stroke(200, 200, 0, 60, 200);    TEST_ASSERT_EQUAL(HY_G_SWIPE_DOWN, hy_gesture_classify(&s));
    s = stroke(200, 39, 0, 60, 200);     TEST_ASSERT_EQUAL(HY_G_EDGE_DOWN, hy_gesture_classify(&s));
    s = stroke(200, 40, 0, 60, 200);     TEST_ASSERT_EQUAL(HY_G_SWIPE_DOWN, hy_gesture_classify(&s));
    s = stroke(200, 440, 0, -60, 200);   TEST_ASSERT_EQUAL(HY_G_EDGE_UP, hy_gesture_classify(&s));
    s = stroke(200, 439, 0, -60, 200);   TEST_ASSERT_EQUAL(HY_G_SWIPE_UP, hy_gesture_classify(&s));
    s = stroke(39, 200, 60, 0, 200);     TEST_ASSERT_EQUAL(HY_G_EDGE_RIGHT, hy_gesture_classify(&s));
    s = stroke(40, 200, 60, 0, 200);     TEST_ASSERT_EQUAL(HY_G_SWIPE_RIGHT, hy_gesture_classify(&s));
    /* a top-edge upward swipe and a left-edge leftward swipe stay plain swipes */
    s = stroke(200, 20, 0, -60, 200);    TEST_ASSERT_EQUAL(HY_G_SWIPE_UP, hy_gesture_classify(&s));
    s = stroke(20, 200, -60, 0, 200);    TEST_ASSERT_EQUAL(HY_G_SWIPE_LEFT, hy_gesture_classify(&s));
}

static void test_drag_is_not_tap(void)
{
    static const int len[3] = { 20, 30, 59 };
    static const int dirx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    static const int diry[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    for (int i = 0; i < 3; i++) {
        for (int d = 0; d < 8; d++) {
            hy_stroke_t s = stroke(200, 200, dirx[d] * len[i], diry[d] * len[i], 100);
            hy_gesture_t g = hy_gesture_classify(&s);
            TEST_ASSERT_NOT_EQUAL(HY_G_TAP, g);
            TEST_ASSERT_NOT_EQUAL(HY_G_LONG_PRESS, g);
        }
    }
}

static void test_double_tap(void)
{
    hy_stroke_t a = stroke(200, 200, 0, 0, 50);
    hy_stroke_t b = stroke(230, 200, 0, 0, 50);
    b.t0_ms = a.t1_ms + 250; b.t1_ms = b.t0_ms + 50;
    TEST_ASSERT_TRUE(hy_gesture_double(&a, &b));
    b = stroke(230, 200, 0, 0, 50);
    b.t0_ms = a.t1_ms + 350; b.t1_ms = b.t0_ms + 50;
    TEST_ASSERT_FALSE(hy_gesture_double(&a, &b));
    b = stroke(250, 200, 0, 0, 50);
    b.t0_ms = a.t1_ms + 250; b.t1_ms = b.t0_ms + 50;
    TEST_ASSERT_FALSE(hy_gesture_double(&a, &b));
    /* second tap starting before the first ended is not a double tap */
    b = stroke(200, 200, 0, 0, 50);
    b.t0_ms = a.t1_ms - 10; b.t1_ms = b.t0_ms + 50;
    TEST_ASSERT_FALSE(hy_gesture_double(&a, &b));
}

static void test_pager(void)
{
    hy_nav_t n; hy_nav_init(&n);
    TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(1, n.depth);
    TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, HY_G_SWIPE_LEFT));
    TEST_ASSERT_EQUAL(HY_V_OVERVIEW, hy_nav_view(&n));
    hy_nav_gesture(&n, HY_G_SWIPE_LEFT);
    TEST_ASSERT_EQUAL(HY_V_HISTORY, hy_nav_view(&n));
    hy_nav_gesture(&n, HY_G_SWIPE_LEFT);
    TEST_ASSERT_EQUAL(HY_V_HISTORY, hy_nav_view(&n));
    hy_nav_gesture(&n, HY_G_SWIPE_RIGHT);
    TEST_ASSERT_EQUAL(HY_V_OVERVIEW, hy_nav_view(&n));
    hy_nav_gesture(&n, HY_G_SWIPE_RIGHT);
    TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
    hy_nav_gesture(&n, HY_G_SWIPE_RIGHT);
    TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(1, n.depth);
}

static void test_edges_from_everywhere(void)
{
    hy_nav_t n; hy_nav_init(&n);
    hy_nav_gesture(&n, HY_G_SWIPE_LEFT); /* OVERVIEW */
    hy_nav_open_detail(&n, 42);
    TEST_ASSERT_EQUAL(HY_V_DETAIL, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(42, n.med_id);
    TEST_ASSERT_EQUAL(2, n.depth);
    hy_nav_gesture(&n, HY_G_EDGE_DOWN);
    TEST_ASSERT_EQUAL(HY_V_QS, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(3, n.depth);
    hy_nav_gesture(&n, HY_G_EDGE_DOWN); /* in QS: nothing */
    TEST_ASSERT_EQUAL(3, n.depth);
    hy_nav_gesture(&n, HY_G_SWIPE_UP);
    TEST_ASSERT_EQUAL(HY_V_DETAIL, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(2, n.depth);
    hy_nav_gesture(&n, HY_G_EDGE_DOWN);
    hy_nav_gesture(&n, HY_G_EDGE_UP);
    TEST_ASSERT_EQUAL(HY_V_DETAIL, hy_nav_view(&n)); /* pops QS only */
    hy_nav_gesture(&n, HY_G_EDGE_UP);
    TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(1, n.depth);

    hy_nav_gesture(&n, HY_G_SWIPE_LEFT);
    hy_nav_open_detail(&n, 7);
    hy_nav_gesture(&n, HY_G_EDGE_RIGHT);
    TEST_ASSERT_EQUAL(HY_V_OVERVIEW, hy_nav_view(&n));
    hy_nav_gesture(&n, HY_G_EDGE_RIGHT); /* pager root at depth 1: pages back */
    TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));

    hy_nav_init(&n);
    hy_nav_push(&n, HY_V_SETTINGS);
    hy_nav_push(&n, HY_V_SET_ZONE);
    hy_nav_gesture(&n, HY_G_EDGE_RIGHT);
    TEST_ASSERT_EQUAL(HY_V_SETTINGS, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(2, n.depth);
    TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, HY_G_TAP));
    TEST_ASSERT_EQUAL(HY_V_SETTINGS, hy_nav_view(&n));
    /* swipes on a pushed view do not move the pager */
    hy_nav_gesture(&n, HY_G_SWIPE_LEFT);
    TEST_ASSERT_EQUAL(HY_V_SETTINGS, hy_nav_view(&n));
}

static void test_push_full_ignored(void)
{
    hy_nav_t n; hy_nav_init(&n);
    for (int i = 0; i < 10; i++) hy_nav_push(&n, HY_V_SETTINGS);
    TEST_ASSERT_EQUAL(HY_NAV_DEPTH, n.depth);
}

static void test_idle_swallows_every_gesture(void)
{
    for (int due = 0; due < 2; due++) {
        for (int g = HY_G_NONE; g <= HY_G_EDGE_RIGHT; g++) {
            hy_nav_t n; hy_nav_init(&n);
            n.idle = true; n.due = due;
            TEST_ASSERT_EQUAL(HY_FX_WAKE, hy_nav_gesture(&n, (hy_gesture_t)g));
            TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
            TEST_ASSERT_EQUAL(1, n.depth);
            TEST_ASSERT_FALSE(n.idle);
            TEST_ASSERT_EQUAL(due, n.due);
        }
    }
}

static void test_due_gestures(void)
{
    hy_nav_t n; hy_nav_init(&n); n.due = true;
    TEST_ASSERT_EQUAL(HY_FX_SNOOZE, hy_nav_gesture(&n, HY_G_SWIPE_LEFT));
    TEST_ASSERT_EQUAL(HY_FX_DUE_NEXT, hy_nav_gesture(&n, HY_G_SWIPE_UP));
    TEST_ASSERT_EQUAL(HY_FX_DUE_NEXT, hy_nav_gesture(&n, HY_G_EDGE_UP));
    static const hy_gesture_t none[] = { HY_G_SWIPE_RIGHT, HY_G_TAP, HY_G_LONG_PRESS,
                                         HY_G_EDGE_RIGHT, HY_G_NONE };
    for (unsigned i = 0; i < sizeof none / sizeof none[0]; i++) {
        TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, none[i]));
        TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
        TEST_ASSERT_EQUAL(1, n.depth);
    }
    TEST_ASSERT_TRUE(n.due);
}

static void test_root_flags(void)
{
    hy_nav_t n; hy_nav_init(&n);
    hy_nav_gesture(&n, HY_G_SWIPE_LEFT); /* OVERVIEW */
    hy_nav_push(&n, HY_V_SETTINGS);
    hy_nav_set_root_flags(&n, true, false);
    TEST_ASSERT_EQUAL(HY_V_CLOCK_INVALID, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(1, n.depth);
    hy_nav_gesture(&n, HY_G_EDGE_DOWN);
    TEST_ASSERT_EQUAL(HY_V_QS, hy_nav_view(&n));
    hy_nav_gesture(&n, HY_G_EDGE_UP);
    TEST_ASSERT_EQUAL(HY_V_CLOCK_INVALID, hy_nav_view(&n));
    hy_nav_gesture(&n, HY_G_EDGE_UP);
    TEST_ASSERT_EQUAL(HY_V_CLOCK_INVALID, hy_nav_view(&n));
    hy_nav_gesture(&n, HY_G_SWIPE_LEFT);
    TEST_ASSERT_EQUAL(HY_V_CLOCK_INVALID, hy_nav_view(&n));
    hy_nav_set_root_flags(&n, true, true);
    TEST_ASSERT_EQUAL(HY_V_STORAGE_DOWN, hy_nav_view(&n));
    hy_nav_set_root_flags(&n, false, true);
    TEST_ASSERT_EQUAL(HY_V_STORAGE_DOWN, hy_nav_view(&n));
    hy_nav_set_root_flags(&n, false, false);
    TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(1, n.depth);
    /* same root kind keeps the page and stack */
    hy_nav_gesture(&n, HY_G_SWIPE_LEFT);
    hy_nav_push(&n, HY_V_SETTINGS);
    hy_nav_set_root_flags(&n, false, false);
    TEST_ASSERT_EQUAL(HY_V_SETTINGS, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(2, n.depth);
}

static void test_idle_drift_bounds(void)
{
    int8_t px = 0, py = 0, fx, fy;
    for (int64_t m = 0; m < 1000; m++) {
        int8_t dx, dy, ex, ey;
        hy_idle_drift(m, &dx, &dy);
        TEST_ASSERT_TRUE(dx >= -8 && dx <= 8);
        TEST_ASSERT_TRUE(dy >= -8 && dy <= 8);
        hy_idle_drift(m + 16, &ex, &ey);
        TEST_ASSERT_EQUAL(dx, ex);
        TEST_ASSERT_EQUAL(dy, ey);
        if (m > 0) TEST_ASSERT_TRUE(dx != px || dy != py);
        px = dx; py = dy;
        hy_idle_drift(-m - 1, &fx, &fy);
        TEST_ASSERT_TRUE(fx >= -8 && fx <= 8 && fy >= -8 && fy <= 8);
    }
    /* negative minutes continue the same period and stay distinct from neighbours */
    int8_t ax, ay, bx, by;
    hy_idle_drift(-1, &ax, &ay);
    hy_idle_drift(15, &bx, &by);
    TEST_ASSERT_EQUAL(ax, bx);
    TEST_ASSERT_EQUAL(ay, by);
    hy_idle_drift(-1, &ax, &ay);
    hy_idle_drift(0, &bx, &by);
    TEST_ASSERT_TRUE(ax != bx || ay != by);
}

static void test_round_trip_drag_is_not_tap(void)
{
    hy_stroke_t s = stroke(200, 200, 0, 0, 100);
    s.max_dev = 40;
    TEST_ASSERT_NOT_EQUAL(HY_G_TAP, hy_gesture_classify(&s));
    TEST_ASSERT_NOT_EQUAL(HY_G_LONG_PRESS, hy_gesture_classify(&s));
    s = stroke(200, 200, 0, 0, 800);
    s.max_dev = 40;
    TEST_ASSERT_NOT_EQUAL(HY_G_LONG_PRESS, hy_gesture_classify(&s));
    TEST_ASSERT_NOT_EQUAL(HY_G_TAP, hy_gesture_classify(&s));
    s = stroke(200, 200, 0, 0, 100);
    s.max_dev = 17;
    TEST_ASSERT_EQUAL(HY_G_NONE, hy_gesture_classify(&s));
    s.max_dev = 16;
    TEST_ASSERT_EQUAL(HY_G_TAP, hy_gesture_classify(&s));
    s = stroke(200, 200, 0, 0, 600);
    s.max_dev = 16;
    TEST_ASSERT_EQUAL(HY_G_LONG_PRESS, hy_gesture_classify(&s));
    /* swipes still classify from the endpoints */
    s = stroke(200, 200, -60, 0, 200);
    s.max_dev = 90;
    TEST_ASSERT_EQUAL(HY_G_SWIPE_LEFT, hy_gesture_classify(&s));
}

static void test_edge_right_on_pager_pages_back(void)
{
    hy_nav_t n; hy_nav_init(&n);
    hy_nav_gesture(&n, HY_G_SWIPE_LEFT);
    hy_nav_gesture(&n, HY_G_SWIPE_LEFT);
    TEST_ASSERT_EQUAL(HY_V_HISTORY, hy_nav_view(&n));
    hy_nav_gesture(&n, HY_G_EDGE_RIGHT);
    TEST_ASSERT_EQUAL(HY_V_OVERVIEW, hy_nav_view(&n));
    hy_nav_gesture(&n, HY_G_EDGE_RIGHT);
    TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
    hy_nav_gesture(&n, HY_G_EDGE_RIGHT);
    TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(1, n.depth);
}

static void test_qs_opens_on_full_stack(void)
{
    hy_nav_t n; hy_nav_init(&n);
    for (int i = 0; i < 10; i++) hy_nav_push(&n, HY_V_SETTINGS);
    TEST_ASSERT_EQUAL(HY_NAV_DEPTH, n.depth);
    hy_nav_gesture(&n, HY_G_EDGE_DOWN);
    TEST_ASSERT_EQUAL(HY_V_QS, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(HY_NAV_DEPTH, n.depth);
    hy_nav_gesture(&n, HY_G_SWIPE_UP);
    TEST_ASSERT_EQUAL(HY_NAV_DEPTH - 1, n.depth);
    TEST_ASSERT_EQUAL(HY_V_SETTINGS, hy_nav_view(&n));
}

static void test_idle_swallows_on_pushed_stack(void)
{
    for (int due = 0; due < 2; due++) {
        for (int g = HY_G_NONE; g <= HY_G_EDGE_RIGHT; g++) {
            hy_nav_t n; hy_nav_init(&n);
            hy_nav_gesture(&n, HY_G_SWIPE_LEFT);
            hy_nav_open_detail(&n, 5);
            hy_nav_push(&n, HY_V_QS);
            n.idle = true; n.due = due;
            TEST_ASSERT_EQUAL(HY_FX_WAKE, hy_nav_gesture(&n, (hy_gesture_t)g));
            TEST_ASSERT_EQUAL(HY_V_QS, hy_nav_view(&n));
            TEST_ASSERT_EQUAL(3, n.depth);
            TEST_ASSERT_EQUAL(HY_V_OVERVIEW, n.stack[0]);
            TEST_ASSERT_FALSE(n.idle);
            TEST_ASSERT_EQUAL(due, n.due);
        }
    }
}

static void test_classify_exact_boundaries(void)
{
    hy_stroke_t s;
    s = stroke(200, 200, 60, 40, 200);   TEST_ASSERT_EQUAL(HY_G_SWIPE_RIGHT, hy_gesture_classify(&s));
    s = stroke(200, 200, 60, 60, 200);   TEST_ASSERT_EQUAL(HY_G_NONE, hy_gesture_classify(&s));
    s = stroke(200, 200, 0, 0, 0);
    s.t0_ms = 2000; s.t1_ms = 1000;      /* t1 < t0: treated as a long duration, never a crash */
    TEST_ASSERT_NOT_EQUAL(HY_G_SWIPE_LEFT, hy_gesture_classify(&s));
}

static void test_double_tap_exact_and_wrap(void)
{
    hy_stroke_t a = stroke(200, 200, 0, 0, 50);
    hy_stroke_t b = stroke(240, 200, 0, 0, 50);
    b.t0_ms = a.t1_ms + 300; b.t1_ms = b.t0_ms + 50;
    TEST_ASSERT_TRUE(hy_gesture_double(&a, &b));
    b.t0_ms = a.t1_ms + 301;
    TEST_ASSERT_FALSE(hy_gesture_double(&a, &b));
    /* millisecond counter wrapping between the taps */
    a = stroke(200, 200, 0, 0, 50);
    a.t0_ms = 0xFFFFFF00u; a.t1_ms = 0xFFFFFF40u;
    b = stroke(210, 200, 0, 0, 50);
    b.t0_ms = a.t1_ms + 200; b.t1_ms = b.t0_ms + 50;
    TEST_ASSERT_TRUE(b.t0_ms < a.t1_ms);
    TEST_ASSERT_TRUE(hy_gesture_double(&a, &b));
    b.t0_ms = a.t1_ms - 5;
    TEST_ASSERT_FALSE(hy_gesture_double(&a, &b));
}

static void test_poweroff_rule(void)
{
    for (int mode = 0; mode < 2; mode++) {          /* plain, due (idle is rule 0a: test_off_counts_as_idle) */
        for (int g = HY_G_NONE; g <= HY_G_EDGE_RIGHT; g++) {
            hy_nav_t n; hy_nav_init(&n);
            hy_nav_push(&n, HY_V_SETTINGS);
            n.poweroff = true; n.due = (mode == 1);
            TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, (hy_gesture_t)g));
            TEST_ASSERT_EQUAL(2, n.depth);
            TEST_ASSERT_EQUAL(HY_V_HOME, n.stack[0]);
            TEST_ASSERT_EQUAL(HY_V_SETTINGS, hy_nav_view(&n));
            TEST_ASSERT_EQUAL(mode == 1, n.due);
            TEST_ASSERT_FALSE(n.idle);
            TEST_ASSERT_TRUE(n.poweroff);
        }
    }
    hy_nav_t n; hy_nav_init(&n);
    TEST_ASSERT_FALSE(n.poweroff);
}

static void test_due_swipe_down_prev(void)
{
    hy_nav_t n; hy_nav_init(&n); n.due = true;
    TEST_ASSERT_EQUAL(HY_FX_DUE_PREV, hy_nav_gesture(&n, HY_G_SWIPE_DOWN));
    TEST_ASSERT_EQUAL(HY_FX_DUE_PREV, hy_nav_gesture(&n, HY_G_EDGE_DOWN));
    TEST_ASSERT_EQUAL(1, n.depth);
    TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
    TEST_ASSERT_TRUE(n.due);
}

static hy_nav_t form_nav(void)
{
    hy_nav_t n; hy_nav_init(&n);
    hy_nav_push(&n, HY_V_MEDFORM);
    return n;
}

static void test_form_edge_right_back(void)
{
    hy_nav_t n = form_nav();
    TEST_ASSERT_EQUAL(HY_FX_FORM_BACK, hy_nav_gesture(&n, HY_G_EDGE_RIGHT));
    TEST_ASSERT_EQUAL(2, n.depth);
    TEST_ASSERT_EQUAL(HY_V_MEDFORM, hy_nav_view(&n));
}

static void test_form_edge_right_dirty_back(void)
{
    hy_nav_t n = form_nav();
    n.form_dirty = true;
    TEST_ASSERT_EQUAL(HY_FX_FORM_BACK, hy_nav_gesture(&n, HY_G_EDGE_RIGHT));
    TEST_ASSERT_EQUAL(2, n.depth);
    TEST_ASSERT_EQUAL(HY_V_MEDFORM, hy_nav_view(&n));
}

static void test_form_edge_up_dirty_ignored(void)
{
    hy_nav_t n = form_nav();
    n.form_dirty = true;
    TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, HY_G_EDGE_UP));
    TEST_ASSERT_EQUAL(2, n.depth);
    TEST_ASSERT_EQUAL(HY_V_MEDFORM, hy_nav_view(&n));
}

static void test_form_edge_up_clean_home(void)
{
    hy_nav_t n = form_nav();
    TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, HY_G_EDGE_UP));
    TEST_ASSERT_EQUAL(1, n.depth);
    TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
}

static void test_form_qs_opens(void)
{
    hy_nav_t n = form_nav();
    n.form_dirty = true;
    TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, HY_G_EDGE_DOWN));
    TEST_ASSERT_EQUAL(3, n.depth);
    TEST_ASSERT_EQUAL(HY_V_QS, hy_nav_view(&n));
    TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, HY_G_SWIPE_UP));
    TEST_ASSERT_EQUAL(2, n.depth);
    TEST_ASSERT_EQUAL(HY_V_MEDFORM, hy_nav_view(&n));
}

static void test_form_under_due_and_idle(void)
{
    hy_nav_t n = form_nav();
    n.due = true;
    TEST_ASSERT_EQUAL(HY_FX_SNOOZE, hy_nav_gesture(&n, HY_G_SWIPE_LEFT));
    TEST_ASSERT_EQUAL(HY_FX_DUE_PREV, hy_nav_gesture(&n, HY_G_SWIPE_DOWN));
    TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, HY_G_EDGE_RIGHT));
    TEST_ASSERT_EQUAL(2, n.depth);
    TEST_ASSERT_EQUAL(HY_V_MEDFORM, hy_nav_view(&n));
    n.due = false; n.idle = true;
    TEST_ASSERT_EQUAL(HY_FX_WAKE, hy_nav_gesture(&n, HY_G_EDGE_RIGHT));
    TEST_ASSERT_FALSE(n.idle);
    TEST_ASSERT_EQUAL(2, n.depth);
    n.poweroff = true;
    TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, HY_G_EDGE_RIGHT));
    TEST_ASSERT_EQUAL(2, n.depth);
}

static void test_archive_view_edge_right_pops(void)
{
    hy_nav_t n; hy_nav_init(&n);
    hy_nav_open_detail(&n, 7);
    hy_nav_push(&n, HY_V_MED_ARCHIVE);
    TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, HY_G_EDGE_RIGHT));
    TEST_ASSERT_EQUAL(2, n.depth);
    TEST_ASSERT_EQUAL(HY_V_DETAIL, hy_nav_view(&n));
}

static void test_form_from_detail(void)
{
    hy_nav_t n; hy_nav_init(&n);
    hy_nav_open_detail(&n, 7);
    hy_nav_push(&n, HY_V_MEDFORM);
    TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, HY_G_EDGE_UP));
    TEST_ASSERT_EQUAL(1, n.depth);
    TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
}

static void test_form_dirty_edge_up_from_settings(void)
{
    hy_nav_t n; hy_nav_init(&n);
    hy_nav_push(&n, HY_V_MEDFORM);
    n.form_dirty = true;
    hy_nav_push(&n, HY_V_QS);
    hy_nav_push(&n, HY_V_SETTINGS);
    TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, HY_G_EDGE_UP));
    TEST_ASSERT_EQUAL(2, n.depth);
    TEST_ASSERT_EQUAL(HY_V_MEDFORM, hy_nav_view(&n));
}

static void test_form_clean_edge_up_from_settings(void)
{
    hy_nav_t n; hy_nav_init(&n);
    hy_nav_push(&n, HY_V_MEDFORM);
    hy_nav_push(&n, HY_V_QS);
    hy_nav_push(&n, HY_V_SETTINGS);
    TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, HY_G_EDGE_UP));
    TEST_ASSERT_EQUAL(1, n.depth);
    TEST_ASSERT_EQUAL(HY_V_HOME, hy_nav_view(&n));
}

static void test_off_counts_as_idle(void)
{
    hy_gesture_t gs[] = { HY_G_SWIPE_LEFT, HY_G_TAP };
    for (int i = 0; i < 2; i++) {
        hy_nav_t n; hy_nav_init(&n);
        n.idle = true; n.due = true;
        TEST_ASSERT_EQUAL(HY_FX_WAKE, hy_nav_gesture(&n, gs[i]));
        TEST_ASSERT_FALSE(n.idle);
        TEST_ASSERT_TRUE(n.due);
    }
    /* an overlay up on a DIM or OFF screen: the stroke only wakes */
    for (int i = 0; i < 2; i++) {
        hy_nav_t n; hy_nav_init(&n);
        n.idle = true; n.poweroff = true;
        TEST_ASSERT_EQUAL(HY_FX_WAKE, hy_nav_gesture(&n, gs[i]));
        TEST_ASSERT_FALSE(n.idle);
        TEST_ASSERT_TRUE(n.poweroff);
        TEST_ASSERT_EQUAL(HY_FX_NONE, hy_nav_gesture(&n, gs[i]));   /* awake: rule 0 swallows it */
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_classify_table);
    RUN_TEST(test_drag_is_not_tap);
    RUN_TEST(test_double_tap);
    RUN_TEST(test_pager);
    RUN_TEST(test_edges_from_everywhere);
    RUN_TEST(test_push_full_ignored);
    RUN_TEST(test_idle_swallows_every_gesture);
    RUN_TEST(test_due_gestures);
    RUN_TEST(test_root_flags);
    RUN_TEST(test_idle_drift_bounds);
    RUN_TEST(test_round_trip_drag_is_not_tap);
    RUN_TEST(test_edge_right_on_pager_pages_back);
    RUN_TEST(test_qs_opens_on_full_stack);
    RUN_TEST(test_idle_swallows_on_pushed_stack);
    RUN_TEST(test_classify_exact_boundaries);
    RUN_TEST(test_double_tap_exact_and_wrap);
    RUN_TEST(test_poweroff_rule);
    RUN_TEST(test_off_counts_as_idle);
    RUN_TEST(test_due_swipe_down_prev);
    RUN_TEST(test_form_edge_right_back);
    RUN_TEST(test_form_edge_right_dirty_back);
    RUN_TEST(test_form_edge_up_dirty_ignored);
    RUN_TEST(test_form_edge_up_clean_home);
    RUN_TEST(test_form_qs_opens);
    RUN_TEST(test_form_under_due_and_idle);
    RUN_TEST(test_archive_view_edge_right_pops);
    RUN_TEST(test_form_from_detail);
    RUN_TEST(test_form_dirty_edge_up_from_settings);
    RUN_TEST(test_form_clean_edge_up_from_settings);
    return UNITY_END();
}
