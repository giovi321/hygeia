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

/* Alternating spikes: every spike is a turning point, so n spikes make n - 1 strokes. */
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

/* Same-sign spikes (the return stroke is small or missing in the signal) still make strokes: the rise to a spike and
   the fall from it are one each. */
static void test_shake_same_sign_train(void)
{
    rig_t r;
    start(&r);
    spike(&r, 0, 3500, 120);
    spike(&r, 0, 3500, 120);
    spike(&r, 0, 3500, 120);
    quiet(&r, 1000);
    TEST_ASSERT_EQUAL(1, r.trues);

    start(&r);
    spike(&r, 1, -3500, 120);
    spike(&r, 1, -3500, 120);
    spike(&r, 1, -3500, 120);
    quiet(&r, 1000);
    TEST_ASSERT_EQUAL(1, r.trues);

    start(&r);   /* two spikes are three turning points: two strokes */
    spike(&r, 0, 3500, 120);
    spike(&r, 0, 3500, 120);
    quiet(&r, 1000);
    TEST_ASSERT_EQUAL(0, r.trues);
}

/* The swing, not the sign, decides: a train of spikes under HY_SHK_SWING_MG does not count. */
static void test_shake_small_swing(void)
{
    rig_t r;
    start(&r);
    for (int i = 0; i < 8; i++) spike(&r, 0, 2500, 120);
    for (int i = 0; i < 4; i++) {
        spike(&r, 2, 1400, 120);
        spike(&r, 2, -1400, 120);
    }
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

/* One knock: a spike with its rebound on the next sample, and a ring-down that halves every half cycle. */
static void test_shake_knock(void)
{
    rig_t r;
    start(&r);
    feed_axis(&r, 0, 2500);
    feed_axis(&r, 0, -2000);
    quiet(&r, 1000);
    TEST_ASSERT_EQUAL(0, r.trues);

    start(&r);
    int amp = 6000;
    for (int i = 0; i < 8; i++) {
        feed_axis(&r, 0, amp);
        quiet(&r, 80);
        amp = -amp / 2;
    }
    quiet(&r, 1000);
    TEST_ASSERT_EQUAL(0, r.trues);
}

static void test_shake_walk_like(void)
{
    rig_t r;
    start(&r);
    sine(&r, 1, 900, 2, 10000);
    TEST_ASSERT_EQUAL(0, r.trues);
    start(&r);
    sine(&r, 1, 1200, 2, 10000);
    TEST_ASSERT_EQUAL(0, r.trues);
}

/* Large swings that are slower than HY_SHK_STROKE_MS per stroke: a slow tilt back and forth, a slow wave. */
static void test_shake_slow_tilt(void)
{
    rig_t r;
    start(&r);
    sine(&r, 0, 2500, 1, 10000);
    TEST_ASSERT_EQUAL(0, r.trues);
    start(&r);
    sine(&r, 1, 2000, 2, 10000);   /* swing 4000, 250 ms per stroke */
    TEST_ASSERT_EQUAL(0, r.trues);
}

/* A peak train whose strokes are too slow. */
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

/* Four alternating spikes 100 ms apart, the first one lead_ms after reset. */
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
    /* spikes at 20, 120, 220, 320: the strokes end at about 140, 240, 340; the first is inside the settle time and is
       not counted, so only two strokes count */
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

static void test_shake_cooldown_clock_wrap(void)
{
    rig_t r;
    r.t = 0;
    r.trues = 0;
    hy_shake_reset(&r.s, r.t);
    r.t = 0x80002000u;   /* more than 2^31 ms after reset, no detection yet */
    quiet(&r, 400);
    one_shake(&r);
    quiet(&r, 1500);
    one_shake(&r);
    quiet(&r, 1000);
    TEST_ASSERT_EQUAL(2, r.trues);
}

/* One long shake is one detection: after the cooldown, strokes that keep coming do not arm the detector. */
static void test_shake_long_shake(void)
{
    rig_t r;
    start(&r);
    sine(&r, 0, 2500, 4, 3000);
    quiet(&r, 600);
    TEST_ASSERT_EQUAL(1, r.trues);
    quiet(&r, 1000);   /* calm again: the next shake counts */
    one_shake(&r);
    quiet(&r, 600);
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
    RUN_TEST(test_shake_same_sign_train);
    RUN_TEST(test_shake_small_swing);
    RUN_TEST(test_shake_single_spike);
    RUN_TEST(test_shake_knock);
    RUN_TEST(test_shake_walk_like);
    RUN_TEST(test_shake_slow_tilt);
    RUN_TEST(test_shake_slow_peaks);
    RUN_TEST(test_shake_settle);
    RUN_TEST(test_shake_cooldown);
    RUN_TEST(test_shake_long_shake);
    RUN_TEST(test_shake_axis_switch);
    RUN_TEST(test_shake_cooldown_clock_wrap);
    return UNITY_END();
}
