#include <math.h>
#include "unity.h"
#include "hy/motion.h"

void setUp(void) {}
void tearDown(void) {}

#define STEP 20

typedef struct { hy_shake_t s; uint32_t t; int trues; } rig_t;

/* Gravity (0, -1000, 0) plus an offset in mg on x, y and z. */
static void feed(rig_t *r, int dx, int dy, int dz)
{
    hy_accel_t a = {(int16_t)dx, (int16_t)(-1000 + dy), (int16_t)dz};
    if (hy_shake_feed(&r->s, r->t, a)) r->trues++;
    r->t += STEP;
}

static void feed_axis(rig_t *r, int axis, int mg)
{
    feed(r, axis == 0 ? mg : 0, axis == 1 ? mg : 0, axis == 2 ? mg : 0);
}

static void quiet(rig_t *r, uint32_t ms)
{
    for (uint32_t d = 0; d < ms; d += STEP) feed(r, 0, 0, 0);
}

static void sine(rig_t *r, int axis, int amp_mg, int hz, uint32_t ms)
{
    for (uint32_t d = 0; d < ms; d += STEP)
        feed_axis(r, axis, (int)lround(amp_mg * sin(2.0 * M_PI * hz * (double)d / 1000.0)));
}

/* One single-sample spike of mg on the axis, then quiet so that the next peak is gap_ms after this one. */
static void spike(rig_t *r, int axis, int mg, uint32_t gap_ms)
{
    feed_axis(r, axis, mg);
    quiet(r, gap_ms - STEP);
}

static void start(rig_t *r)
{
    r->t = 1000;
    r->trues = 0;
    hy_shake_reset(&r->s, r->t);
    quiet(r, 400);   /* past the settle time, baseline trained */
}

static void test_shake_basic(void)
{
    rig_t r;
    start(&r);
    sine(&r, 0, 2500, 4, 1000);
    quiet(&r, 600);
    TEST_ASSERT_EQUAL(1, r.trues);
}

static void test_shake_four_peaks(void)
{
    rig_t r;
    start(&r);
    spike(&r, 0, 2500, 120);
    spike(&r, 0, -2500, 120);
    spike(&r, 0, 2500, 120);
    spike(&r, 0, -2500, 120);
    TEST_ASSERT_EQUAL(1, r.trues);
}

static void test_shake_three_peaks(void)
{
    rig_t r;
    start(&r);
    spike(&r, 0, 2500, 120);
    spike(&r, 0, -2500, 120);
    spike(&r, 0, 2500, 120);
    quiet(&r, 1000);
    TEST_ASSERT_EQUAL(0, r.trues);
}

static void test_shake_single_spike(void)
{
    rig_t r;
    start(&r);
    feed_axis(&r, 2, 3000);
    feed_axis(&r, 2, 3000);   /* 40 ms */
    quiet(&r, 2000);
    TEST_ASSERT_EQUAL(0, r.trues);
}

static void test_shake_spike_rebound(void)
{
    rig_t r;
    start(&r);
    for (int i = 0; i < 3; i++) {   /* (a) the rebound on the next sample */
        feed_axis(&r, 0, 2500);
        feed_axis(&r, 0, -2000);
        quiet(&r, 580);
    }
    TEST_ASSERT_EQUAL(0, r.trues);
    for (int i = 0; i < 3; i++) {   /* (b) one quiet sample, then the rebound 40 ms later */
        feed_axis(&r, 0, 2500);
        feed_axis(&r, 0, 0);
        feed_axis(&r, 0, -2000);
        quiet(&r, 560);
    }
    TEST_ASSERT_EQUAL(0, r.trues);
}

static void test_shake_walk_like(void)
{
    rig_t r;
    start(&r);
    sine(&r, 1, 900, 2, 10000);
    TEST_ASSERT_EQUAL(0, r.trues);
}

static void test_shake_slow_peaks(void)
{
    rig_t r;
    start(&r);
    spike(&r, 0, 2500, 500);
    spike(&r, 0, -2500, 500);
    spike(&r, 0, 2500, 500);
    spike(&r, 0, -2500, 500);
    TEST_ASSERT_EQUAL(0, r.trues);
}

/* Four alternating peaks 100 ms apart, the first one lead_ms after reset. */
static int shake_after_reset(uint32_t lead_ms)
{
    rig_t r;
    r.t = 1000;
    r.trues = 0;
    hy_shake_reset(&r.s, r.t);
    quiet(&r, lead_ms);
    spike(&r, 0, 2500, 100);
    spike(&r, 0, -2500, 100);
    spike(&r, 0, 2500, 100);
    spike(&r, 0, -2500, 100);
    quiet(&r, 500);
    return r.trues;
}

static void test_shake_settle(void)
{
    /* peaks at 20, 120 (inside the settle time, ignored), 220, 320: only two count */
    TEST_ASSERT_EQUAL(0, shake_after_reset(20));
    TEST_ASSERT_EQUAL(1, shake_after_reset(300));
}

static void one_shake(rig_t *r)
{
    sine(r, 0, 2500, 4, 500);
}

static void test_shake_cooldown(void)
{
    rig_t r;
    start(&r);
    one_shake(&r);
    quiet(&r, 500);
    one_shake(&r);
    quiet(&r, 1000);
    TEST_ASSERT_EQUAL(1, r.trues);

    start(&r);
    one_shake(&r);
    quiet(&r, 1500);
    one_shake(&r);
    quiet(&r, 1000);
    TEST_ASSERT_EQUAL(2, r.trues);
}

static void test_shake_axis_switch(void)
{
    rig_t r;
    start(&r);
    spike(&r, 0, 2500, 120);
    spike(&r, 2, -2500, 120);
    spike(&r, 0, 2500, 120);
    spike(&r, 2, -2500, 120);
    quiet(&r, 1000);
    TEST_ASSERT_EQUAL(0, r.trues);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_shake_basic);
    RUN_TEST(test_shake_four_peaks);
    RUN_TEST(test_shake_three_peaks);
    RUN_TEST(test_shake_single_spike);
    RUN_TEST(test_shake_spike_rebound);
    RUN_TEST(test_shake_walk_like);
    RUN_TEST(test_shake_slow_peaks);
    RUN_TEST(test_shake_settle);
    RUN_TEST(test_shake_cooldown);
    RUN_TEST(test_shake_axis_switch);
    return UNITY_END();
}
