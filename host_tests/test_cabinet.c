#include <math.h>
#include <string.h>
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
    TEST_ASSERT_TRUE(f[HY_CAB_GYRO_START_MS / CAB_SYN_STEP_MS].gh > 0.0f);   /* first unmasked row */

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
    const uint16_t want[] = { 100, 64, 40, 37, 34 };
    for (uint8_t s = 1; s <= 5; s++) TEST_ASSERT_EQUAL_UINT16(want[s - 1], hy_cab_wom_mg(s));
    TEST_ASSERT_EQUAL_UINT16(40, hy_cab_wom_mg(0));
    TEST_ASSERT_EQUAL_UINT16(40, hy_cab_wom_mg(6));
}

static void test_wom_hit_table_knock_steps(void)
{
    /* A knock peaking at 88 mg one sensor sample after a still one: under the least sensitive step, over 64 mg
       and the default (the default favours catching a door pulled by hand; see hy_cab_wom_mg). */
    hy_accel_t still = { 0, -1000, 0 }, knock = { 60, -1088, 30 };
    TEST_ASSERT_FALSE(hy_cab_wom_hit(still, knock, 100));
    TEST_ASSERT_TRUE(hy_cab_wom_hit(still, knock, 64));
    TEST_ASSERT_FALSE(hy_cab_wom_hit(still, knock, hy_cab_wom_mg(1)));
    TEST_ASSERT_TRUE(hy_cab_wom_hit(still, knock, hy_cab_wom_mg(3)));
    /* Strictly more than the threshold, on any axis, either sign. */
    TEST_ASSERT_FALSE(hy_cab_wom_hit(still, (hy_accel_t){ 100, -1000, 0 }, 100));
    TEST_ASSERT_TRUE(hy_cab_wom_hit(still, (hy_accel_t){ -101, -1000, 0 }, 100));
    TEST_ASSERT_TRUE(hy_cab_wom_hit(still, (hy_accel_t){ 0, -1000, 101 }, 100));
    TEST_ASSERT_FALSE(hy_cab_wom_hit(still, still, 40));
    /* The same knock in a 50 Hz stream from rest fires at its peak sample. */
    static hy_cab_seq_t q;
    memset(&q, 0, sizeof q);
    q.rest = still;
    q.n = 6;
    for (uint16_t i = 0; i < q.n; i++) {
        hy_accel_t a = i == 3 ? knock : still;
        q.s[i] = (hy_imu6_t){ { a.x, a.y, a.z }, { 0, 0, 0 } };
    }
    for (uint16_t i = 0; i < q.n; i++) {
        TEST_ASSERT_EQUAL(i == 3, hy_cab_wom_hit_at(&q, i, 64));
        TEST_ASSERT_FALSE(hy_cab_wom_hit_at(&q, i, 100));
    }
}

static void test_wom_prev_interpolates(void)
{
    /* 48 ms back from a 50 Hz sample: 0.6 of the sample 40 ms back plus 0.4 of the one 60 ms back. */
    hy_accel_t p = hy_cab_wom_prev((hy_accel_t){ 0, 100, -1000 }, (hy_accel_t){ 100, 0, -1100 });
    TEST_ASSERT_EQUAL_INT16(40, p.x);
    TEST_ASSERT_EQUAL_INT16(60, p.y);
    TEST_ASSERT_EQUAL_INT16(-1040, p.z);
    p = hy_cab_wom_prev((hy_accel_t){ 0, 0, 0 }, (hy_accel_t){ 1, -1, -3 });   /* rounded to the nearest mg */
    TEST_ASSERT_EQUAL_INT16(0, p.x);
    TEST_ASSERT_EQUAL_INT16(0, p.y);
    TEST_ASSERT_EQUAL_INT16(-1, p.z);
}

static void test_wom_hit_slope_not_drift(void)
{
    /* A slow tilt: y moves 5 mg per 20 ms sample, 12 mg per sensor sample, 495 mg from rest at the end. The sensor
       compares successive samples, so it never fires at 40 mg; a step of 150 mg fires at once. */
    static hy_cab_seq_t q;
    memset(&q, 0, sizeof q);
    q.rest = (hy_accel_t){ 0, -1000, 0 };
    q.n = 100;
    for (uint16_t i = 0; i < q.n; i++) q.s[i] = (hy_imu6_t){ { 0, (int16_t)(-1000 + 5 * (i + 1)), 0 }, { 0, 0, 0 } };
    for (uint16_t i = 0; i < q.n; i++) TEST_ASSERT_FALSE(hy_cab_wom_hit_at(&q, i, 40));
    TEST_ASSERT_TRUE(hy_cab_wom_hit_at(&q, 50, 11));   /* 12 mg per sensor sample */
    TEST_ASSERT_FALSE(hy_cab_wom_hit_at(&q, 50, 12));
    q.s[0].a[0] = 150;   /* the first sample against the rest before it */
    TEST_ASSERT_TRUE(hy_cab_wom_hit_at(&q, 0, 100));
    /* Back to 0 at sample 1: the previous sensor sample seen from sample 2 is 0.6 x 150 = 90 mg, from sample 3
       0.4 x 150 = 60 mg, so the step back fires at 89 and 59 mg but not at 90 and 60. */
    TEST_ASSERT_TRUE(hy_cab_wom_hit_at(&q, 2, 89));
    TEST_ASSERT_FALSE(hy_cab_wom_hit_at(&q, 2, 90));
    TEST_ASSERT_TRUE(hy_cab_wom_hit_at(&q, 3, 59));
    TEST_ASSERT_FALSE(hy_cab_wom_hit_at(&q, 3, 60));
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
    /* One slow swing of 4.8 s that never pauses (a tilt pauses at its turning point, which now ends a capture). */
    cab_syn_t p = noisy(CAB_SYN_DOOR_OPEN, 2);
    p.dur = 6.0f;
    hy_cab_cap_t c;
    uint32_t fed;
    TEST_ASSERT_EQUAL_INT(HY_CAP_TOO_LONG, run_capture(&p, 400, &c, &fed));
    TEST_ASSERT_EQUAL_UINT32(HY_CAB_MAX_N, fed);
    TEST_ASSERT_EQUAL_UINT16(HY_CAB_MAX_N, c.seq.n);
}

static void test_cap_new_pose_done(void)
{
    /* Turned by 60 degrees about x over 1 s at a steady 60 dps, then held: the capture ends in the new pose, far
       from the rest vector, and keeps the whole turn (a steady turn is never still). */
    const double pi = 3.14159265358979323846;
    hy_cab_cap_t c;
    hy_cab_cap_start(&c, (hy_accel_t){ 0, -1000, 0 }, true, 0);
    hy_cap_res_t r = HY_CAP_MORE;
    uint32_t i = 0;
    for (; i < 150 && r == HY_CAP_MORE; i++) {
        double t = i * 0.02, ang = (t < 1.0 ? t : 1.0) * 60.0 * pi / 180.0;
        double rate = t < 1.0 ? 600.0 : 0.0;   /* deci-dps */
        hy_imu6_t s = { { 0, cab_syn_i16(-1000.0 * cos(ang)), cab_syn_i16(1000.0 * sin(ang)) },
                        { cab_syn_i16(rate), 0, 0 } };
        r = hy_cab_cap_feed(&c, i * CAB_SYN_STEP_MS, &s);
    }
    TEST_ASSERT_EQUAL_INT(HY_CAP_DONE, r);
    TEST_ASSERT_UINT16_WITHIN(2, 50, c.seq.n);
    TEST_ASSERT_EQUAL_UINT32(50 + HY_CAB_QUIET_MS / CAB_SYN_STEP_MS, i);   /* ended once the hold was still */
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
    RUN_TEST(test_wom_hit_table_knock_steps);
    RUN_TEST(test_wom_prev_interpolates);
    RUN_TEST(test_wom_hit_slope_not_drift);
    RUN_TEST(test_cap_door_done);
    RUN_TEST(test_cap_door_done_with_lead);
    RUN_TEST(test_cap_too_short);
    RUN_TEST(test_cap_too_long);
    RUN_TEST(test_cap_new_pose_done);
    RUN_TEST(test_cap_without_gyro_ignores_gyro);
    RUN_TEST(test_still_tracker);
    return UNITY_END();
}
