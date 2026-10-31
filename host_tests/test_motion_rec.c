#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "hy/motion.h"

#ifndef HY_TEST_DATA_DIR
#error HY_TEST_DATA_DIR must be defined by the Makefile
#endif

void setUp(void) {}
void tearDown(void) {}

typedef struct { uint32_t t; hy_accel_t a; } sample_t;
typedef struct { sample_t *s; int n; } rec_t;

/* Loads data/motion/<name>. Returns false when the file is absent. Fails the test on a malformed file. The times
   are rebased so that the first sample is at 0 ms: the recorded t_ms starts at an arbitrary boot time. */
static bool load(const char *name, rec_t *r)
{
    char path[512];
    snprintf(path, sizeof path, "%s/motion/%s", HY_TEST_DATA_DIR, name);
    r->s = NULL;
    r->n = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return false;   /* only a missing file is ignored */
        char msg[640];
        snprintf(msg, sizeof msg, "cannot open %s: %s", path, strerror(errno));
        TEST_FAIL_MESSAGE(msg);
    }
    char line[128];
    TEST_ASSERT_NOT_NULL_MESSAGE(fgets(line, sizeof line, f), name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("t_ms,x,y,z", strtok(line, "\r\n"), name);
    int cap = 0;
    uint32_t t0 = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '\r' || line[0] == '\n') continue;
        unsigned long t;
        int x, y, z;
        TEST_ASSERT_EQUAL_MESSAGE(4, sscanf(line, "%lu,%d,%d,%d", &t, &x, &y, &z), name);
        TEST_ASSERT_TRUE_MESSAGE(x >= INT16_MIN && x <= INT16_MAX && y >= INT16_MIN && y <= INT16_MAX &&
                                 z >= INT16_MIN && z <= INT16_MAX, "value outside int16 range");
        if (r->n > 0)
            TEST_ASSERT_TRUE_MESSAGE((uint32_t)t - t0 > r->s[r->n - 1].t, "t_ms not increasing");
        if (r->n == cap) {
            cap = cap ? cap * 2 : 1024;
            r->s = realloc(r->s, (size_t)cap * sizeof *r->s);
            TEST_ASSERT_NOT_NULL(r->s);
        }
        if (r->n == 0) t0 = (uint32_t)t;
        r->s[r->n++] = (sample_t){(uint32_t)t - t0, {(int16_t)x, (int16_t)y, (int16_t)z}};
    }
    fclose(f);
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, r->n, name);
    return true;
}

static void unload(rec_t *r)
{
    free(r->s);
    r->s = NULL;
}

#define LOAD_OR_IGNORE(name, r) \
    do { if (!load(name, &(r))) TEST_IGNORE_MESSAGE("missing data/motion/" name); } while (0)

/* Number of detections over the whole file, the samples scaled by num/den; the detector is reset at the first sample.
   Prints one margin line: shakes, the largest stroke count reached (a detection counts as HY_SHK_STROKES) and the
   largest stroke swing that took at most HY_SHK_STROKE_MS (a stroke is the change between two successive turning
   points on one axis; HY_SHK_SWING_MG is the threshold). Scaling the signal by k moves the swing threshold by 1/k,
   so a run at 100/115 shows the count with the threshold 15 % higher. */
static int shake_count_scaled(const char *name, const rec_t *r, int num, int den)
{
    hy_shake_t s;
    hy_shake_reset(&s, r->s[0].t);
    int n = 0, max_count = 0;
    int32_t max_swing = 0;
    for (int i = 0; i < r->n; i++) {
        hy_accel_t a;
        int32_t v[3] = {r->s[i].a.x, r->s[i].a.y, r->s[i].a.z};
        int16_t *w[3] = {&a.x, &a.y, &a.z};
        for (int k = 0; k < 3; k++) {
            int32_t sv = v[k] * num / den;
            TEST_ASSERT_TRUE_MESSAGE(sv >= INT16_MIN && sv <= INT16_MAX, "scaled value outside int16 range");
            *w[k] = (int16_t)sv;
        }
        bool had[3];
        int32_t pv[3];
        uint32_t pt[3];
        for (int k = 0; k < 3; k++) {
            had[k] = s.ax[k].have_prev;
            pv[k] = s.ax[k].prev_v;
            pt[k] = s.ax[k].prev_ms;
        }
        if (hy_shake_feed(&s, r->s[i].t, a)) {
            n++;
            max_count = HY_SHK_STROKES;
        }
        for (int k = 0; k < 3; k++) {
            if (s.ax[k].count > max_count) max_count = s.ax[k].count;
            if (had[k] && (s.ax[k].prev_ms != pt[k] || s.ax[k].prev_v != pv[k]) &&
                s.ax[k].prev_ms - pt[k] <= HY_SHK_STROKE_MS) {
                int32_t sw = s.ax[k].prev_v - pv[k];
                if (sw < 0) sw = -sw;
                if (sw > max_swing) max_swing = sw;
            }
        }
    }
    printf("motion rec %s x%d/%d: shakes %d, max count %d, max stroke %d mg\n", name, num, den, n,
           max_count, (int)max_swing);
    return n;
}

static int shake_count(const char *name, const rec_t *r)
{
    return shake_count_scaled(name, r, 1, 1);
}

/* Init orientation: the candidate that holds on every sample of the first second if there is one, else 0. */
static hy_rot_t init_rot(const rec_t *r)
{
    hy_rot_t first, c;
    if (!hy_orient_candidate(r->s[0].a, &first)) return HY_ROT_0;
    for (int i = 1; i < r->n && r->s[i].t - r->s[0].t < HY_OR_DWELL_MS; i++)
        if (!hy_orient_candidate(r->s[i].a, &c) || c != first) return HY_ROT_0;
    return first;
}

#define MAX_SW 64
typedef struct { int n; int idx[MAX_SW]; hy_rot_t rot[MAX_SW]; hy_rot_t init; } sw_t;

/* Feeds every sample; records the sample index and new orientation of every switch. */
static void orient_run(const rec_t *r, sw_t *w)
{
    hy_orient_t o;
    w->init = init_rot(r);
    w->n = 0;
    hy_orient_init(&o, w->init);
    for (int i = 0; i < r->n; i++) {
        if (hy_orient_feed(&o, r->s[i].t, r->s[i].a)) {
            TEST_ASSERT_LESS_THAN_MESSAGE(MAX_SW, w->n, "too many switches");
            w->idx[w->n] = i;
            w->rot[w->n] = hy_orient_current(&o);
            w->n++;
        }
    }
}

static void no_shake(const char *name, const rec_t *r)
{
    int n = shake_count(name, r);
    int n2 = shake_count_scaled(name, r, 2, 1);   /* margin: twice the motion is still not a shake */
    TEST_ASSERT_EQUAL_MESSAGE(0, n, name);
    TEST_ASSERT_EQUAL_MESSAGE(0, n2, name);
}

static void check_no_shake(const char *name)
{
    rec_t r;
    if (!load(name, &r)) TEST_IGNORE_MESSAGE(name);
    no_shake(name, &r);
    unload(&r);
}

static void check_no_switch(const char *name)
{
    rec_t r;
    sw_t w;
    if (!load(name, &r)) TEST_IGNORE_MESSAGE(name);
    no_shake(name, &r);
    orient_run(&r, &w);
    unload(&r);
    TEST_ASSERT_EQUAL_MESSAGE(0, w.n, name);
}

/* shake_firm holds three bursts and detects 2: the recording starts inside the first shake, so burst 1 is a fragment
   of 3 strokes at t = 0, which is below HY_SHK_STROKES (4). The two full bursts give one detection each. */
static void test_shake_firm(void)
{
    rec_t r;
    LOAD_OR_IGNORE("shake_firm.csv", r);
    int n = shake_count("shake_firm.csv", &r);
    int n_weak = shake_count_scaled("shake_firm.csv", &r, 100, 115);   /* threshold 15 % higher */
    unload(&r);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(2, n_weak);
}

/* A live recording made with the detector armed: two bursts (about 3.2 to 4.7 s and 6.8 to 8.7 s) and a 0.9 g
   bump at 10.8 s. The detector built before this one fired twice, both times in the second burst. */
static void test_shake_live_combo(void)
{
    rec_t r;
    LOAD_OR_IGNORE("shake_live_combo.csv", r);
    int n = shake_count("shake_live_combo.csv", &r);
    int n_weak = shake_count_scaled("shake_live_combo.csv", &r, 100, 115);
    unload(&r);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(2, n_weak);
}

/* A recording of the board lying still for 30 s (every axis stays within 46 mg), made about 40 s before the detector
   was armed (see data/motion/README.md): it holds no shake. */
static void test_shake_live_arm(void)
{
    rec_t r;
    LOAD_OR_IGNORE("shake_live_arm.csv", r);
    int n = shake_count("shake_live_arm.csv", &r);
    unload(&r);
    TEST_ASSERT_EQUAL(0, n);
}

static void test_shake_light(void)
{
    rec_t r;
    LOAD_OR_IGNORE("shake_light.csv", r);
    (void)shake_count("shake_light.csv", &r);   /* informative: the margin line is the output */
    unload(&r);
}

static void test_tap_desk(void)     { check_no_shake("tap_desk.csv"); }
static void test_tap_held(void)     { check_no_shake("tap_held.csv"); }
static void test_tap_double(void)   { check_no_shake("tap_double.csv"); }
static void test_carry_walk(void)   { check_no_shake("carry_walk.csv"); }
static void test_putdown_pickup(void) { check_no_shake("putdown_pickup.csv"); }
static void test_table_knock(void)  { check_no_shake("table_knock.csv"); }

static void test_rotate_cycle(void)
{
    /* The recording turns the board left edge up, bottom edge up, right edge up, top edge up (accel x -962 at the
       first switch: -x up is 270). */
    static const hy_rot_t want[] = {HY_ROT_270, HY_ROT_180, HY_ROT_90, HY_ROT_0};
    rec_t r;
    sw_t w;
    LOAD_OR_IGNORE("rotate_cycle.csv", r);
    no_shake("rotate_cycle.csv", &r);
    orient_run(&r, &w);
    TEST_ASSERT_EQUAL(4, w.n);
    hy_rot_t prev = w.init;
    for (int k = 0; k < w.n; k++) {
        TEST_ASSERT_EQUAL(want[k], w.rot[k]);
        /* Last sample before the switch that pointed to the previous orientation's candidate or to none. */
        int j = w.idx[k] - 1;
        for (; j >= 0; j--) {
            hy_rot_t c;
            if (!hy_orient_candidate(r.s[j].a, &c) || c == prev) break;
        }
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, j);
        uint32_t dt = r.s[w.idx[k]].t - r.s[j].t;
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1000, dt);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(1300, dt);
        prev = w.rot[k];
    }
    unload(&r);
}

/* Wobbling around the diagonal between two edges: three switches, 270, 0 and 270 degrees. Each one is a real
   tilt under the rule: the pose sat inside the 30 degree cone of the new edge for HY_OR_DWELL_MS without a sample
   leaving it (the 0 switch at the rim of the cone, 27 to 30 degrees, the other two 8 to 29 degrees from their
   edge). Times are from the first sample. */
static void test_tilt_diagonal_real_tilts(void)
{
    static const hy_rot_t want[] = {HY_ROT_270, HY_ROT_0, HY_ROT_270};
    static const uint32_t at_ms[] = {14680, 17340, 19280};   /* recorded; checked within +-100 ms */
    rec_t r;
    sw_t w;
    LOAD_OR_IGNORE("tilt_diagonal.csv", r);
    no_shake("tilt_diagonal.csv", &r);
    orient_run(&r, &w);
    TEST_ASSERT_EQUAL(HY_ROT_0, w.init);
    TEST_ASSERT_EQUAL(3, w.n);
    for (int k = 0; k < w.n; k++) {
        TEST_ASSERT_EQUAL(want[k], w.rot[k]);
        TEST_ASSERT_UINT32_WITHIN(100, at_ms[k], r.s[w.idx[k]].t);
        /* the candidate held on every sample of the dwell before the switch */
        int j = w.idx[k];
        for (hy_rot_t c; j > 0 && hy_orient_candidate(r.s[j - 1].a, &c) && c == w.rot[k]; j--) {}
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(HY_OR_DWELL_MS, r.s[w.idx[k]].t - r.s[j].t);
    }
    unload(&r);
}

static void test_flat_spin(void)     { check_no_switch("flat_spin.csv"); }

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_shake_firm);
    RUN_TEST(test_shake_live_combo);
    RUN_TEST(test_shake_live_arm);
    RUN_TEST(test_shake_light);
    RUN_TEST(test_tap_desk);
    RUN_TEST(test_tap_held);
    RUN_TEST(test_tap_double);
    RUN_TEST(test_carry_walk);
    RUN_TEST(test_putdown_pickup);
    RUN_TEST(test_table_knock);
    RUN_TEST(test_rotate_cycle);
    RUN_TEST(test_tilt_diagonal_real_tilts);
    RUN_TEST(test_flat_spin);
    return UNITY_END();
}
