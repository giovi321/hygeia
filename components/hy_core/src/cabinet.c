#include <math.h>
#include <string.h>
#include "hy/cabinet.h"

#define STEP_MS (1000 / HY_CAB_RATE_HZ)
#define STILL_MEAN_N 10

static int32_t iabs32(int32_t v)
{
    return v < 0 ? -v : v;
}

void hy_cab_features(const hy_cab_seq_t *q, hy_cab_feat_t *out)
{
    float rx = q->rest.x, ry = q->rest.y, rz = q->rest.z;
    float rn = sqrtf(rx * rx + ry * ry + rz * rz);
    if (rn < 1.0f) {
        for (uint16_t i = 0; i < q->n; i++) out[i] = (hy_cab_feat_t){0};
        return;
    }
    float gx = rx / rn, gy = ry / rn, gz = rz / rn;
    for (uint16_t i = 0; i < q->n; i++) {
        const hy_imu6_t *s = &q->s[i];
        float ax = s->a[0], ay = s->a[1], az = s->a[2];
        float ad = ax * gx + ay * gy + az * gz;
        float hx = ax - ad * gx, hy = ay - ad * gy, hz = az - ad * gz;
        hy_cab_feat_t f = { 0.0f, 0.0f, ad - rn, sqrtf(hx * hx + hy * hy + hz * hz) };
        if (q->has_gyro && (uint32_t)i * STEP_MS >= HY_CAB_GYRO_START_MS) {
            float wx = s->g[0] * 0.1f, wy = s->g[1] * 0.1f, wz = s->g[2] * 0.1f;
            float wv = wx * gx + wy * gy + wz * gz;
            float px = wx - wv * gx, py = wy - wv * gy, pz = wz - wv * gz;
            f.gv = wv;
            f.gh = sqrtf(px * px + py * py + pz * pz);
        }
        out[i] = f;
    }
}

/* The trigger sees the change over one sensor sample (48 ms). Bounds from the replay of the recordings and the
   synthetic door (test_cab_rec.c). At rest the change stays at 31 mg at most (3 samples over 28 mg in 88 s of
   still parts). A door pulled by hand to 120 dps in 0.4 s, 0.15 m from the hinge, changes by 48 mg at most (its
   tangential acceleration rises smoothly to 126 mg); a knock on the table the device lies on by 90 mg.
   - 100: above the knock (0 triggers); still a lift (putdown_pickup 8) and a firm door (2.1 times nominal)
   - 64: a door 1.33 times nominal; the knock fires once
   - 40 (default): the nominal door down to 0.83 of it; equal to the teach trigger
   - 37: a door 0.77 of nominal
   - 34: a door 0.71 of nominal; 3 mg above the largest change at rest */
uint16_t hy_cab_wom_mg(uint8_t sens)
{
    static const uint16_t mg[5] = { 100, 64, 40, 37, 34 };
    if (sens < 1 || sens > 5) return 40;
    return mg[sens - 1];
}

bool hy_cab_wom_hit(hy_accel_t prev, hy_accel_t a, uint16_t thr_mg)
{
    return iabs32((int32_t)a.x - prev.x) > thr_mg || iabs32((int32_t)a.y - prev.y) > thr_mg ||
           iabs32((int32_t)a.z - prev.z) > thr_mg;
}

#if HY_CAB_WOM_PERIOD_MS < 2 * STEP_MS || HY_CAB_WOM_PERIOD_MS > 3 * STEP_MS
#error "hy_cab_wom_prev interpolates between the samples 2 and 3 steps back"
#endif

/* b2 + (b3 - b2) * (period - 2 steps) / 1 step, rounded to the nearest mg */
static int16_t wom_lerp(int16_t b2, int16_t b3)
{
    int32_t num = ((int32_t)b3 - b2) * (HY_CAB_WOM_PERIOD_MS - 2 * STEP_MS);
    int32_t q = (num >= 0 ? num + STEP_MS / 2 : num - STEP_MS / 2) / STEP_MS;
    return (int16_t)(b2 + q);
}

hy_accel_t hy_cab_wom_prev(hy_accel_t back2, hy_accel_t back3)
{
    return (hy_accel_t){ wom_lerp(back2.x, back3.x), wom_lerp(back2.y, back3.y), wom_lerp(back2.z, back3.z) };
}

static hy_accel_t seq_accel(const hy_cab_seq_t *q, int32_t i)
{
    if (i < 0) return q->rest;
    return (hy_accel_t){ q->s[i].a[0], q->s[i].a[1], q->s[i].a[2] };
}

bool hy_cab_wom_hit_at(const hy_cab_seq_t *q, uint16_t i, uint16_t thr_mg)
{
    hy_accel_t prev = hy_cab_wom_prev(seq_accel(q, (int32_t)i - 2), seq_accel(q, (int32_t)i - 3));
    return hy_cab_wom_hit(prev, seq_accel(q, i), thr_mg);
}

void hy_cab_cap_start(hy_cab_cap_t *c, hy_accel_t rest, bool has_gyro, uint32_t t_ms)
{
    memset(c, 0, sizeof *c);
    c->seq.rest = rest;
    c->seq.has_gyro = has_gyro;
    c->t0_ms = t_ms;
    c->last_move_ms = t_ms;
}

/* Sample j of a capture (j * STEP_MS after its start) turns slower than HY_CAB_QUIET_DDPS, or its gyro does not
   count (absent, or still starting up). */
static bool gyro_quiet(const hy_cab_seq_t *q, uint16_t j)
{
    if (!q->has_gyro || (uint32_t)j * STEP_MS < HY_CAB_GYRO_START_MS) return true;
    const int16_t *g = q->s[j].g;
    int32_t g2 = (int32_t)g[0] * g[0] + (int32_t)g[1] * g[1] + (int32_t)g[2] * g[2];
    return g2 < (int32_t)HY_CAB_QUIET_DDPS * HY_CAB_QUIET_DDPS;
}

/* The first sample of the still tail: the earliest k >= 1 such that over samples k to n - 1 every accel axis stays
   within a range of HY_CAB_QUIET_MG and the gyro is quiet. n when the last sample is not quiet itself. The pose
   the tail holds does not matter, so a movement that ends in a new pose ends as well as one that comes back. */
static uint16_t still_tail(const hy_cab_seq_t *q)
{
    int16_t lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 };
    uint16_t k = q->n;
    while (k > 1) {
        const hy_imu6_t *x = &q->s[k - 1];
        if (!gyro_quiet(q, (uint16_t)(k - 1))) break;
        bool ok = true;
        for (int a = 0; a < 3; a++) {
            int16_t l = k == q->n ? x->a[a] : (x->a[a] < lo[a] ? x->a[a] : lo[a]);
            int16_t h = k == q->n ? x->a[a] : (x->a[a] > hi[a] ? x->a[a] : hi[a]);
            if ((int32_t)h - l > HY_CAB_QUIET_MG) ok = false;
            lo[a] = l;
            hi[a] = h;
        }
        if (!ok) break;
        k--;
    }
    return k;
}

hy_cap_res_t hy_cab_cap_feed(hy_cab_cap_t *c, uint32_t t_ms, const hy_imu6_t *x)
{
    hy_cab_seq_t *q = &c->seq;
    if (q->n >= HY_CAB_MAX_N) return HY_CAP_TOO_LONG;
    q->s[q->n++] = *x;

    /* The trigger sample (index 0) counts as movement whatever it reads. */
    uint16_t k = still_tail(q);
    if (k == q->n) c->last_move_ms = t_ms;
    if ((uint32_t)(q->n - k) * STEP_MS >= HY_CAB_QUIET_MS) {
        q->n = k;
        return (uint32_t)q->n * STEP_MS < HY_CAB_MIN_MS ? HY_CAP_TOO_SHORT : HY_CAP_DONE;
    }
    return q->n >= HY_CAB_MAX_N ? HY_CAP_TOO_LONG : HY_CAP_MORE;
}

void hy_cab_still_reset(hy_cab_still_t *st)
{
    memset(st, 0, sizeof *st);
}

static void still_push(hy_cab_still_t *st, hy_accel_t a)
{
    st->sum_buf[st->idx] = a;
    st->idx = (uint8_t)((st->idx + 1) % STILL_MEAN_N);
    if (st->cnt < STILL_MEAN_N) st->cnt++;
}

static int16_t mean_round(int32_t sum)
{
    return (int16_t)((sum >= 0 ? sum + STILL_MEAN_N / 2 : sum - STILL_MEAN_N / 2) / STILL_MEAN_N);
}

bool hy_cab_still_feed(hy_cab_still_t *st, uint32_t t_ms, hy_accel_t a, hy_accel_t *rest)
{
    if (!st->has_last || iabs32((int32_t)a.x - st->last.x) >= HY_CAB_STILL_STEP_MG ||
        iabs32((int32_t)a.y - st->last.y) >= HY_CAB_STILL_STEP_MG ||
        iabs32((int32_t)a.z - st->last.z) >= HY_CAB_STILL_STEP_MG) {
        st->still_since = t_ms;
        st->idx = 0;
        st->cnt = 0;
    }
    st->has_last = true;
    st->last = a;
    still_push(st, a);

    if (t_ms - st->still_since < HY_CAB_STILL_MS || st->cnt < STILL_MEAN_N) return false;
    if (rest) {
        int32_t sx = 0, sy = 0, sz = 0;
        for (uint8_t i = 0; i < STILL_MEAN_N; i++) {
            sx += st->sum_buf[i].x;
            sy += st->sum_buf[i].y;
            sz += st->sum_buf[i].z;
        }
        *rest = (hy_accel_t){ mean_round(sx), mean_round(sy), mean_round(sz) };
    }
    return true;
}
