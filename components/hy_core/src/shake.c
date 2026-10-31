#include "hy/motion.h"

static int32_t iabs32(int32_t v)
{
    return v < 0 ? -v : v;
}

void hy_shake_reset(hy_shake_t *s, uint32_t now_ms)
{
    *s = (hy_shake_t){ .start_ms = now_ms };
}

/* Follows one axis. Returns true when this sample confirms a stroke that makes the count reach HY_SHK_STROKES. */
static bool track(hy_shake_t *s, hy_shake_axis_t *a, uint32_t t_ms, int32_t x)
{
    if (!s->primed) {
        a->hi = a->lo = x;
        a->t_hi = a->t_lo = t_ms;
        return false;
    }

    if (a->dir >= 0 && x > a->hi) { a->hi = x; a->t_hi = t_ms; }
    if (a->dir <= 0 && x < a->lo) { a->lo = x; a->t_lo = t_ms; }

    int32_t tv;
    uint32_t tt;
    if (a->dir >= 0 && a->hi - x >= HY_SHK_REVERSAL_MG) {
        tv = a->hi;
        tt = a->t_hi;
        a->dir = -1;
        a->lo = x;
        a->t_lo = t_ms;
    } else if (a->dir <= 0 && x - a->lo >= HY_SHK_REVERSAL_MG) {
        tv = a->lo;
        tt = a->t_lo;
        a->dir = 1;
        a->hi = x;
        a->t_hi = t_ms;
    } else {
        return false;
    }

    if (!a->have_prev) {
        a->have_prev = true;
        a->prev_v = tv;
        a->prev_ms = tt;
        return false;
    }
    bool stroke = iabs32(tv - a->prev_v) >= HY_SHK_SWING_MG && tt - a->prev_ms <= HY_SHK_STROKE_MS;
    a->prev_v = tv;
    a->prev_ms = tt;
    if (stroke) s->last_stroke_ms = t_ms;
    if (!stroke || s->cooling || t_ms - s->start_ms < HY_SHK_SETTLE_MS) {
        a->count = 0;
        return false;
    }
    return ++a->count >= HY_SHK_STROKES;
}

bool hy_shake_feed(hy_shake_t *s, uint32_t t_ms, hy_accel_t a)
{
    int32_t v[3] = { a.x, a.y, a.z };
    bool hit = false;
    for (int i = 0; i < 3; i++)
        if (track(s, &s->ax[i], t_ms, v[i])) hit = true;
    s->primed = true;

    if (hit) {
        for (int i = 0; i < 3; i++) s->ax[i].count = 0;
        s->cooling = true;
        s->cool_until_ms = t_ms + HY_SHK_COOLDOWN_MS;
        return true;
    }
    if (s->cooling && (int32_t)(t_ms - s->cool_until_ms) >= 0 && t_ms - s->last_stroke_ms >= HY_SHK_CALM_MS)
        s->cooling = false;
    return false;
}
