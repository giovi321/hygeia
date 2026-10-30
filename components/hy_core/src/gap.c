#include "hy/gap.h"

static const char *const REASON_NAMES[] = { "off", "lost", "clock", "jump" };

bool hy_gap_on_start(const hy_alive_t *a, int64_t now, bool clock_was_invalid, hy_gap_t *out,
                     hy_gap_reason_t *why)
{
    if (!a || !out || !why || a->alive <= 0 || a->alive > now) return false;
    if (now - a->alive < HY_GAP_MIN_S) return false;
    out->start = a->alive;
    out->end = now;
    *why = clock_was_invalid ? HY_GAP_CLOCK : (a->clean ? HY_GAP_OFF : HY_GAP_LOST);
    return true;
}

bool hy_gap_on_jump(int64_t pre, int64_t now, hy_gap_t *out)
{
    if (!out || now - pre < HY_GAP_MIN_S) return false;
    out->start = pre;
    out->end = now;
    return true;
}

const char *hy_gap_reason_str(hy_gap_reason_t r)
{
    return ((unsigned)r <= (unsigned)HY_GAP_JUMP) ? REASON_NAMES[r] : REASON_NAMES[0];
}

bool hy_gap_pend_take(hy_gap_pending_t *p, hy_gap_t *out)
{
    if (!p || !out || !p->active) return false;
    *out = p->gap;
    p->active = false;
    return true;
}

bool hy_gap_pend_jump(hy_gap_pending_t *p, int64_t pre, int64_t now, hy_gap_t *flushed)
{
    hy_gap_t g;
    if (!p || !flushed || !hy_gap_on_jump(pre, now, &g)) return false;
    bool had = hy_gap_pend_take(p, flushed);
    p->gap = g;
    p->active = true;
    return had;
}

hy_gap_back_t hy_gap_pend_back(hy_gap_pending_t *p, int64_t now)
{
    if (!p || !p->active) return HY_GAP_PEND_NONE;
    if (now >= p->gap.end) return HY_GAP_PEND_KEPT;
    if (now - p->gap.start >= HY_GAP_MIN_S) {
        p->gap.end = now;
        return HY_GAP_PEND_CLIPPED;
    }
    p->active = false;
    return HY_GAP_PEND_DROPPED;
}

bool hy_gap_pend_due(hy_gap_pending_t *p, int64_t now, hy_gap_t *out)
{
    if (!p || !p->active || now - p->gap.end < HY_GAP_CONFIRM_S) return false;
    return hy_gap_pend_take(p, out);
}
