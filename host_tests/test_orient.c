#include "unity.h"
#include "hy/motion.h"

void setUp(void) {}
void tearDown(void) {}

#define STEP 20

/* Feeds samples at t, t+STEP, ..., t+ms (inclusive) and advances *t past the last one. Returns how many fed
   samples returned true. */
static int run(hy_orient_t *o, uint32_t *t, uint32_t ms, hy_accel_t a)
{
    int n = 0;
    uint32_t t0 = *t;
    for (uint32_t d = 0; d <= ms; d += STEP) {
        if (hy_orient_feed(o, t0 + d, a)) n++;
        *t = t0 + d + STEP;
    }
    return n;
}

static const hy_accel_t A0 = {0, -1000, 0}, A90 = {1000, 0, 0}, A180 = {0, 1000, 0}, A270 = {-1000, 0, 0};
static const hy_accel_t FLAT = {0, 0, 1000};

static void check_cand(hy_accel_t a, bool want, hy_rot_t r)
{
    hy_rot_t out = HY_ROT_270;
    TEST_ASSERT_EQUAL(want, hy_orient_candidate(a, &out));
    if (want) TEST_ASSERT_EQUAL(r, out);
}

static void test_candidate_axes(void)
{
    check_cand(A0, true, HY_ROT_0);
    check_cand(A90, true, HY_ROT_90);
    check_cand(A180, true, HY_ROT_180);
    check_cand(A270, true, HY_ROT_270);
}

static void test_candidate_dead_band(void)
{
    check_cand((hy_accel_t){485, -875, 0}, true, HY_ROT_0);
    check_cand((hy_accel_t){515, -857, 0}, false, HY_ROT_0);
    check_cand((hy_accel_t){707, -707, 0}, false, HY_ROT_0);
}

static void test_candidate_flat_and_moving(void)
{
    check_cand((hy_accel_t){0, -400, 917}, false, HY_ROT_0);
    check_cand((hy_accel_t){0, -700, 0}, false, HY_ROT_0);
    check_cand((hy_accel_t){0, -1300, 0}, false, HY_ROT_0);
}

static void test_orient_dwell_1s(void)
{
    hy_orient_t o;
    hy_orient_init(&o, HY_ROT_0);
    uint32_t t = 1000;
    TEST_ASSERT_EQUAL(0, run(&o, &t, 980, A90));
    TEST_ASSERT_EQUAL(HY_ROT_0, hy_orient_current(&o));
    TEST_ASSERT_TRUE(hy_orient_feed(&o, t, A90));   /* the 1000 ms sample */
    TEST_ASSERT_EQUAL(HY_ROT_90, hy_orient_current(&o));
    t += STEP;
    TEST_ASSERT_EQUAL(0, run(&o, &t, 2000, A90));
}

static void test_orient_interrupted(void)
{
    hy_orient_t o;
    hy_orient_init(&o, HY_ROT_0);
    uint32_t t = 0;
    TEST_ASSERT_EQUAL(0, run(&o, &t, 600, A90));
    TEST_ASSERT_EQUAL(0, run(&o, &t, 0, FLAT));
    TEST_ASSERT_EQUAL(0, run(&o, &t, 600, A90));
    TEST_ASSERT_EQUAL(HY_ROT_0, hy_orient_current(&o));
    TEST_ASSERT_EQUAL(1, run(&o, &t, 400, A90));
    TEST_ASSERT_EQUAL(HY_ROT_90, hy_orient_current(&o));
}

static void test_orient_gap_restarts(void)
{
    hy_orient_t o;
    hy_orient_init(&o, HY_ROT_0);
    uint32_t t = 0;
    TEST_ASSERT_EQUAL(0, run(&o, &t, 600, A90));
    t = t - STEP + 300;   /* next sample 300 ms after the last */
    TEST_ASSERT_EQUAL(0, run(&o, &t, 900, A90));
    TEST_ASSERT_EQUAL(HY_ROT_0, hy_orient_current(&o));
    TEST_ASSERT_EQUAL(1, run(&o, &t, 100, A90));
    TEST_ASSERT_EQUAL(HY_ROT_90, hy_orient_current(&o));
}

/* sin of deg degrees times 1000, 0..90, from a 5 degree table with linear interpolation */
static int sin_m(int deg)
{
    static const int s5[] = {0, 87, 174, 259, 342, 423, 500, 574, 643, 707, 766, 819, 866, 906, 940, 966, 985,
                             996, 1000};
    int i = deg / 5, f = deg % 5;
    if (i >= 18) return 1000;
    return s5[i] + (s5[i + 1] - s5[i]) * f / 5;
}

/* Gravity reaction at deg degrees from -y toward +x, magnitude 1000 */
static hy_accel_t at_deg(int deg)
{
    hy_accel_t a = {(int16_t)sin_m(deg), (int16_t)-sin_m(90 - deg), 0};
    return a;
}

/* Alternates p and q every 300 ms (15 samples) for total_ms; returns how many samples returned true. */
static int alternate(hy_orient_t *o, hy_accel_t p, hy_accel_t q, uint32_t total_ms)
{
    uint32_t t = 0;
    int trues = 0;
    bool first = true;
    while (t < total_ms) {
        for (int i = 0; i < 15; i++) {
            if (hy_orient_feed(o, t, first ? p : q)) trues++;
            t += STEP;
        }
        first = !first;
    }
    return trues;
}

static void test_orient_diagonal_no_flap(void)
{
    hy_orient_t o;
    hy_orient_init(&o, HY_ROT_0);
    TEST_ASSERT_EQUAL(0, alternate(&o, at_deg(40), at_deg(50), 10000));
    TEST_ASSERT_EQUAL(HY_ROT_0, hy_orient_current(&o));

    hy_orient_init(&o, HY_ROT_0);
    TEST_ASSERT_EQUAL(0, alternate(&o, at_deg(25), at_deg(65), 10000));
    TEST_ASSERT_EQUAL(HY_ROT_0, hy_orient_current(&o));
}

static void test_orient_flat_keeps(void)
{
    hy_orient_t o;
    hy_orient_init(&o, HY_ROT_180);
    uint32_t t = 0;
    int n = run(&o, &t, 5000, FLAT);
    /* slow in-plane spin of a 300 mg component on top of 950 mg out of the screen */
    for (int deg = 0; deg < 360; deg += 4) {
        int q = deg / 90, r = deg % 90;
        hy_accel_t a = at_deg(r);
        int x = a.x, y = a.y;
        for (int k = 0; k < q; k++) { int nx = -y, ny = x; x = nx; y = ny; }   /* quarter turns */
        a.x = (int16_t)(x * 3 / 10);
        a.y = (int16_t)(y * 3 / 10);
        a.z = 950;
        n += run(&o, &t, 80, a);
    }
    TEST_ASSERT_EQUAL(0, n);
    TEST_ASSERT_EQUAL(HY_ROT_180, hy_orient_current(&o));
}

static void test_orient_resume_quick(void)
{
    hy_orient_t o;
    hy_orient_init(&o, HY_ROT_0);
    uint32_t t = 0;
    hy_orient_resume(&o);
    TEST_ASSERT_EQUAL(1, run(&o, &t, 260, A270));
    TEST_ASSERT_EQUAL(HY_ROT_270, hy_orient_current(&o));
    TEST_ASSERT_EQUAL(0, run(&o, &t, 260, A0));
    TEST_ASSERT_EQUAL(HY_ROT_270, hy_orient_current(&o));
    TEST_ASSERT_EQUAL(1, run(&o, &t, 740, A0));
    TEST_ASSERT_EQUAL(HY_ROT_0, hy_orient_current(&o));
}

static void test_orient_init_is_slow(void)
{
    hy_orient_t o;
    hy_orient_init(&o, HY_ROT_0);
    uint32_t t = 0;
    TEST_ASSERT_EQUAL(0, run(&o, &t, 260, A90));
}

static void test_orient_quick_ends_after_full_dwell(void)
{
    hy_orient_t o;
    hy_orient_init(&o, HY_ROT_0);
    uint32_t t = 0;
    hy_orient_resume(&o);
    TEST_ASSERT_EQUAL(0, run(&o, &t, 1100, FLAT));
    TEST_ASSERT_EQUAL(0, run(&o, &t, 260, A90));   /* slow dwell now */
    TEST_ASSERT_EQUAL(1, run(&o, &t, 740, A90));
}

static void test_orient_same_as_cur(void)
{
    hy_orient_t o;
    hy_orient_init(&o, HY_ROT_90);
    uint32_t t = 0;
    TEST_ASSERT_EQUAL(0, run(&o, &t, 5000, A90));
    TEST_ASSERT_EQUAL(HY_ROT_90, hy_orient_current(&o));
}

static void test_orient_set(void)
{
    hy_orient_t o;
    hy_orient_init(&o, HY_ROT_0);
    uint32_t t = 0;
    run(&o, &t, 600, A90);
    hy_orient_set(&o, HY_ROT_180);
    TEST_ASSERT_EQUAL(HY_ROT_180, hy_orient_current(&o));
    TEST_ASSERT_EQUAL(0, run(&o, &t, 600, A90));   /* candidate was cleared: dwell restarts */
    TEST_ASSERT_EQUAL(1, run(&o, &t, 400, A90));
}

/* The in-plane gravity vector of 1000 mg turned from 0 to 90 degrees at 15 degrees per second, then held. */
static void test_orient_slow_turn(void)
{
    hy_orient_t o;
    hy_orient_init(&o, HY_ROT_0);
    uint32_t t = 0;
    int n = 0;
    for (uint32_t ms = 0; ms <= 6000; ms += STEP) {
        int deg = (int)(ms * 15 / 1000);
        n += hy_orient_feed(&o, t, at_deg(deg > 90 ? 90 : deg)) ? 1 : 0;
        t += STEP;
    }
    n += run(&o, &t, 2000, at_deg(90));
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(HY_ROT_90, hy_orient_current(&o));
}

/* The in-plane component alternates between 490 and 510 mg along +x every 200 ms, z makes the total 1000 mg:
   the candidate flips between none and 90, so the dwell keeps restarting. */
static void test_orient_plane_threshold(void)
{
    hy_orient_t o;
    hy_orient_init(&o, HY_ROT_0);
    const hy_accel_t lo = {490, 0, 872}, hi = {510, 0, 860};
    uint32_t t = 0;
    int n = 0;
    for (int i = 0; i < 25; i++) {
        n += run(&o, &t, 180, (i & 1) ? hi : lo);
    }
    TEST_ASSERT_EQUAL(0, n);
    TEST_ASSERT_EQUAL(HY_ROT_0, hy_orient_current(&o));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_candidate_axes);
    RUN_TEST(test_candidate_dead_band);
    RUN_TEST(test_candidate_flat_and_moving);
    RUN_TEST(test_orient_dwell_1s);
    RUN_TEST(test_orient_interrupted);
    RUN_TEST(test_orient_gap_restarts);
    RUN_TEST(test_orient_diagonal_no_flap);
    RUN_TEST(test_orient_flat_keeps);
    RUN_TEST(test_orient_resume_quick);
    RUN_TEST(test_orient_init_is_slow);
    RUN_TEST(test_orient_quick_ends_after_full_dwell);
    RUN_TEST(test_orient_same_as_cur);
    RUN_TEST(test_orient_set);
    RUN_TEST(test_orient_slow_turn);
    RUN_TEST(test_orient_plane_threshold);
    return UNITY_END();
}
