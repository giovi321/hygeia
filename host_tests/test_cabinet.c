#include <math.h>
#include "unity.h"
#include "hy/cabinet.h"
#include "cab_synth.h"

void setUp(void) {}
void tearDown(void) {}

static hy_cab_seq_t seq_of(const cab_syn_t *p, uint32_t total_ms)
{
    hy_cab_seq_t q;
    cab_syn_make(p, total_ms, &q);
    return q;
}

static cab_syn_t noisy(cab_syn_kind_t kind, uint32_t seed)
{
    cab_syn_t p = cab_syn_new(kind);
    p.noise = true;
    p.seed = seed;
    return p;
}

/* Feeds the movement to a capture started on its first sample; returns the first result other than MORE
   (or MORE when max_n samples did not decide). *n_fed is the number of samples fed. */
static hy_cap_res_t run_capture(const cab_syn_t *p, uint32_t max_n, hy_cab_cap_t *c, uint32_t *n_fed)
{
    hy_cab_cap_start(c, p->rest, p->has_gyro, 1000);
    hy_cap_res_t r = HY_CAP_MORE;
    uint32_t i = 0;
    for (; i < max_n && r == HY_CAP_MORE; i++) {
        hy_imu6_t s = cab_syn_sample(p, i);
        r = hy_cab_cap_feed(c, 1000 + i * CAB_SYN_STEP_MS, &s);
    }
    *n_fed = i;
    return r;
}

static void test_feat_door_vertical(void)
{
    cab_syn_t p = noisy(CAB_SYN_DOOR_OPEN, 7);
    hy_cab_seq_t q = seq_of(&p, 1000);
    hy_cab_feat_t f[HY_CAB_MAX_N];
    hy_cab_features(&q, f);
    float gv_max = -1000.0f, gh_max = 0.0f;
    for (uint16_t i = 0; i < q.n; i++) {
        if (f[i].gv > gv_max) gv_max = f[i].gv;
        if (f[i].gh > gh_max) gh_max = f[i].gh;
    }
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 120.0f, gv_max);
    TEST_ASSERT_TRUE(gh_max < 5.0f);
}

static void test_feat_door_close_negative(void)
{
    cab_syn_t p = noisy(CAB_SYN_DOOR_CLOSE, 7);
    hy_cab_seq_t q = seq_of(&p, 1000);
    hy_cab_feat_t f[HY_CAB_MAX_N];
    hy_cab_features(&q, f);
    float gv_min = 1000.0f;
    for (uint16_t i = 0; i < q.n; i++)
        if (f[i].gv < gv_min) gv_min = f[i].gv;
    TEST_ASSERT_FLOAT_WITHIN(5.0f, -120.0f, gv_min);
}

static void test_feat_tilt_and_lift_channels(void)
{
    cab_syn_t pt = cab_syn_new(CAB_SYN_TILT);
    hy_cab_seq_t qt = seq_of(&pt, 1200);
    hy_cab_feat_t ft[HY_CAB_MAX_N];
    hy_cab_features(&qt, ft);
    float gv_abs = 0.0f, gh_max = 0.0f;
    for (uint16_t i = 0; i < qt.n; i++) {
        if (fabsf(ft[i].gv) > gv_abs) gv_abs = fabsf(ft[i].gv);
        if (ft[i].gh > gh_max) gh_max = ft[i].gh;
    }
    TEST_ASSERT_TRUE(gv_abs < 2.0f);       /* a tilt turns about a horizontal axis only */
    TEST_ASSERT_TRUE(gh_max > 60.0f);      /* 40 degrees over 1 s peaks at 40 * pi = 125 dps */

    cab_syn_t pl = cab_syn_new(CAB_SYN_LIFT);
    hy_cab_seq_t ql = seq_of(&pl, 800);
    hy_cab_feat_t fl[HY_CAB_MAX_N];
    hy_cab_features(&ql, fl);
    float av_max = -1000.0f, av_min = 1000.0f, ah_max = 0.0f;
    for (uint16_t i = 0; i < ql.n; i++) {
        if (fl[i].av > av_max) av_max = fl[i].av;
        if (fl[i].av < av_min) av_min = fl[i].av;
        if (fl[i].ah > ah_max) ah_max = fl[i].ah;
        TEST_ASSERT_EQUAL_FLOAT(0.0f, fl[i].gv);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, fl[i].gh);
    }
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 300.0f, av_max);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, -300.0f, av_min);
    TEST_ASSERT_TRUE(ah_max < 2.0f);
}

static void check_same_features(const hy_cab_seq_t *a, const hy_cab_seq_t *b)
{
    hy_cab_feat_t fa[HY_CAB_MAX_N], fb[HY_CAB_MAX_N];
    TEST_ASSERT_EQUAL_UINT16(a->n, b->n);
    hy_cab_features(a, fa);
    hy_cab_features(b, fb);
    for (uint16_t i = 0; i < a->n; i++) {
        TEST_ASSERT_FLOAT_WITHIN(0.5f, fa[i].gv, fb[i].gv);
        TEST_ASSERT_FLOAT_WITHIN(0.5f, fa[i].gh, fb[i].gh);
        TEST_ASSERT_FLOAT_WITHIN(2.0f, fa[i].av, fb[i].av);
        TEST_ASSERT_FLOAT_WITHIN(2.0f, fa[i].ah, fb[i].ah);
    }
}

static void test_feat_rotation_invariant(void)
{
    const cab_syn_kind_t kinds[] = { CAB_SYN_DOOR_OPEN, CAB_SYN_TILT };
    for (unsigned k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        cab_syn_t p = noisy(kinds[k], 11 + k);
        hy_cab_seq_t base = seq_of(&p, 1400);
        const double z_deg[] = { 90.0, 180.0, 270.0 };
        for (unsigned d = 0; d < 3; d++) {
            hy_cab_seq_t q = base;
            cab_syn_rotate(&q, 0, 0, 1, z_deg[d]);
            check_same_features(&base, &q);
        }
        hy_cab_seq_t q = base;
        cab_syn_rotate(&q, base.rest.x, base.rest.y, base.rest.z, 37.0);
        check_same_features(&base, &q);
    }
}

static void test_feat_gyro_mask(void)
{
    cab_syn_t p = noisy(CAB_SYN_DOOR_OPEN, 3);
    hy_cab_seq_t q = seq_of(&p, 1000);
    hy_cab_feat_t f[HY_CAB_MAX_N];
    hy_cab_features(&q, f);
    /* The noise gives the first rows a nonzero gyro reading, so zero rows come from the mask. */
    TEST_ASSERT_TRUE(q.s[0].g[0] != 0 || q.s[0].g[1] != 0 || q.s[0].g[2] != 0);
    for (uint16_t i = 0; i < q.n; i++) {
        if ((uint32_t)i * CAB_SYN_STEP_MS < HY_CAB_GYRO_START_MS) {
            TEST_ASSERT_EQUAL_FLOAT(0.0f, f[i].gv);
            TEST_ASSERT_EQUAL_FLOAT(0.0f, f[i].gh);
        }
    }
    TEST_ASSERT_TRUE(f[3].gh > 0.0f);      /* first unmasked row (60 ms) */

    q.has_gyro = false;
    hy_cab_features(&q, f);
    for (uint16_t i = 0; i < q.n; i++) {
        TEST_ASSERT_EQUAL_FLOAT(0.0f, f[i].gv);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, f[i].gh);
    }
    TEST_ASSERT_TRUE(fabsf(f[20].av) + f[20].ah > 0.0f);   /* accel rows still computed */
}

static void test_wom_mapping(void)
{
    const uint16_t want[] = { 250, 160, 100, 64, 40 };
    for (uint8_t s = 1; s <= 5; s++) TEST_ASSERT_EQUAL_UINT16(want[s - 1], hy_cab_wom_mg(s));
    TEST_ASSERT_EQUAL_UINT16(100, hy_cab_wom_mg(0));
    TEST_ASSERT_EQUAL_UINT16(100, hy_cab_wom_mg(6));
}

static void test_wom_hit_table_knock_default(void)
{
    hy_accel_t rest = { 0, -1000, 0 }, knock = { 60, -1088, 30 };
    TEST_ASSERT_FALSE(hy_cab_wom_hit(rest, knock, 100));
    TEST_ASSERT_TRUE(hy_cab_wom_hit(rest, knock, 64));
    /* Strictly more than the threshold, on any axis, either sign. */
    TEST_ASSERT_FALSE(hy_cab_wom_hit(rest, (hy_accel_t){ 100, -1000, 0 }, 100));
    TEST_ASSERT_TRUE(hy_cab_wom_hit(rest, (hy_accel_t){ -101, -1000, 0 }, 100));
    TEST_ASSERT_TRUE(hy_cab_wom_hit(rest, (hy_accel_t){ 0, -1000, 101 }, 100));
    TEST_ASSERT_FALSE(hy_cab_wom_hit(rest, rest, 40));
}

static void test_cap_door_done(void)
{
    cab_syn_t p = noisy(CAB_SYN_DOOR_OPEN, 5);
    hy_cab_cap_t c;
    uint32_t fed;
    hy_cap_res_t r = run_capture(&p, 100, &c, &fed);
    TEST_ASSERT_EQUAL_INT(HY_CAP_DONE, r);
    /* 0.8 s is 40 samples; the 400 ms quiet tail is trimmed, so it is not in n. */
    TEST_ASSERT_UINT16_WITHIN(2, 40, c.seq.n);
    TEST_ASSERT_TRUE(fed >= (uint32_t)c.seq.n + HY_CAB_QUIET_MS / CAB_SYN_STEP_MS);
    TEST_ASSERT_TRUE(c.seq.has_gyro);
}

static void test_cap_door_done_with_lead(void)
{
    /* A trigger that fires 100 ms before the swing: the still lead stays in the capture. */
    cab_syn_t p = noisy(CAB_SYN_DOOR_OPEN, 5);
    p.start_ms = 100;
    hy_cab_cap_t c;
    uint32_t fed;
    TEST_ASSERT_EQUAL_INT(HY_CAP_DONE, run_capture(&p, 100, &c, &fed));
    TEST_ASSERT_UINT16_WITHIN(2, 45, c.seq.n);
}

static void test_cap_too_short(void)
{
    cab_syn_t p = noisy(CAB_SYN_LIFT, 9);
    p.dur = 200.0f / 600.0f;               /* a 200 ms bump */
    hy_cab_cap_t c;
    uint32_t fed;
    TEST_ASSERT_EQUAL_INT(HY_CAP_TOO_SHORT, run_capture(&p, 100, &c, &fed));
}

static void test_cap_too_long(void)
{
    cab_syn_t p = noisy(CAB_SYN_TILT, 2);
    p.dur = 5.0f;                          /* one slow 5 s wobble */
    hy_cab_cap_t c;
    uint32_t fed;
    TEST_ASSERT_EQUAL_INT(HY_CAP_TOO_LONG, run_capture(&p, 400, &c, &fed));
    TEST_ASSERT_EQUAL_UINT32(HY_CAB_MAX_N, fed);
    TEST_ASSERT_EQUAL_UINT16(HY_CAB_MAX_N, c.seq.n);
}

static void test_cap_without_gyro_ignores_gyro(void)
{
    /* Gyro readings that would never be quiet must not matter when the capture has no gyro. */
    cab_syn_t p = cab_syn_new(CAB_SYN_LIFT);
    p.has_gyro = false;
    hy_cab_cap_t c;
    hy_cab_cap_start(&c, p.rest, false, 0);
    hy_cap_res_t r = HY_CAP_MORE;
    for (uint32_t i = 0; i < 100 && r == HY_CAP_MORE; i++) {
        hy_imu6_t s = cab_syn_sample(&p, i);
        s.g[0] = 5000;
        r = hy_cab_cap_feed(&c, i * CAB_SYN_STEP_MS, &s);
    }
    TEST_ASSERT_EQUAL_INT(HY_CAP_DONE, r);
}

static void test_still_tracker(void)
{
    hy_cab_still_t st;
    hy_cab_still_reset(&st);
    hy_accel_t rest = { 0, 0, 0 };
    bool got = false;
    uint32_t t = 0;
    /* A slow drift of 5 mg per step stays under the step limit. */
    for (int i = 0; i <= 50; i++, t += 20) {
        hy_accel_t a = { (int16_t)(i / 2), (int16_t)(-1000 + i), 0 };
        bool r = hy_cab_still_feed(&st, t, a, &rest);
        if (t < 1000) TEST_ASSERT_FALSE(r);
        if (t == 1000) {
            TEST_ASSERT_TRUE(r);
            got = true;
        }
    }
    TEST_ASSERT_TRUE(got);
    /* Mean of the last 10 samples (i = 41..50): x = 20..25 -> 22.5 -> 23, y = -959..-950 -> -954.5 -> -955. */
    TEST_ASSERT_EQUAL_INT16(23, rest.x);
    TEST_ASSERT_EQUAL_INT16(-955, rest.y);
    TEST_ASSERT_EQUAL_INT16(0, rest.z);

    /* A 50 mg step restarts the period: false for the next second, true again 1000 ms after the step. */
    hy_accel_t b = { 75, -950, 0 };
    uint32_t t_step = t;
    for (; t < t_step + 1000; t += 20) TEST_ASSERT_FALSE(hy_cab_still_feed(&st, t, b, &rest));
    TEST_ASSERT_TRUE(hy_cab_still_feed(&st, t, b, &rest));
    TEST_ASSERT_EQUAL_INT16(75, rest.x);
    TEST_ASSERT_EQUAL_INT16(-950, rest.y);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_feat_door_vertical);
    RUN_TEST(test_feat_door_close_negative);
    RUN_TEST(test_feat_tilt_and_lift_channels);
    RUN_TEST(test_feat_rotation_invariant);
    RUN_TEST(test_feat_gyro_mask);
    RUN_TEST(test_wom_mapping);
    RUN_TEST(test_wom_hit_table_knock_default);
    RUN_TEST(test_cap_door_done);
    RUN_TEST(test_cap_door_done_with_lead);
    RUN_TEST(test_cap_too_short);
    RUN_TEST(test_cap_too_long);
    RUN_TEST(test_cap_without_gyro_ignores_gyro);
    RUN_TEST(test_still_tracker);
    return UNITY_END();
}
