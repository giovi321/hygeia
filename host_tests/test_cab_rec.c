/* Replays the recorded motion files (data/motion, accel only) and the owner's sample downloads (data/cabinet)
   through the cabinet open pipeline: the still tracker, the wake-on-motion trigger, the capture segmenter and the
   matcher, as the device runs them on its 50 Hz stream. Prints the trigger table and the matcher margins. */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "hy/cabinet.h"
#include "hy/motion.h"
#include "cab_synth.h"

#ifndef HY_TEST_DATA_DIR
#error HY_TEST_DATA_DIR must be defined by the Makefile
#endif

void setUp(void) {}
void tearDown(void) {}

#define STEP_MS     CAB_SYN_STEP_MS
#define REC_MAX     4000                 /* samples per file: 80 s */
#define CAPS_MAX    48
#define WIN_N       (3 * HY_CAB_RATE_HZ)  /* negative windows: 3 s every 1 s */
#define WIN_STEP    HY_CAB_RATE_HZ
#define N_AUG       5
#define STILL_HALF  HY_CAB_RATE_HZ       /* a still part: every axis within HY_CAB_QUIET_MG over +-1 s */

static const char *const k_files[] = { "carry_walk", "flat_spin", "putdown_pickup", "rotate_cycle", "shake_firm",
                                       "shake_light", "shake_live_arm", "shake_live_combo", "table_knock", "tap_desk",
                                       "tap_double", "tap_held", "tilt_diagonal" };
#define N_FILES (sizeof k_files / sizeof k_files[0])

/* ---- Reader ---- */

typedef struct { hy_imu6_t s[REC_MAX]; uint32_t n; bool has_gyro; } rec_t;

static FILE *open_or_fail(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f && errno != ENOENT) {
        char msg[640];
        snprintf(msg, sizeof msg, "cannot open %s: %s", path, strerror(errno));
        TEST_FAIL_MESSAGE(msg);
    }
    return f;
}

static bool in_i16(long v)
{
    return v >= INT16_MIN && v <= INT16_MAX;
}

/* A 4-column motion file data/motion/<name>.csv (t_ms,x,y,z): one continuous stream, has_gyro false. False when
   the file is absent. */
static bool load_motion(const char *name, rec_t *r)
{
    char path[512];
    snprintf(path, sizeof path, "%s/motion/%s.csv", HY_TEST_DATA_DIR, name);
    FILE *f = open_or_fail(path);
    if (!f) return false;
    char line[160];
    TEST_ASSERT_NOT_NULL_MESSAGE(fgets(line, sizeof line, f), name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("t_ms,x,y,z", strtok(line, "\r\n"), name);
    r->n = 0;
    r->has_gyro = false;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '\r' || line[0] == '\n') continue;
        unsigned long t;
        long x, y, z;
        TEST_ASSERT_EQUAL_MESSAGE(4, sscanf(line, "%lu,%ld,%ld,%ld", &t, &x, &y, &z), name);
        TEST_ASSERT_TRUE_MESSAGE(in_i16(x) && in_i16(y) && in_i16(z), name);
        TEST_ASSERT_TRUE_MESSAGE(r->n < REC_MAX, name);
        r->s[r->n++] = (hy_imu6_t){ { (int16_t)x, (int16_t)y, (int16_t)z }, { 0, 0, 0 } };
    }
    fclose(f);
    TEST_ASSERT_TRUE_MESSAGE(r->n > 0, name);
    return true;
}

typedef enum { DL_TEACH, DL_CAPTURE } dl_kind_t;   /* capture: kind test or live */

/* A sample download (kind,take,t_ms,ax,ay,az,gx,gy,gz): the sequences of the wanted kind, in file order, one per
   (kind, take). An optional row of kind rest_<kind> (rest_teach, rest_test or rest_live: rest_live,take,0,ax,ay,az,
   0,0,0) gives the rest vector of that one sequence; without it the rest is the last sample of the sequence, the
   point at which the segmenter found it still. has_gyro is true when any gyro value in the file is not 0. Returns the number of sequences. */
static int load_download(const char *path, dl_kind_t want, hy_cab_seq_t *out, int max)
{
    FILE *f = open_or_fail(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    char line[200];
    TEST_ASSERT_NOT_NULL_MESSAGE(fgets(line, sizeof line, f), path);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("kind,take,t_ms,ax,ay,az,gx,gy,gz", strtok(line, "\r\n"), path);
    int n = 0, take_of[64];
    char kind_of[64][16];
    bool rest_of[64], gyro = false;
    hy_accel_t rests[64];
    int n_rest = 0, rest_take[64];
    char rest_kind[64][16];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '\r' || line[0] == '\n') continue;
        char kind[16];
        int take;
        unsigned long t;
        long v[6];
        TEST_ASSERT_EQUAL_MESSAGE(9, sscanf(line, "%15[^,],%d,%lu,%ld,%ld,%ld,%ld,%ld,%ld", kind, &take, &t, &v[0], &v[1],
                                            &v[2], &v[3], &v[4], &v[5]), path);
        for (int k = 0; k < 6; k++) TEST_ASSERT_TRUE_MESSAGE(in_i16(v[k]), path);
        if (strncmp(kind, "rest_", 5) == 0) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(kind + 5, "teach") == 0 || strcmp(kind + 5, "test") == 0 ||
                                     strcmp(kind + 5, "live") == 0, path);
            TEST_ASSERT_TRUE(n_rest < 64);
            snprintf(rest_kind[n_rest], sizeof rest_kind[n_rest], "%s", kind + 5);
            rest_take[n_rest] = take;
            rests[n_rest++] = (hy_accel_t){ (int16_t)v[0], (int16_t)v[1], (int16_t)v[2] };
            continue;
        }
        bool teach = strcmp(kind, "teach") == 0;
        TEST_ASSERT_TRUE_MESSAGE(teach || strcmp(kind, "test") == 0 || strcmp(kind, "live") == 0, path);
        if (teach != (want == DL_TEACH)) continue;
        if (v[3] || v[4] || v[5]) gyro = true;
        if (n == 0 || take_of[n - 1] != take || strcmp(kind_of[n - 1], kind) != 0) {
            TEST_ASSERT_TRUE_MESSAGE(n < max && n < 64, path);
            memset(&out[n], 0, sizeof out[n]);
            take_of[n] = take;
            snprintf(kind_of[n], sizeof kind_of[n], "%s", kind);
            rest_of[n] = false;
            n++;
        }
        hy_cab_seq_t *q = &out[n - 1];
        TEST_ASSERT_TRUE_MESSAGE(q->n < HY_CAB_MAX_N, path);
        q->s[q->n++] = (hy_imu6_t){ { (int16_t)v[0], (int16_t)v[1], (int16_t)v[2] },
                                    { (int16_t)v[3], (int16_t)v[4], (int16_t)v[5] } };
    }
    fclose(f);
    for (int i = 0; i < n; i++) {
        out[i].has_gyro = gyro;
        for (int k = 0; k < n_rest; k++)
            if (rest_take[k] == take_of[i] && strcmp(rest_kind[k], kind_of[i]) == 0) {
                out[i].rest = rests[k];
                rest_of[i] = true;
            }
        if (!rest_of[i]) {
            const hy_imu6_t *e = &out[i].s[out[i].n - 1];
            out[i].rest = (hy_accel_t){ e->a[0], e->a[1], e->a[2] };
        }
    }
    return n;
}

/* ---- Pipeline ---- */

static hy_accel_t acc(const hy_imu6_t *s)
{
    return (hy_accel_t){ s->a[0], s->a[1], s->a[2] };
}

/* The trigger at stream sample i against the sensor's previous sample; the stream before sample 0 reads s[0]. */
static bool stream_hit(const hy_imu6_t *s, uint32_t i, uint16_t thr)
{
    hy_accel_t b2 = acc(&s[i >= 2 ? i - 2 : 0]), b3 = acc(&s[i >= 3 ? i - 3 : 0]);
    return hy_cab_wom_hit(hy_cab_wom_prev(b2, b3), acc(&s[i]), thr);
}

/* The largest per-axis slope the trigger sees at sample i: the smallest threshold at which it does not fire. */
static uint16_t stream_slope(const hy_imu6_t *s, uint32_t i)
{
    hy_accel_t p = hy_cab_wom_prev(acc(&s[i >= 2 ? i - 2 : 0]), acc(&s[i >= 3 ? i - 3 : 0]));
    int32_t d[3] = { s[i].a[0] - p.x, s[i].a[1] - p.y, s[i].a[2] - p.z }, m = 0;
    for (int k = 0; k < 3; k++) {
        if (d[k] < 0) d[k] = -d[k];
        if (d[k] > m) m = d[k];
    }
    return (uint16_t)m;
}

/* Any movement: triggers from sample from to sample to (exclusive), the trigger disarmed for
   HY_CAB_ANY_HOLDOFF_MS after each. */
static int any_count(const hy_imu6_t *s, uint32_t from, uint32_t to, uint16_t thr)
{
    int c = 0;
    uint32_t armed_at = from;
    for (uint32_t i = from; i < to; i++) {
        if (i < armed_at || !stream_hit(s, i, thr)) continue;
        c++;
        armed_at = i + HY_CAB_ANY_HOLDOFF_MS / STEP_MS;
    }
    return c;
}

typedef struct { hy_cab_seq_t seq; uint32_t trig_i, end_i; } cap_t;

static int s_results[4];   /* hy_cap_res_t counts of the last replay */

/* Taught movement on the stream: the trigger is armed once the still tracker reports HY_CAB_STILL_MS of rest and
   stays armed until it fires, with the tracker's latest rest; a hit at thr starts a capture pre samples before the
   hit sample (an earlier start, as when the trigger fires late); after the capture, whatever its result, the
   tracker starts again. Returns the number of captures the segmenter finished as DONE (up to max); too short and
   too long ones are dropped, as on the device. */
static int replay(const hy_imu6_t *s, uint32_t n, bool has_gyro, uint16_t thr, uint32_t pre, cap_t *out, int max)
{
    static hy_cab_still_t st;
    static hy_cab_cap_t c;
    hy_cab_still_reset(&st);
    hy_accel_t rest = { 0, 0, 0 };
    bool armed = false;
    int got = 0;
    memset(s_results, 0, sizeof s_results);
    for (uint32_t i = 0; i < n; i++) {
        if (armed && stream_hit(s, i, thr)) {
            uint32_t j = i >= pre ? i - pre : 0;
            hy_cab_cap_start(&c, rest, has_gyro, j * STEP_MS);
            hy_cap_res_t r = HY_CAP_MORE;
            for (; j < n && r == HY_CAP_MORE; j++) r = hy_cab_cap_feed(&c, j * STEP_MS, &s[j]);
            s_results[r]++;
            if (r == HY_CAP_DONE && got < max) {
                out[got].seq = c.seq;
                out[got].trig_i = i;
                out[got].end_i = j;
                got++;
            }
            i = j;
            hy_cab_still_reset(&st);
            armed = false;
            if (i >= n) break;
        }
        hy_accel_t r;
        if (hy_cab_still_feed(&st, i * STEP_MS, acc(&s[i]), &r)) {
            rest = r;
            armed = true;
        }
    }
    return got;
}

/* Window k of a stream: 3 s from sample k * 1 s, rest from the still tracker's last rest before it (the first
   sample of the window when the stream was never still before). False past the end. */
static bool window(const rec_t *r, uint32_t k, hy_cab_seq_t *out)
{
    uint32_t from = k * WIN_STEP;
    if (from + WIN_N > r->n) return false;
    static hy_cab_still_t st;
    hy_cab_still_reset(&st);
    hy_accel_t rest = acc(&r->s[from]), cur;
    for (uint32_t i = 0; i < from; i++)
        if (hy_cab_still_feed(&st, i * STEP_MS, acc(&r->s[i]), &cur)) rest = cur;
    memset(out, 0, sizeof *out);
    out->rest = rest;
    out->has_gyro = r->has_gyro;
    out->n = WIN_N;
    memcpy(out->s, &r->s[from], WIN_N * sizeof out->s[0]);
    return true;
}

/* ---- Augmentation ---- */

static float unit_rand(uint32_t seed, uint32_t ch)
{
    return (float)(0.5 * (cab_syn_rand(seed, 0, ch) + 1.0));   /* 0 to 1 */
}

/* A variant of the movement of capture c in stream s: the stream from 1.6 s before the trigger to 0.8 s after
   the capture's end, slowed or sped up by 0.85 to 1.15 (linear resampling), its deviation from the rest scaled by
   0.9 to 1.1, with a seeded noise of +-10 mg per axis from 200 ms before the trigger on (on the rest before, the
   added noise would keep the still tracker from arming); then replayed with the capture starting 0 to 8 samples
   (160 ms) before the trigger at thr. False when the trigger does not fire or the segmenter drops the capture. */
static bool augment(const hy_imu6_t *s, uint32_t n, const cap_t *c, uint32_t seed, uint16_t thr, hy_cab_seq_t *out)
{
    static hy_imu6_t seg[REC_MAX];
    static cap_t got[2];
    const float f = 0.85f + 0.30f * unit_rand(seed, 0), amp = 0.9f + 0.2f * unit_rand(seed, 1);
    const uint32_t pre = (uint32_t)lroundf(8.0f * unit_rand(seed, 2));
    const uint32_t from = c->trig_i >= 80 ? c->trig_i - 80 : 0;
    const uint32_t to = c->end_i + 40 < n ? c->end_i + 40 : n;
    const hy_accel_t rest = c->seq.rest;
    uint32_t m = (uint32_t)lroundf((float)(to - from - 1) * f) + 1;
    if (m > REC_MAX) m = REC_MAX;
    for (uint32_t j = 0; j < m; j++) {
        float pos = (float)j / f;
        uint32_t i0 = (uint32_t)pos;
        if (from + i0 + 1 >= to) i0 = to - from - 2;
        float w = pos - (float)i0;
        if (w > 1.0f) w = 1.0f;
        const hy_imu6_t *a = &s[from + i0], *b = &s[from + i0 + 1];
        const int16_t r3[3] = { rest.x, rest.y, rest.z };
        for (int k = 0; k < 3; k++) {
            float v = (float)a->a[k] + w * (float)(b->a[k] - a->a[k]);
            v = (float)r3[k] + amp * (v - (float)r3[k]);
            if (from + i0 + 10 >= c->trig_i) v += 10.0f * (float)cab_syn_rand(seed, j + 1, (uint32_t)k + 3);
            seg[j].a[k] = cab_syn_i16(v);
            seg[j].g[k] = cab_syn_i16((float)a->g[k] + w * (float)(b->g[k] - a->g[k]));
        }
    }
    if (replay(seg, m, c->seq.has_gyro, thr, pre, got, 1) == 0) return false;
    *out = got[0].seq;
    return true;
}

/* ---- Recordings, captures and models (built once, shared by the tests) ---- */

static rec_t s_rec[N_FILES];
static bool s_have[N_FILES], s_loaded;

static void load_all(void)
{
    if (s_loaded) return;
    for (unsigned f = 0; f < N_FILES; f++) s_have[f] = load_motion(k_files[f], &s_rec[f]);
    s_loaded = true;
}

static int file_index(const char *name)
{
    for (unsigned f = 0; f < N_FILES; f++)
        if (strcmp(k_files[f], name) == 0) return (int)f;
    TEST_FAIL_MESSAGE(name);
    return -1;
}

#define NEED(name)                                                                       \
    do {                                                                                 \
        load_all();                                                                      \
        if (!s_have[file_index(name)]) TEST_IGNORE_MESSAGE("missing data/motion/" name); \
    } while (0)

/* The person waits for the device between takes. The recordings leave some rests shorter than the device needs to
   re-arm (the segmenter's quiet tail plus HY_CAB_STILL_MS of the still tracker), so a take there would be lost.
   The teach replay therefore inserts PAD_N samples of the recording's own rest (its last 10 samples, played
   back and forth so every step is one the recording made, ending forward on the sample before) before every sample at which the teach trigger fires after at least PAD_AFTER samples with every step under
   HY_CAB_STILL_STEP_MG; the slope the trigger sees at that sample is unchanged. Live captures and windows use the
   recordings as they are. */
#define PAD_N     (((HY_CAB_STILL_MS + 200) / STEP_MS + 9) / 10 * 10)
#define PAD_AFTER 20   /* 400 ms */

static void pad_rests(const rec_t *in, rec_t *out, int *pads)
{
    uint32_t run = 0;
    *pads = 0;
    out->n = 0;
    out->has_gyro = in->has_gyro;
    for (uint32_t i = 0; i < in->n; i++) {
        if (run >= PAD_AFTER && stream_hit(in->s, i, HY_CAB_TEACH_TRIG_MG)) {
            TEST_ASSERT_TRUE(out->n + PAD_N < REC_MAX);
            for (uint32_t j = 0; j < PAD_N; j++) {
                uint32_t b = j / 10, o = j % 10;
                bool fwd = (PAD_N / 10 - 1 - b) % 2 == 0;
                out->s[out->n++] = in->s[i - 10 + (fwd ? o : 9 - o)];
            }
            (*pads)++;
        }
        bool step = false;
        if (i > 0)
            for (int k = 0; k < 3; k++) {
                int32_t d = (int32_t)in->s[i].a[k] - in->s[i - 1].a[k];
                if (d >= HY_CAB_STILL_STEP_MG || -d >= HY_CAB_STILL_STEP_MG) step = true;
            }
        run = step || i == 0 ? 0 : run + 1;
        TEST_ASSERT_TRUE(out->n < REC_MAX);
        out->s[out->n++] = in->s[i];
    }
}

typedef struct {
    const char *file;
    bool built;
    rec_t teach;            /* the file with the rests padded */
    hy_cab_model_t m;
    cap_t caps[CAPS_MAX];   /* the file's captures at the teach trigger */
    int n_caps;
    float pos_max, neg_min;   /* margins: the largest positive distance, the smallest negative distance */
    char neg_at[48];
} model_t;

static model_t s_models[3] = { { .file = "putdown_pickup" }, { .file = "rotate_cycle" }, { .file = "tap_double" } };

static model_t *model_of(const char *file)
{
    for (unsigned i = 0; i < sizeof s_models / sizeof s_models[0]; i++)
        if (strcmp(s_models[i].file, file) == 0) return &s_models[i];
    return NULL;
}

static void print_caps(const model_t *md)
{
    printf("%s: %d captures at the teach trigger (%u mg), at (padded stream time, length):", md->file, md->n_caps,
           (unsigned)HY_CAB_TEACH_TRIG_MG);
    for (int k = 0; k < md->n_caps; k++)
        printf(" %.2f s (%u ms)", md->caps[k].trig_i * STEP_MS / 1000.0, (unsigned)md->caps[k].seq.n * STEP_MS);
    printf("\n");
}

/* The file's captures at the teach trigger; with 3 or more, the model from the first 3, accepted as is when a take
   is odd (the teach flow's last redo). False when the file is absent or has fewer than 3 captures. */
static bool build_model(model_t *md)
{
    if (md->built) return md->m.valid;
    md->built = true;
    const rec_t *in = &s_rec[file_index(md->file)];
    static cap_t plain[CAPS_MAX];
    int n_plain = replay(in->s, in->n, in->has_gyro, HY_CAB_TEACH_TRIG_MG, 0, plain, CAPS_MAX), pads;
    pad_rests(in, &md->teach, &pads);
    const rec_t *r = &md->teach;
    md->n_caps = replay(r->s, r->n, r->has_gyro, HY_CAB_TEACH_TRIG_MG, 0, md->caps, CAPS_MAX);
    printf("%s: %d captures in the recording as it is; %d rests padded: %d done, %d too short, %d too long, %d "
           "unfinished at the end\n",
           md->file, n_plain, pads, s_results[HY_CAP_DONE], s_results[HY_CAP_TOO_SHORT], s_results[HY_CAP_TOO_LONG],
           s_results[HY_CAP_MORE]);
    print_caps(md);
    if (md->n_caps < HY_CAB_TAKES) return false;
    static hy_cab_seq_t takes[HY_CAB_TAKES];
    for (int k = 0; k < HY_CAB_TAKES; k++) takes[k] = md->caps[k].seq;
    int odd = -1;
    hy_err_t e = hy_cab_model_build(&md->m, takes, &odd);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(md->m.d_self), md->file);
    if (e != HY_OK) md->m.valid = true;
    printf("%s model: trigger %u mg, d_self %.3f, threshold at 3 %.3f, odd take %d; each take's smallest distance to "
           "the others:", md->file, (unsigned)md->m.trig_mg, md->m.d_self, hy_cab_threshold(&md->m, 3), odd);
    for (int k = 0; k < HY_CAB_TAKES; k++) {
        float b = INFINITY;
        for (int j = 0; j < HY_CAB_TAKES; j++)
            if (j != k) b = fminf(b, hy_cab_dist(&md->m.take[j], &md->m.take[k]));
        printf(" %.3f", b);
    }
    printf("\n");
    md->pos_max = 0.0f;
    md->neg_min = INFINITY;
    return true;
}

/* Every live capture of the file at the model's trigger and every window of the file: no match. */
static void expect_no_match_in(model_t *md, unsigned f)
{
    if (!s_have[f]) return;
    const rec_t *r = &s_rec[f];
    static cap_t caps[CAPS_MAX];
    static hy_cab_seq_t q;
    int n = replay(r->s, r->n, r->has_gyro, md->m.trig_mg, 0, caps, CAPS_MAX);
    char msg[160];
    for (int k = 0; k < n; k++) {
        float d;
        bool hit = hy_cab_match(&md->m, 3, &caps[k].seq, &d);
        if (d < md->neg_min) {
            md->neg_min = d;
            snprintf(md->neg_at, sizeof md->neg_at, "%s capture at %.2f s", k_files[f], caps[k].trig_i * 0.02);
        }
        snprintf(msg, sizeof msg, "%s model, %s capture at %.2f s: %.3f", md->file, k_files[f], caps[k].trig_i * 0.02, d);
        TEST_ASSERT_FALSE_MESSAGE(hit, msg);
    }
    for (uint32_t w = 0; window(r, w, &q); w++) {
        float d;
        bool hit = hy_cab_match(&md->m, 3, &q, &d);
        if (d < md->neg_min) {
            md->neg_min = d;
            snprintf(md->neg_at, sizeof md->neg_at, "%s window at %u s", k_files[f], (unsigned)w);
        }
        snprintf(msg, sizeof msg, "%s model, %s window at %u s: %.3f", md->file, k_files[f], (unsigned)w, d);
        TEST_ASSERT_FALSE_MESSAGE(hit, msg);
    }
}

static void expect_no_match_others(model_t *md)
{
    for (unsigned f = 0; f < N_FILES; f++)
        if (strcmp(k_files[f], md->file) != 0) expect_no_match_in(md, f);
}

/* Capture k of the model's file and N_AUG variants of it: each matches at sensitivity 3. */
static void expect_match_variants(model_t *md, int k)
{
    const rec_t *r = &md->teach;
    static hy_cab_seq_t q;
    char msg[160];
    for (uint32_t a = 0; a <= N_AUG; a++) {
        float d;
        if (a == 0) {
            /* the capture itself, as the live trigger at the model's level would cut it */
            static cap_t one[CAPS_MAX];
            int n = replay(r->s, r->n, r->has_gyro, md->m.trig_mg, 0, one, CAPS_MAX);
            bool found = false;
            for (int i = 0; i < n && !found; i++)
                if (one[i].trig_i >= md->caps[k].trig_i && one[i].trig_i < md->caps[k].end_i) {
                    q = one[i].seq;
                    found = true;
                }
            snprintf(msg, sizeof msg, "%s capture %d not captured at %u mg", md->file, k, (unsigned)md->m.trig_mg);
            TEST_ASSERT_TRUE_MESSAGE(found, msg);
        } else {
            snprintf(msg, sizeof msg, "%s capture %d variant %u not captured", md->file, k, (unsigned)a);
            TEST_ASSERT_TRUE_MESSAGE(augment(r->s, r->n, &md->caps[k], 7919u * (uint32_t)k + a, md->m.trig_mg, &q), msg);
        }
        bool hit = hy_cab_match(&md->m, 3, &q, &d);
        if (d > md->pos_max) md->pos_max = d;
        snprintf(msg, sizeof msg, "%s capture %d variant %u: %.3f over %.3f", md->file, k, (unsigned)a, d,
                 hy_cab_threshold(&md->m, 3));
        TEST_ASSERT_TRUE_MESSAGE(hit, msg);
    }
}

static void print_margin(const model_t *md)
{
    printf("%s model margin: largest positive (variants of the takes) %.3f, d_self %.3f, threshold %.3f, smallest "
           "negative %.3f (%s)\n", md->file, md->pos_max, md->m.d_self, hy_cab_threshold(&md->m, 3), md->neg_min,
           md->neg_at);
}

/* ---- Any movement ---- */

/* True when sample i lies in a still part: every axis within HY_CAB_QUIET_MG over the second before and after. */
static bool still_part(const rec_t *r, uint32_t i)
{
    if (i < STILL_HALF || i + STILL_HALF >= r->n) return false;
    for (int k = 0; k < 3; k++) {
        int16_t lo = INT16_MAX, hi = INT16_MIN;
        for (uint32_t j = i - STILL_HALF; j <= i + STILL_HALF; j++) {
            if (r->s[j].a[k] < lo) lo = r->s[j].a[k];
            if (r->s[j].a[k] > hi) hi = r->s[j].a[k];
        }
        if (hi - lo > HY_CAB_QUIET_MG) return false;
    }
    return true;
}

static void test_any_counts_per_file(void)
{
    load_all();
    int counts[N_FILES][5];
    printf("Any movement triggers per file (holdoff %u ms); thresholds", (unsigned)HY_CAB_ANY_HOLDOFF_MS);
    for (uint8_t s = 1; s <= 5; s++) printf(" %u", (unsigned)hy_cab_wom_mg(s));
    printf(" mg\n%-18s %6s %6s %6s %6s %6s %6s %6s\n", "file", "sens 1", "2", "3", "4", "5", "peak", "still");
    for (unsigned f = 0; f < N_FILES; f++) {
        if (!s_have[f]) continue;
        const rec_t *r = &s_rec[f];
        uint16_t peak = 0, still = 0;
        for (uint32_t i = 0; i < r->n; i++) {
            uint16_t sl = stream_slope(r->s, i);
            if (sl > peak) peak = sl;
            if (still_part(r, i) && sl > still) still = sl;
        }
        printf("%-18s", k_files[f]);
        for (uint8_t s = 1; s <= 5; s++) {
            counts[f][s - 1] = any_count(r->s, 0, r->n, hy_cab_wom_mg(s));
            printf(" %6d", counts[f][s - 1]);
        }
        printf(" %6u %6u\n", (unsigned)peak, (unsigned)still);
        /* The most sensitive step stays above the noise of a device at rest. */
        char msg[96];
        snprintf(msg, sizeof msg, "%s: still part slope %u at the most sensitive %u mg", k_files[f], (unsigned)still,
                 (unsigned)hy_cab_wom_mg(5));
        TEST_ASSERT_TRUE_MESSAGE(still < hy_cab_wom_mg(5), msg);
    }
    for (uint8_t s = 1; s < 5; s++) TEST_ASSERT_TRUE(hy_cab_wom_mg(s) > hy_cab_wom_mg(s + 1));
    const int knock = file_index("table_knock"), arm = file_index("shake_live_arm");
    const int lift = file_index("putdown_pickup");
    if (s_have[arm]) TEST_ASSERT_EQUAL_INT(0, counts[arm][2]);
    if (s_have[lift]) {
        TEST_ASSERT_TRUE(counts[lift][0] >= 2);
        TEST_ASSERT_TRUE(counts[lift][2] >= 2);
    }
    /* Not reachable together with a door at the default: a knock on the table and a spin on it change faster than
       a door pulled by hand (see test_any_door_default). The default fires on them; the reaction still needs a due
       dose. Least sensitive step: the knock does not fire. */
    if (s_have[knock]) TEST_ASSERT_EQUAL_INT(0, counts[knock][0]);
    /* Wakes stay bounded at the default: the measured counts, so a retune that multiplies them fails. */
    const int tap = file_index("tap_double"), spin = file_index("flat_spin");
    if (s_have[knock]) TEST_ASSERT_TRUE(counts[knock][2] <= 1);
    if (s_have[tap]) TEST_ASSERT_TRUE(counts[tap][2] <= 5);
    if (s_have[spin]) TEST_ASSERT_TRUE(counts[spin][2] <= 13);
}

/* The default fires on a nominal synthetic door, with or without the accel ramp, and on the ramped door at any
   quarter turn of the device about the vertical. */
static void test_any_door_default(void)
{
    const uint16_t thr = hy_cab_wom_mg(3);
    for (int kind = CAB_SYN_DOOR_OPEN; kind <= CAB_SYN_DOOR_CLOSE; kind++) {
        for (int ramp = 0; ramp <= 1; ramp++) {
            cab_syn_t p = cab_syn_new((cab_syn_kind_t)kind);
            p.ramp = ramp;
            p.start_ms = 400;
            static hy_cab_seq_t q;
            cab_syn_make(&p, 2000, &q);
            uint16_t peak = 0;
            for (uint32_t i = 0; i < q.n; i++) {
                uint16_t sl = stream_slope(q.s, i);
                if (sl > peak) peak = sl;
            }
            printf("synthetic door %s, ramp %d: peak slope %u mg, default %u mg\n",
                   kind == CAB_SYN_DOOR_OPEN ? "open" : "close", ramp, (unsigned)peak, (unsigned)thr);
            TEST_ASSERT_TRUE(any_count(q.s, 0, q.n, thr) >= 1);
            for (int quarter = 1; quarter < 4 && ramp; quarter++) {
                static hy_cab_seq_t t;
                t = q;
                cab_syn_rotate(&t, q.rest.x, q.rest.y, q.rest.z, 90.0 * quarter);
                TEST_ASSERT_TRUE(any_count(t.s, 0, t.n, thr) >= 1);
            }
        }
    }
}

/* ---- Taught movement on real data ---- */

static void test_taught_putdown_pickup(void)
{
    NEED("putdown_pickup");
    model_t *md = model_of("putdown_pickup");
    TEST_ASSERT_TRUE_MESSAGE(build_model(md), "putdown_pickup: fewer than 3 captures");
    for (int k = 0; k < md->n_caps; k++) expect_match_variants(md, k);
    /* Each take fits the other two at sensitivity 3. The largest of these distances is d_self, so this holds
       exactly when the threshold is not clamped under d_self (HY_CAB_THR_MAX >= d_self); it is not a held-out
       positive, as the recording has no fourth take. */
    for (int k = 0; k < HY_CAB_TAKES; k++) {
        float b = INFINITY;
        for (int j = 0; j < HY_CAB_TAKES; j++)
            if (j != k) b = fminf(b, hy_cab_dist(&md->m.take[j], &md->m.take[k]));
        char msg[96];
        snprintf(msg, sizeof msg, "take %d against the other two: %.3f over %.3f", k, b, hy_cab_threshold(&md->m, 3));
        TEST_ASSERT_TRUE_MESSAGE(b <= hy_cab_threshold(&md->m, 3), msg);
    }
    expect_no_match_others(md);
    print_margin(md);
}

/* Positives when the file has at least 4 captures (model from 3, the rest match); otherwise only the negatives
   against a model from the captures found, if there are 3. */
static void taught_from_file(const char *file, const char *cause, const char *const *neg, int n_neg)
{
    model_t *md = model_of(file);
    bool ok = build_model(md);
    if (!ok) {
        printf("%s: %d captures, no model\n", file, md->n_caps);
        static char why[160];
        snprintf(why, sizeof why, "%s: %d captures, fewer than 3 (%s)", file, md->n_caps, cause);
        TEST_IGNORE_MESSAGE(why);
    }
    if (md->n_caps >= HY_CAB_TAKES + 1)
        for (int k = HY_CAB_TAKES; k < md->n_caps; k++) expect_match_variants(md, k);
    else
        printf("%s: %d captures, positives not asserted\n", file, md->n_caps);
    if (neg) {
        for (int i = 0; i < n_neg; i++) expect_no_match_in(md, (unsigned)file_index(neg[i]));
    } else {
        expect_no_match_others(md);
    }
    print_margin(md);
}

static void test_taught_rotate_cycle(void)
{
    NEED("rotate_cycle");
    taught_from_file("rotate_cycle", "hand-held holds end the capture", NULL, 0);
}

static void test_taught_tap_pattern(void)
{
    NEED("tap_double");
    static const char *const neg[] = { "putdown_pickup", "shake_firm", "shake_light", "shake_live_arm",
                                       "shake_live_combo" };
    taught_from_file("tap_double", "taps capture 20 to 60 ms, under HY_CAB_MIN_MS", neg, (int)(sizeof neg / sizeof neg[0]));
}

/* ---- Shake to undo ---- */

static int shake_detections(const hy_imu6_t *s, uint32_t n)
{
    hy_shake_t sh;
    hy_shake_reset(&sh, 0);
    int d = 0;
    for (uint32_t i = 0; i < n; i++)
        if (hy_shake_feed(&sh, i * STEP_MS, acc(&s[i]))) d++;
    return d;
}

/* A door swing seen by the shake detector: 1 s at rest, the opening, 1 s at rest, the closing ending with a
   1.5 g bump against the frame that rings out (+1.5 g, -0.75 g, +0.375 g on the axis normal to the door). One in
   five swings slams with twice that (3 g): its first swing passes HY_SHK_SWING_MG, the ringing does not, so a
   single stroke never makes a shake. */
static void test_door_like_never_shakes(void)
{
    static hy_imu6_t s[REC_MAX];
    for (uint32_t n = 0; n < 50; n++) {
        cab_syn_t po = cab_syn_new(CAB_SYN_DOOR_OPEN), pc = cab_syn_new(CAB_SYN_DOOR_CLOSE);
        po.amp = pc.amp = 0.6f + 0.06f * (float)(n % 15);   /* 0.6 to 1.44 */
        po.dur = pc.dur = 0.6f + 0.05f * (float)((n * 7u) % 15);
        po.noise = pc.noise = true;
        po.seed = 300u + n;
        pc.seed = 600u + n;
        po.ramp = pc.ramp = (n % 2) == 1;
        po.start_ms = pc.start_ms = 1000;
        uint32_t no = (1000 + cab_syn_move_ms(&po) + 1000) / STEP_MS;
        uint32_t nc = (1000 + cab_syn_move_ms(&pc) + 1000) / STEP_MS;
        for (uint32_t i = 0; i < no; i++) s[i] = cab_syn_sample(&po, i);
        for (uint32_t i = 0; i < nc; i++) s[no + i] = cab_syn_sample(&pc, i);
        /* The bump at the end of the closing swing, on the door normal (the tangential axis of the swing). */
        uint32_t end = no + (1000 + cab_syn_move_ms(&pc)) / STEP_MS;
        static const int16_t bump[] = { 1500, -750, 375 };
        const int slam = n % 5 == 4 ? 2 : 1;
        for (int k = 0; k < 3; k++)
            s[end + (uint32_t)k].a[0] = (int16_t)(s[end + (uint32_t)k].a[0] + slam * bump[k]);
        TEST_ASSERT_EQUAL_INT(0, shake_detections(s, no + nc));
    }
}

/* A capture fed to the shake detector after 1 s at its rest vector (the detector's settle time is over). */
static int capture_shakes(const hy_cab_seq_t *q)
{
    static hy_imu6_t s[HY_CAB_RATE_HZ + HY_CAB_MAX_N];
    uint32_t n = 0;
    for (; n < HY_CAB_RATE_HZ; n++) s[n] = (hy_imu6_t){ { q->rest.x, q->rest.y, q->rest.z }, { 0, 0, 0 } };
    for (uint16_t i = 0; i < q->n; i++) s[n++] = q->s[i];
    return shake_detections(s, n);
}

static void test_taught_no_shake(void)
{
    load_all();
    int fed = 0;
    for (unsigned i = 0; i < sizeof s_models / sizeof s_models[0]; i++) {
        model_t *md = &s_models[i];
        if (!s_have[file_index(md->file)]) continue;
        build_model(md);
        for (int k = 0; k < md->n_caps; k++, fed++) TEST_ASSERT_EQUAL_INT(0, capture_shakes(&md->caps[k].seq));
        if (!md->m.valid) continue;
        /* and the augmented positives of the model's tests */
        static hy_cab_seq_t q;
        for (int k = 0; k < md->n_caps; k++)
            for (uint32_t a = 1; a <= N_AUG; a++)
                if (augment(md->teach.s, md->teach.n, &md->caps[k], 7919u * (uint32_t)k + a, md->m.trig_mg, &q)) {
                    TEST_ASSERT_EQUAL_INT(0, capture_shakes(&q));
                    fed++;
                }
    }
    /* The synthetic movements a person might teach. */
    for (int kind = CAB_SYN_DOOR_OPEN; kind <= CAB_SYN_LIFT; kind++) {
        static hy_cab_seq_t q;
        cab_syn_t p = cab_syn_new((cab_syn_kind_t)kind);
        p.noise = true;
        p.seed = 77;
        cab_syn_make(&p, 1500, &q);
        TEST_ASSERT_EQUAL_INT(0, capture_shakes(&q));
        fed++;
    }
    printf("captures fed to the shake detector: %d\n", fed);
    TEST_ASSERT_TRUE(fed > 4);
}

static void test_shake_never_matches(void)
{
    load_all();
    static const char *const shakes[] = { "shake_firm", "shake_live_combo" };
    int models = 0;
    for (unsigned i = 0; i < sizeof s_models / sizeof s_models[0]; i++) {
        model_t *md = &s_models[i];
        if (!s_have[file_index(md->file)] || !build_model(md)) continue;
        models++;
        for (unsigned k = 0; k < 2; k++) expect_no_match_in(md, (unsigned)file_index(shakes[k]));
    }
    if (models == 0) TEST_IGNORE_MESSAGE("no model from the recordings");
}

/* ---- Owner samples ---- */

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static void test_owner_samples(void)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s/cabinet", HY_TEST_DATA_DIR);
    DIR *d = opendir(dir);
    if (!d) TEST_IGNORE_MESSAGE("no data/cabinet");
    static hy_cab_seq_t takes[HY_CAB_TAKES + 1], caps[64];
    static hy_cab_model_t m;
    int teach_files = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!ends_with(e->d_name, "_teach.csv")) continue;
        char base[256], path[1024];
        size_t bl = strlen(e->d_name) - strlen("_teach.csv");
        if (bl >= sizeof base) continue;
        memcpy(base, e->d_name, bl);
        base[bl] = '\0';
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(HY_CAB_TAKES, load_download(path, DL_TEACH, takes, HY_CAB_TAKES + 1), path);
        int odd;
        if (hy_cab_model_build(&m, takes, &odd) != HY_OK) m.valid = true;
        TEST_ASSERT_TRUE_MESSAGE(isfinite(m.d_self), path);
        printf("%s: trigger %u mg, d_self %.3f, threshold at 3 %.3f\n", base, (unsigned)m.trig_mg, m.d_self,
               hy_cab_threshold(&m, 3));
        teach_files++;
        static const char *const suffix[] = { "_match.csv", "_other.csv" };
        for (int want = 0; want < 2; want++) {
            snprintf(path, sizeof path, "%s/%s%s", dir, base, suffix[want]);
            FILE *f = fopen(path, "r");
            if (!f) continue;
            fclose(f);
            int n = load_download(path, DL_CAPTURE, caps, 64);
            for (int k = 0; k < n; k++) {
                float dist;
                bool hit = hy_cab_match(&m, 3, &caps[k], &dist);
                char msg[1100];
                snprintf(msg, sizeof msg, "%s capture %d: %.3f, threshold %.3f", path, k, dist, hy_cab_threshold(&m, 3));
                printf("%s\n", msg);
                TEST_ASSERT_EQUAL_MESSAGE(want == 0, hit, msg);
            }
        }
    }
    closedir(d);
    if (teach_files == 0) TEST_IGNORE_MESSAGE("no *_teach.csv in data/cabinet");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_any_counts_per_file);
    RUN_TEST(test_any_door_default);
    RUN_TEST(test_taught_putdown_pickup);
    RUN_TEST(test_taught_rotate_cycle);
    RUN_TEST(test_taught_tap_pattern);
    RUN_TEST(test_door_like_never_shakes);
    RUN_TEST(test_taught_no_shake);
    RUN_TEST(test_shake_never_matches);
    RUN_TEST(test_owner_samples);
    return UNITY_END();
}
