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

/* R12: a forward jump [1000, 5000) waits HY_GAP_CONFIRM_S after its end, then comes out once. */
static void test_gap_pend_confirm_once(void)
{
    hy_gap_pending_t p = { 0 };
    hy_gap_t f, g;
    TEST_ASSERT_FALSE(hy_gap_pend_jump(&p, 1000, 5000, &f));
    TEST_ASSERT_TRUE(p.active);
    TEST_ASSERT_FALSE(hy_gap_pend_due(&p, 5000 + HY_GAP_CONFIRM_S - 1, &g));
    TEST_ASSERT_TRUE(hy_gap_pend_due(&p, 5000 + HY_GAP_CONFIRM_S, &g));
    TEST_ASSERT_EQUAL_INT64(1000, g.start);
    TEST_ASSERT_EQUAL_INT64(5000, g.end);
    TEST_ASSERT_FALSE(p.active);
    TEST_ASSERT_FALSE(hy_gap_pend_due(&p, 5000 + 2 * HY_GAP_CONFIRM_S, &g));
}

/* Nothing comes out of the pending state on any tick inside the window (the runner publishes only outputs). */
static void test_gap_pend_nothing_before_confirm(void)
{
    hy_gap_pending_t p = { 0 };
    hy_gap_t f, g;
    hy_gap_pend_jump(&p, 1000, 90000, &f);
    for (int64_t t = 90000; t < 90000 + HY_GAP_CONFIRM_S; t += 30) {
        TEST_ASSERT_FALSE(hy_gap_pend_due(&p, t, &g));
    }
    TEST_ASSERT_TRUE(p.active);
    TEST_ASSERT_EQUAL_INT64(1000, p.gap.start);
    TEST_ASSERT_EQUAL_INT64(90000, p.gap.end);
}

/* A backward set inside the window to start + 300 clips the gap to [start, now). */
static void test_gap_pend_back_clips(void)
{
    hy_gap_pending_t p = { 0 };
    hy_gap_t f, g;
    hy_gap_pend_jump(&p, 1000, 90000, &f);
    TEST_ASSERT_EQUAL(HY_GAP_PEND_CLIPPED, hy_gap_pend_back(&p, 1300));
    TEST_ASSERT_TRUE(p.active);
    TEST_ASSERT_EQUAL_INT64(1000, p.gap.start);
    TEST_ASSERT_EQUAL_INT64(1300, p.gap.end);
    TEST_ASSERT_FALSE(hy_gap_pend_due(&p, 1300 + HY_GAP_CONFIRM_S - 1, &g));
    TEST_ASSERT_TRUE(hy_gap_pend_due(&p, 1300 + HY_GAP_CONFIRM_S, &g));
    TEST_ASSERT_EQUAL_INT64(1000, g.start);
    TEST_ASSERT_EQUAL_INT64(1300, g.end);
}

/* A backward set to before start + HY_GAP_MIN_S drops the gap; one at or after its end keeps it. */
static void test_gap_pend_back_drops(void)
{
    hy_gap_pending_t p = { 0 };
    hy_gap_t f, g;
    hy_gap_pend_jump(&p, 1000, 90000, &f);
    TEST_ASSERT_EQUAL(HY_GAP_PEND_DROPPED, hy_gap_pend_back(&p, 1000 + HY_GAP_MIN_S - 1));
    TEST_ASSERT_FALSE(p.active);
    TEST_ASSERT_FALSE(hy_gap_pend_due(&p, 90000 + HY_GAP_CONFIRM_S, &g));
    TEST_ASSERT_EQUAL(HY_GAP_PEND_NONE, hy_gap_pend_back(&p, 500));

    hy_gap_pend_jump(&p, 1000, 90000, &f);
    TEST_ASSERT_EQUAL(HY_GAP_PEND_DROPPED, hy_gap_pend_back(&p, 900));
    TEST_ASSERT_FALSE(p.active);

    hy_gap_pend_jump(&p, 1000, 90000, &f);
    TEST_ASSERT_EQUAL(HY_GAP_PEND_KEPT, hy_gap_pend_back(&p, 90000));
    TEST_ASSERT_EQUAL_INT64(90000, p.gap.end);
    TEST_ASSERT_EQUAL(HY_GAP_PEND_CLIPPED, hy_gap_pend_back(&p, 1000 + HY_GAP_MIN_S));
    TEST_ASSERT_EQUAL_INT64(1000 + HY_GAP_MIN_S, p.gap.end);
}

/* A second forward jump while one is pending hands the first one out, then holds the second. */
static void test_gap_pend_second_jump_flushes(void)
{
    hy_gap_pending_t p = { 0 };
    hy_gap_t f = { 0, 0 }, g;
    TEST_ASSERT_FALSE(hy_gap_pend_jump(&p, 1000, 5000, &f));
    TEST_ASSERT_TRUE(hy_gap_pend_jump(&p, 5100, 9000, &f));
    TEST_ASSERT_EQUAL_INT64(1000, f.start);
    TEST_ASSERT_EQUAL_INT64(5000, f.end);
    TEST_ASSERT_TRUE(p.active);
    TEST_ASSERT_EQUAL_INT64(5100, p.gap.start);
    TEST_ASSERT_EQUAL_INT64(9000, p.gap.end);
    /* a step under HY_GAP_MIN_S is no gap: the pending one stays as it is */
    TEST_ASSERT_FALSE(hy_gap_pend_jump(&p, 9100, 9100 + HY_GAP_MIN_S - 1, &f));
    TEST_ASSERT_EQUAL_INT64(5100, p.gap.start);
    /* a flush (clock invalid) hands it out at once */
    TEST_ASSERT_TRUE(hy_gap_pend_take(&p, &g));
    TEST_ASSERT_EQUAL_INT64(5100, g.start);
    TEST_ASSERT_FALSE(p.active);
    TEST_ASSERT_FALSE(hy_gap_pend_take(&p, &g));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gap_on_start_clean_off);
    RUN_TEST(test_gap_on_start_lost);
    RUN_TEST(test_gap_on_start_none);
    RUN_TEST(test_gap_on_jump);
    RUN_TEST(test_gap_reason_str);
    RUN_TEST(test_gap_pend_confirm_once);
    RUN_TEST(test_gap_pend_nothing_before_confirm);
    RUN_TEST(test_gap_pend_back_clips);
    RUN_TEST(test_gap_pend_back_drops);
    RUN_TEST(test_gap_pend_second_jump_flushes);
    return UNITY_END();
}
