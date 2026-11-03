#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "unity.h"
#include "hy/cabinet.h"
#include "cab_synth.h"

void setUp(void) {}
void tearDown(void) {}

#define LEAD_MS 400
#define N_POS   20

static hy_accel_t accel_of(const hy_imu6_t *s)
{
    return (hy_accel_t){ s->a[0], s->a[1], s->a[2] };
}

/* The trigger at stream sample k of p, against the sensor's previous sample; before sample 0 p is at rest. */
static bool stream_hit(const cab_syn_t *p, uint32_t k, uint16_t trig_mg)
{
    hy_accel_t back[2];
    for (uint32_t j = 0; j < 2; j++) {
        if (k >= j + 2) {
            hy_imu6_t b = cab_syn_sample(p, k - j - 2);
            back[j] = accel_of(&b);
        } else {
            back[j] = p->rest;
        }
    }
    hy_imu6_t s = cab_syn_sample(p, k);
    return hy_cab_wom_hit(hy_cab_wom_prev(back[0], back[1]), accel_of(&s), trig_mg);
}

static cab_syn_t mv(cab_syn_kind_t kind, float amp, float dur, uint32_t seed)
{
    cab_syn_t p = cab_syn_new(kind);
    p.amp = amp;
    p.dur = dur;
    p.noise = true;
    p.seed = seed;
    p.start_ms = LEAD_MS;
    return p;
}

/* A capture of p as the device makes it: started pre samples before the first sample at which a trigger of
   trig_mg fires, ended by the capture segmenter. False when the trigger never fires or the segmenter discards
   the capture (too short), as the device then matches nothing. */
static bool try_capture(const cab_syn_t *p, uint16_t trig_mg, uint32_t pre, hy_cab_seq_t *out)
{
    uint32_t k = 0;
    for (; k < 400; k++) {
        if (stream_hit(p, k, trig_mg)) break;
    }
    if (k == 400) return false;
    uint32_t i = k >= pre ? k - pre : 0;
    static hy_cab_cap_t c;
    hy_cab_cap_start(&c, p->rest, p->has_gyro, 0);
    hy_cap_res_t r = HY_CAP_MORE;
    for (uint32_t t = 0; r == HY_CAP_MORE && t < 400; i++, t++) {
        hy_imu6_t s = cab_syn_sample(p, i);
        r = hy_cab_cap_feed(&c, t * CAB_SYN_STEP_MS, &s);
    }
    TEST_ASSERT_TRUE(r != HY_CAP_MORE && r != HY_CAP_TOO_LONG);
    *out = c.seq;
    return r == HY_CAP_DONE;
}

static void capture(const cab_syn_t *p, uint16_t trig_mg, uint32_t pre, hy_cab_seq_t *out)
{
    TEST_ASSERT_TRUE(try_capture(p, trig_mg, pre, out));
}

/* The three takes of a teach: the teach trigger, sizes a person's repeats might differ by. */
static void teach(cab_syn_kind_t kind, hy_cab_seq_t takes[HY_CAB_TAKES])
{
    static const float amp[HY_CAB_TAKES] = { 1.0f, 1.1f, 0.9f };
    static const float dur[HY_CAB_TAKES] = { 1.0f, 0.9f, 1.1f };
    for (int k = 0; k < HY_CAB_TAKES; k++) {
        cab_syn_t p = mv(kind, amp[k], dur[k], 11u + (uint32_t)k);
        capture(&p, HY_CAB_TEACH_TRIG_MG, 0, &takes[k]);
    }
}

static void build(cab_syn_kind_t kind, hy_cab_model_t *m)
{
    static hy_cab_seq_t takes[HY_CAB_TAKES];
    teach(kind, takes);
    int odd = 99;
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_model_build(m, takes, &odd));
    TEST_ASSERT_EQUAL(-1, odd);
    TEST_ASSERT_TRUE(m->valid);
}

/* Variant n of a movement: a new seed, amplitude 0.85 to 1.15, duration 0.8 to 1.2, start offset 0 to 160 ms.
   False when it stays under the trigger level. */
static bool variant(cab_syn_kind_t kind, uint32_t n, uint16_t trig_mg, hy_cab_seq_t *out)
{
    float amp = 0.85f + 0.30f * (float)((n * 7u) % N_POS) / (N_POS - 1);
    float dur = 0.80f + 0.40f * (float)((n * 13u) % N_POS) / (N_POS - 1);
    cab_syn_t p = mv(kind, amp, dur, 1000u + n * 17u + (uint32_t)kind * 100u);
    return try_capture(&p, trig_mg, (n * 5u) % 9u, out);
}

/* Matches of 20 variants; a variant the device would not capture at the model's trigger level counts as no
   match (distance INFINITY). */
static uint32_t count_matches(const hy_cab_model_t *m, cab_syn_kind_t kind, bool rotate, float *worst, float *nearest)
{
    static hy_cab_seq_t q;
    uint32_t hits = 0;
    *worst = 0.0f;
    *nearest = INFINITY;
    for (uint32_t n = 0; n < N_POS; n++) {
        float d = INFINITY;
        if (!variant(kind, n, m->trig_mg, &q)) {
            *worst = INFINITY;
            continue;
        }
        if (rotate) cab_syn_rotate(&q, 0, 0, 1, 90);
        if (hy_cab_match(m, 3, &q, &d)) hits++;
        if (d > *worst) *worst = d;
        if (d < *nearest) *nearest = d;
    }
    return hits;
}

static void expect_all(const hy_cab_model_t *m, cab_syn_kind_t kind, bool rotate)
{
    float worst, nearest;
    uint32_t hits = count_matches(m, kind, rotate, &worst, &nearest);
    char msg[96];
    snprintf(msg, sizeof msg, "kind %d: worst %.3f, threshold %.3f", (int)kind, worst, hy_cab_threshold(m, 3));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(N_POS, hits, msg);
}

static void expect_none(const hy_cab_model_t *m, cab_syn_kind_t kind)
{
    float worst, nearest;
    uint32_t hits = count_matches(m, kind, false, &worst, &nearest);
    char msg[96];
    snprintf(msg, sizeof msg, "kind %d: nearest %.3f, threshold %.3f", (int)kind, nearest, hy_cab_threshold(m, 3));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, hits, msg);
}

static void test_dist_identity(void)
{
    static hy_cab_seq_t q;
    cab_syn_t p = mv(CAB_SYN_DOOR_OPEN, 1.0f, 1.0f, 3);
    capture(&p, HY_CAB_TEACH_TRIG_MG, 0, &q);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, hy_cab_dist(&q, &q));
}

static void test_dist_time_warp(void)
{
    static hy_cab_seq_t a, b, other;
    cab_syn_t pa = mv(CAB_SYN_DOOR_OPEN, 1.0f, 1.0f, 3);
    cab_syn_t pb = mv(CAB_SYN_DOOR_OPEN, 1.0f, 1.2f, 4);
    cab_syn_t po = mv(CAB_SYN_DOOR_CLOSE, 1.0f, 1.0f, 5);
    capture(&pa, HY_CAB_TEACH_TRIG_MG, 0, &a);
    capture(&pb, HY_CAB_TEACH_TRIG_MG, 0, &b);
    capture(&po, HY_CAB_TEACH_TRIG_MG, 0, &other);
    float d_warp = hy_cab_dist(&a, &b), d_other = hy_cab_dist(&a, &other);
    TEST_ASSERT_TRUE(isfinite(d_other));
    TEST_ASSERT_TRUE(d_warp < 0.3f * d_other);
}

static void test_dist_open_end(void)
{
    static hy_cab_seq_t tmpl, alone, longer;
    cab_syn_t pt = mv(CAB_SYN_DOOR_OPEN, 1.0f, 1.0f, 3);
    cab_syn_t pc = mv(CAB_SYN_DOOR_OPEN, 1.0f, 1.0f, 4);
    capture(&pt, HY_CAB_TEACH_TRIG_MG, 0, &tmpl);
    capture(&pc, HY_CAB_TEACH_TRIG_MG, 0, &alone);
    /* The swing, then 1 s of another movement (a tilt, as when reaching into the cabinet). */
    longer = alone;
    cab_syn_t tilt = mv(CAB_SYN_TILT, 1.0f, 1.0f, 6);
    tilt.start_ms = 0;
    for (uint32_t i = 0; i < HY_CAB_RATE_HZ; i++) longer.s[longer.n++] = cab_syn_sample(&tilt, i);
    float d_alone = hy_cab_dist(&tmpl, &alone), d_longer = hy_cab_dist(&tmpl, &longer);
    TEST_ASSERT_TRUE(isfinite(d_alone));
    TEST_ASSERT_FLOAT_WITHIN(0.1f * d_alone, d_alone, d_longer);
}

static void test_dist_gate_energy(void)
{
    static hy_cab_seq_t tmpl, small;
    cab_syn_t p = mv(CAB_SYN_DOOR_OPEN, 1.0f, 1.0f, 3);
    capture(&p, HY_CAB_TEACH_TRIG_MG, 0, &tmpl);
    small = tmpl;
    for (uint16_t i = 0; i < small.n; i++) {
        hy_imu6_t *s = &small.s[i];
        s->a[0] = (int16_t)lroundf(small.rest.x + 0.2f * (s->a[0] - small.rest.x));
        s->a[1] = (int16_t)lroundf(small.rest.y + 0.2f * (s->a[1] - small.rest.y));
        s->a[2] = (int16_t)lroundf(small.rest.z + 0.2f * (s->a[2] - small.rest.z));
        for (int c = 0; c < 3; c++) s->g[c] = (int16_t)lroundf(0.2f * s->g[c]);
    }
    TEST_ASSERT_TRUE(isinf(hy_cab_dist(&tmpl, &small)));
}

static void test_match_door_positive_negative(void)
{
    static hy_cab_model_t m;
    build(CAB_SYN_DOOR_OPEN, &m);
    expect_all(&m, CAB_SYN_DOOR_OPEN, false);
    expect_none(&m, CAB_SYN_DOOR_CLOSE);
    expect_none(&m, CAB_SYN_TILT);
    expect_none(&m, CAB_SYN_LIFT);
}

static void test_match_tilt_model(void)
{
    static hy_cab_model_t m;
    build(CAB_SYN_TILT, &m);
    expect_all(&m, CAB_SYN_TILT, false);
    expect_none(&m, CAB_SYN_DOOR_OPEN);
    expect_none(&m, CAB_SYN_LIFT);
    /* A door stays under this model's trigger level; captured at the teach level it still does not match. */
    static hy_cab_seq_t q;
    for (uint32_t n = 0; n < N_POS; n++) {
        TEST_ASSERT_TRUE(variant(CAB_SYN_DOOR_OPEN, n, HY_CAB_TEACH_TRIG_MG, &q));
        TEST_ASSERT_FALSE(hy_cab_match(&m, 5, &q, NULL));
    }
}

static void test_match_lift_model(void)
{
    static hy_cab_model_t m;
    build(CAB_SYN_LIFT, &m);
    expect_all(&m, CAB_SYN_LIFT, false);
    expect_none(&m, CAB_SYN_TILT);
    expect_none(&m, CAB_SYN_DOOR_OPEN);
    /* Some doors stay under this model's trigger level; captured at the teach level they still do not match. */
    static hy_cab_seq_t q;
    for (uint32_t n = 0; n < N_POS; n++) {
        TEST_ASSERT_TRUE(variant(CAB_SYN_DOOR_OPEN, n, HY_CAB_TEACH_TRIG_MG, &q));
        TEST_ASSERT_FALSE(hy_cab_match(&m, 5, &q, NULL));
    }
}

static void test_match_rotated_device(void)
{
    static hy_cab_model_t m;
    build(CAB_SYN_DOOR_OPEN, &m);
    expect_all(&m, CAB_SYN_DOOR_OPEN, true);
}

static void test_sensitivity_monotone(void)
{
    static hy_cab_model_t m;
    static hy_cab_seq_t q;
    build(CAB_SYN_DOOR_OPEN, &m);
    /* Captures from a close fit to a poor one, so the match flips somewhere along the sensitivity steps. */
    static const float amp[] = { 1.0f, 0.8f, 0.7f, 0.6f, 1.3f, 1.5f, 1.7f };
    uint32_t flips = 0;
    for (size_t k = 0; k < sizeof amp / sizeof amp[0]; k++) {
        cab_syn_t p = mv(CAB_SYN_DOOR_OPEN, amp[k], 1.0f, 500u + (uint32_t)k);
        capture(&p, m.trig_mg, 2, &q);
        bool prev = false;
        for (uint8_t s = 1; s <= 5; s++) {
            TEST_ASSERT_TRUE(hy_cab_threshold(&m, s) >= (s > 1 ? hy_cab_threshold(&m, s - 1) : 0.0f));
            bool hit = hy_cab_match(&m, s, &q, NULL);
            if (prev) TEST_ASSERT_TRUE(hit);
            if (hit && !prev && s > 1) flips++;
            prev = hit;
        }
    }
    TEST_ASSERT_TRUE(flips > 0);
}

static void test_model_odd_take(void)
{
    static hy_cab_seq_t takes[HY_CAB_TAKES];
    static hy_cab_model_t m;
    cab_syn_t p0 = mv(CAB_SYN_TILT, 1.0f, 1.0f, 21), p1 = mv(CAB_SYN_TILT, 1.1f, 0.9f, 22);
    cab_syn_t p2 = mv(CAB_SYN_LIFT, 1.0f, 1.0f, 23);
    capture(&p0, HY_CAB_TEACH_TRIG_MG, 0, &takes[0]);
    capture(&p1, HY_CAB_TEACH_TRIG_MG, 0, &takes[1]);
    capture(&p2, HY_CAB_TEACH_TRIG_MG, 0, &takes[2]);
    int odd = -1;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_cab_model_build(&m, takes, &odd));
    TEST_ASSERT_EQUAL(2, odd);
    TEST_ASSERT_FALSE(m.valid);
    TEST_ASSERT_FALSE(hy_cab_match(&m, 3, &takes[0], NULL));
    /* The model is still filled: accepted as is, the lift never fits, so d_self is infinite and the threshold
       is the upper bound. */
    TEST_ASSERT_TRUE(isinf(m.d_self));
    TEST_ASSERT_EQUAL_FLOAT(HY_CAB_THR_MAX, hy_cab_threshold(&m, 3));
    for (int k = 0; k < HY_CAB_TAKES; k++) TEST_ASSERT_TRUE(m.take[k].n > 0);
    m.valid = true;
    TEST_ASSERT_TRUE(hy_cab_match(&m, 3, &takes[0], NULL));
}

/* Three doors, one much smaller: the outlier is reported, and the model is filled so the teach flow can accept
   the takes as they are. */
static void test_model_odd_take_accept(void)
{
    static hy_cab_seq_t takes[HY_CAB_TAKES];
    static hy_cab_model_t m;
    static const float amp[HY_CAB_TAKES] = { 1.0f, 1.05f, 0.55f };
    for (int k = 0; k < HY_CAB_TAKES; k++) {
        cab_syn_t p = mv(CAB_SYN_DOOR_OPEN, amp[k], 1.0f, 31u + (uint32_t)k);
        capture(&p, HY_CAB_TEACH_TRIG_MG, 0, &takes[k]);
    }
    int odd = -1;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_cab_model_build(&m, takes, &odd));
    TEST_ASSERT_EQUAL(2, odd);
    TEST_ASSERT_FALSE(m.valid);
    TEST_ASSERT_TRUE(m.trig_mg >= HY_CAB_TRIG_MIN_MG && m.trig_mg <= HY_CAB_TRIG_MAX_MG);
    /* d_self is the worst-take rule over all three trimmed takes. */
    float worst = 0.0f;
    for (int k = 0; k < HY_CAB_TAKES; k++) {
        TEST_ASSERT_TRUE(m.take[k].n > 0 && m.take[k].n <= takes[k].n);
        /* trimmed to the first sample of the take at which the trigger fires */
        uint16_t cut = (uint16_t)(takes[k].n - m.take[k].n);
        TEST_ASSERT_TRUE(hy_cab_wom_hit_at(&takes[k], cut, m.trig_mg));
        for (uint16_t i = 0; i < cut; i++) TEST_ASSERT_FALSE(hy_cab_wom_hit_at(&takes[k], i, m.trig_mg));
        float best = INFINITY;
        for (int j = 0; j < HY_CAB_TAKES; j++)
            if (j != k) best = fminf(best, hy_cab_dist(&m.take[j], &m.take[k]));
        if (best > worst) worst = best;
    }
    TEST_ASSERT_TRUE(isfinite(m.d_self) && m.d_self > 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(worst, m.d_self);
    m.valid = true;
    for (int k = 0; k < HY_CAB_TAKES; k++) TEST_ASSERT_TRUE(hy_cab_match(&m, 3, &m.take[k], NULL));
}

static void test_threshold_clamp(void)
{
    static hy_cab_seq_t takes[HY_CAB_TAKES];
    static hy_cab_model_t m;
    cab_syn_t p = mv(CAB_SYN_DOOR_OPEN, 1.0f, 1.0f, 3);
    capture(&p, HY_CAB_TEACH_TRIG_MG, 0, &takes[0]);
    takes[1] = takes[0];
    takes[2] = takes[0];
    int odd = 99;
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_model_build(&m, takes, &odd));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, m.d_self);
    for (uint8_t s = 0; s <= 6; s++) TEST_ASSERT_EQUAL_FLOAT(HY_CAB_THR_MIN, hy_cab_threshold(&m, s));
    m.d_self = 100.0f;
    TEST_ASSERT_EQUAL_FLOAT(HY_CAB_THR_MAX, hy_cab_threshold(&m, 1));
    m.d_self = 0.5f;
    TEST_ASSERT_EQUAL_FLOAT(0.8f, hy_cab_threshold(&m, 3));
    TEST_ASSERT_EQUAL_FLOAT(0.8f, hy_cab_threshold(&m, 9));
}

static void test_model_trim_trigger(void)
{
    static hy_cab_seq_t takes[HY_CAB_TAKES], orig;
    static hy_cab_model_t m;
    teach(CAB_SYN_DOOR_OPEN, takes);
    /* Take 0 gets a 300 ms slow lead-in: an accel drift growing to 30 mg, under any trigger level. */
    const uint16_t lead = 300 / CAB_SYN_STEP_MS;
    orig = takes[0];
    orig.n = (uint16_t)(takes[0].n + lead);
    TEST_ASSERT_TRUE(orig.n <= HY_CAB_MAX_N);
    for (uint16_t i = 0; i < lead; i++) {
        hy_imu6_t s = { { orig.rest.x, orig.rest.y, orig.rest.z }, { 0, 0, 0 } };
        s.a[1] = (int16_t)(orig.rest.y - (int16_t)(30 * (i + 1) / lead));
        orig.s[i] = s;
    }
    memcpy(&orig.s[lead], takes[0].s, takes[0].n * sizeof takes[0].s[0]);
    takes[0] = orig;
    int odd = 99;
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_model_build(&m, takes, &odd));
    TEST_ASSERT_TRUE(m.trig_mg >= HY_CAB_TRIG_MIN_MG && m.trig_mg <= HY_CAB_TRIG_MAX_MG);
    uint16_t k = 0;
    while (k < orig.n && !hy_cab_wom_hit_at(&orig, k, m.trig_mg)) k++;
    TEST_ASSERT_TRUE(k >= lead);
    TEST_ASSERT_EQUAL_UINT16(orig.n - k, m.take[0].n);
    TEST_ASSERT_EQUAL_MEMORY(&orig.s[k], &m.take[0].s[0], sizeof orig.s[0]);
    TEST_ASSERT_TRUE(k < orig.n);
}

/* Host time per hy_cab_match with 200-sample takes and capture. */
static void test_match_timing(void)
{
    static hy_cab_seq_t takes[HY_CAB_TAKES], q;
    static hy_cab_model_t m;
    /* A 6 s tilt read from 1.8 s on, so every take starts above the trigger and keeps all 200 samples. */
    for (int k = 0; k < HY_CAB_TAKES; k++) {
        cab_syn_t p = cab_syn_new(CAB_SYN_TILT);
        p.dur = 6.0f;
        p.noise = true;
        p.seed = 40u + (uint32_t)k;
        memset(&takes[k], 0, sizeof takes[k]);
        takes[k].rest = p.rest;
        takes[k].has_gyro = true;
        takes[k].n = HY_CAB_MAX_N;
        for (uint32_t i = 0; i < HY_CAB_MAX_N; i++) takes[k].s[i] = cab_syn_sample(&p, i + 90);
    }
    q = takes[0];
    q.s[100].a[0] = (int16_t)(q.s[100].a[0] + 5);
    int odd;
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_model_build(&m, takes, &odd));
    TEST_ASSERT_EQUAL_UINT16(HY_CAB_MAX_N, m.take[0].n);
    const int reps = 50;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    float best = 0.0f;
    for (int r = 0; r < reps; r++) TEST_ASSERT_TRUE(hy_cab_match(&m, 3, &q, &best));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = ((double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3) / reps;
    printf("hy_cab_match, 3 takes x 200 samples, capture 200 samples: %.0f us per match (host, -O0)\n", us);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dist_identity);
    RUN_TEST(test_dist_time_warp);
    RUN_TEST(test_dist_open_end);
    RUN_TEST(test_dist_gate_energy);
    RUN_TEST(test_match_door_positive_negative);
    RUN_TEST(test_match_tilt_model);
    RUN_TEST(test_match_lift_model);
    RUN_TEST(test_match_rotated_device);
    RUN_TEST(test_sensitivity_monotone);
    RUN_TEST(test_model_odd_take);
    RUN_TEST(test_model_odd_take_accept);
    RUN_TEST(test_threshold_clamp);
    RUN_TEST(test_model_trim_trigger);
    RUN_TEST(test_match_timing);
    return UNITY_END();
}
