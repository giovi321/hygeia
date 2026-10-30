#include "hy/motion.h"

static int32_t iabs32(int32_t v)
{
    return v < 0 ? -v : v;
}

void hy_shake_reset(hy_shake_t *s, uint32_t now_ms)
{
    *s = (hy_shake_t){ .start_ms = now_ms, .cool_until_ms = now_ms, .axis = -1, .lat_axis = -1 };
}

bool hy_shake_feed(hy_shake_t *s, uint32_t t_ms, hy_accel_t a)
{
    int32_t v[3] = { a.x, a.y, a.z };
    int32_t hp[3];
    for (int i = 0; i < 3; i++) {
        if (!s->primed) s->base[i] = v[i];
        hp[i] = v[i] - s->base[i];
        s->base[i] += (v[i] - s->base[i]) / 16;
    }
    s->primed = true;

    if (t_ms - s->start_ms < HY_SHK_SETTLE_MS) return false;
    if ((int32_t)(t_ms - s->cool_until_ms) < 0) {
        s->latched = false;
        return false;
    }

    if (s->latched && iabs32(hp[s->lat_axis]) < HY_SHK_REARM_MG) s->latched = false;
    if (s->latched) return false;

    int best = 0;
    for (int i = 1; i < 3; i++)
        if (iabs32(hp[i]) > iabs32(hp[best])) best = i;
    if (iabs32(hp[best]) < HY_SHK_PEAK_MG) return false;

    int8_t sign = hp[best] < 0 ? -1 : 1;
    s->latched = true;
    s->lat_axis = (int8_t)best;

    if (s->count > 0 && t_ms - s->last_ms < HY_SHK_MIN_GAP_MS) return false;

    if (s->count > 0 && s->axis == best && s->sign == -sign && t_ms - s->last_ms <= HY_SHK_MAX_GAP_MS &&
        t_ms - s->first_ms <= HY_SHK_WINDOW_MS) {
        s->count++;
        s->sign = sign;
        s->last_ms = t_ms;
    } else {
        s->axis = (int8_t)best;
        s->sign = sign;
        s->first_ms = s->last_ms = t_ms;
        s->count = 1;
    }

    if (s->count >= HY_SHK_PEAKS) {
        s->count = 0;
        s->cool_until_ms = t_ms + HY_SHK_COOLDOWN_MS;
        return true;
    }
    return false;
}
