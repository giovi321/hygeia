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

uint16_t hy_cab_wom_mg(uint8_t sens)
{
    static const uint16_t mg[5] = { 250, 160, 100, 64, 40 };
    if (sens < 1 || sens > 5) return 100;
    return mg[sens - 1];
}

bool hy_cab_wom_hit(hy_accel_t rest, hy_accel_t a, uint16_t thr_mg)
{
    return iabs32((int32_t)a.x - rest.x) > thr_mg || iabs32((int32_t)a.y - rest.y) > thr_mg ||
           iabs32((int32_t)a.z - rest.z) > thr_mg;
}

void hy_cab_cap_start(hy_cab_cap_t *c, hy_accel_t rest, bool has_gyro, uint32_t t_ms)
{
    memset(c, 0, sizeof *c);
    c->seq.rest = rest;
    c->seq.has_gyro = has_gyro;
    c->t0_ms = t_ms;
    c->last_move_ms = t_ms;
}

/* x is a sample taken ms after the start of the capture. */
static bool sample_quiet(const hy_cab_seq_t *q, const hy_imu6_t *x, uint32_t ms)
{
    if (iabs32((int32_t)x->a[0] - q->rest.x) > HY_CAB_QUIET_MG ||
        iabs32((int32_t)x->a[1] - q->rest.y) > HY_CAB_QUIET_MG ||
        iabs32((int32_t)x->a[2] - q->rest.z) > HY_CAB_QUIET_MG)
        return false;
    if (q->has_gyro && ms >= HY_CAB_GYRO_START_MS) {
        int32_t g2 = (int32_t)x->g[0] * x->g[0] + (int32_t)x->g[1] * x->g[1] + (int32_t)x->g[2] * x->g[2];
        if (g2 >= (int32_t)HY_CAB_QUIET_DDPS * HY_CAB_QUIET_DDPS) return false;
    }
    return true;
}

hy_cap_res_t hy_cab_cap_feed(hy_cab_cap_t *c, uint32_t t_ms, const hy_imu6_t *x)
{
    hy_cab_seq_t *q = &c->seq;
    if (q->n >= HY_CAB_MAX_N) return HY_CAP_TOO_LONG;
    q->s[q->n++] = *x;

    /* The trigger sample (n == 1) counts as movement whatever it reads. */
    bool quiet = q->n > 1 && sample_quiet(q, x, t_ms - c->t0_ms);
    if (!quiet) c->last_move_ms = t_ms;

    if (quiet && t_ms - c->last_move_ms >= HY_CAB_QUIET_MS) {
        while (q->n > 1 && sample_quiet(q, &q->s[q->n - 1], (uint32_t)(q->n - 1) * STEP_MS)) q->n--;
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
