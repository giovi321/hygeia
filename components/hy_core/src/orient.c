#include "hy/motion.h"

bool hy_orient_candidate(hy_accel_t a, hy_rot_t *out)
{
    int32_t x = a.x, y = a.y, z = a.z;
    /* int16 inputs: each square fits 2^30, the sum of three needs int64 */
    int64_t plane2 = (int64_t)x * x + (int64_t)y * y;
    int64_t mag2 = plane2 + (int64_t)z * z;
    if (plane2 < (int64_t)HY_OR_PLANE_MIN_MG * HY_OR_PLANE_MIN_MG) return false;
    if (mag2 < (int64_t)HY_OR_MAG_MIN_MG * HY_OR_MAG_MIN_MG || mag2 > (int64_t)HY_OR_MAG_MAX_MG * HY_OR_MAG_MAX_MG)
        return false;

    int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    hy_rot_t r;
    if (ay >= ax) {
        if (ax * 1000 > ay * HY_OR_TAN_PERMILLE) return false;
        r = y < 0 ? HY_ROT_0 : HY_ROT_180;
    } else {
        if (ay * 1000 > ax * HY_OR_TAN_PERMILLE) return false;
        r = x > 0 ? HY_ROT_90 : HY_ROT_270;
    }
    if (out) *out = r;
    return true;
}

void hy_orient_init(hy_orient_t *o, hy_rot_t cur)
{
    *o = (hy_orient_t){ .cur = cur, .cand = cur };
}

void hy_orient_resume(hy_orient_t *o)
{
    o->has_cand = false;
    o->has_last = false;
    o->quick = true;
}

bool hy_orient_feed(hy_orient_t *o, uint32_t t_ms, hy_accel_t a)
{
    if (o->quick && !o->has_last) o->quick_since_ms = t_ms;
    if (o->has_last && t_ms - o->last_ms > HY_OR_GAP_MS) o->has_cand = false;
    o->last_ms = t_ms;
    o->has_last = true;

    /* the wake dwell ends once a full HY_OR_DWELL_MS of samples has passed since resume */
    if (o->quick && t_ms - o->quick_since_ms >= HY_OR_DWELL_MS) o->quick = false;

    hy_rot_t c;
    if (!hy_orient_candidate(a, &c) || c == o->cur) {
        o->has_cand = false;
        return false;
    }
    if (!o->has_cand || o->cand != c) {
        o->cand = c;
        o->has_cand = true;
        o->cand_since_ms = t_ms;
    }
    uint32_t dwell = o->quick ? HY_OR_WAKE_DWELL_MS : HY_OR_DWELL_MS;
    if (t_ms - o->cand_since_ms < dwell) return false;
    o->cur = c;
    o->has_cand = false;
    o->quick = false;
    return true;
}

hy_rot_t hy_orient_current(const hy_orient_t *o)
{
    return o->cur;
}

void hy_orient_set(hy_orient_t *o, hy_rot_t r)
{
    o->cur = r;
    o->has_cand = false;
}
