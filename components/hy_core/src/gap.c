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
