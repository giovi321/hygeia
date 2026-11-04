#include <string.h>
#include "hy/cabteach.h"

static hy_ct_out_t ev_out(hy_cabev_kind_t ev, uint8_t take)
{
    hy_ct_out_t o;
    memset(&o, 0, sizeof o);
    o.has_ev = true;
    o.ev = ev;
    o.take = take;
    return o;
}

static hy_ct_out_t end_out(hy_ct_t *c, hy_cabev_kind_t ev, uint8_t take)
{
    hy_ct_out_t o = ev_out(ev, take);
    o.end = true;
    c->phase = HY_CT_OFF;
    return o;
}

static hy_ct_out_t fail(hy_ct_t *c, hy_cab_fail_t why, hy_cab_check_t check_why)
{
    return c->mode == HY_CT_CHECK ? end_out(c, HY_CABEV_CHECK_END, (uint8_t)check_why)
                                  : end_out(c, HY_CABEV_TEACH_FAIL, (uint8_t)why);
}

static void pause_from(hy_ct_t *c, uint32_t t)
{
    c->phase = HY_CT_PAUSE;
    c->t0 = t + HY_CT_PAUSE_MS;
}

hy_ct_out_t hy_ct_begin(hy_ct_t *c, hy_ct_mode_t mode, uint32_t t)
{
    memset(c, 0, sizeof *c);
    c->mode = (uint8_t)mode;
    c->phase = HY_CT_LEAD;
    c->t0 = t;
    return ev_out(HY_CABEV_TEACH_READY, 1);
}

static uint8_t take_no(const hy_ct_t *c)
{
    return c->mode == HY_CT_CHECK ? 1 : (uint8_t)(c->cur + 1);
}

static hy_ct_out_t enter_still(hy_ct_t *c, uint32_t t)
{
    c->phase = HY_CT_STILL;
    c->t0 = t;
    hy_ct_out_t o = ev_out(HY_CABEV_TEACH_STILL, take_no(c));
    o.reset_still = true;
    return o;
}

hy_ct_out_t hy_ct_step(hy_ct_t *c, uint32_t t, bool still, bool hit)
{
    hy_ct_out_t o;
    memset(&o, 0, sizeof o);
    switch (c->phase) {
    case HY_CT_LEAD:
        if (t - c->t0 >= HY_CT_LEAD_MS) return enter_still(c, t);
        break;
    case HY_CT_PAUSE:
        if ((int32_t)(t - c->t0) >= 0) return enter_still(c, t);
        break;
    case HY_CT_STILL:
        if (still) {
            c->phase = HY_CT_GO;
            c->t0 = t;
            o = ev_out(HY_CABEV_TEACH_GO, take_no(c));
            o.take_rest = true;
        } else if (t - c->t0 >= HY_CT_STILL_MS) {
            return fail(c, HY_CAB_FAIL_STILL, HY_CAB_CHECK_ABORT);
        }
        break;
    case HY_CT_GO:
        if (hit) {
            c->phase = HY_CT_CAP;
            o.start_capture = true;
        } else if (t - c->t0 >= HY_CT_GO_MS) {
            return fail(c, HY_CAB_FAIL_NO_MOVE, HY_CAB_CHECK_NO_MOVE);
        }
        break;
    default:
        break;
    }
    return o;
}

hy_ct_out_t hy_ct_capture_failed(hy_ct_t *c, uint32_t t)
{
    hy_ct_out_t o;
    memset(&o, 0, sizeof o);
    if (c->phase == HY_CT_CAP) pause_from(c, t);
    return o;
}

hy_ct_out_t hy_ct_capture_done(hy_ct_t *c, uint32_t t, hy_cap_res_t res)
{
    hy_ct_out_t o;
    memset(&o, 0, sizeof o);
    if (c->phase != HY_CT_CAP) return o;
    if (res == HY_CAP_TOO_SHORT || res == HY_CAP_TOO_LONG) {
        o = ev_out(res == HY_CAP_TOO_SHORT ? HY_CABEV_TOO_SMALL : HY_CABEV_TOO_LONG, take_no(c));
        pause_from(c, t);
        return o;
    }
    if (c->mode == HY_CT_CHECK) {
        c->phase = HY_CT_BUILD;
        o.store = true;
        o.slot = 0;
        o.match = true;
        return o;
    }
    o = ev_out(HY_CABEV_TEACH_TAKE, (uint8_t)(c->cur + 1));
    o.store = true;
    o.slot = c->cur;
    if (c->takes_n < HY_CAB_TAKES && c->cur == c->takes_n) c->takes_n++;
    if (c->takes_n == HY_CAB_TAKES) {
        c->phase = HY_CT_BUILD;
        o.build = true;
        return o;
    }
    c->cur = c->takes_n;
    pause_from(c, t);
    return o;
}

hy_ct_out_t hy_ct_build_result(hy_ct_t *c, uint32_t t, bool ok, int odd, bool d_self_finite)
{
    hy_ct_out_t o;
    memset(&o, 0, sizeof o);
    if (c->phase != HY_CT_BUILD || c->mode != HY_CT_TEACH) return o;
    if (ok) {
        o.save = true;
        return o;
    }
    if (odd >= 0 && odd < HY_CAB_TAKES && c->redos < HY_CT_REDOS) {
        c->redos++;
        c->cur = (uint8_t)odd;
        o = ev_out(HY_CABEV_TEACH_ODD, (uint8_t)(odd + 1));
        pause_from(c, t);
        return o;
    }
    if (odd >= 0 && d_self_finite) {
        o.save = true;
        o.accept_as_is = true;
        return o;
    }
    return fail(c, HY_CAB_FAIL_MODEL, HY_CAB_CHECK_ABORT);
}

hy_ct_out_t hy_ct_saved(hy_ct_t *c, bool ok)
{
    if (ok) return end_out(c, HY_CABEV_TEACH_DONE, HY_CAB_TAKES);
    return end_out(c, HY_CABEV_TEACH_FAIL, HY_CAB_FAIL_SAVE);
}

hy_ct_out_t hy_ct_check_result(hy_ct_t *c, bool match)
{
    return end_out(c, HY_CABEV_CHECK_END, match ? HY_CAB_CHECK_OK : HY_CAB_CHECK_MISS);
}

hy_ct_out_t hy_ct_abort(hy_ct_t *c)
{
    return fail(c, HY_CAB_FAIL_ABORT, HY_CAB_CHECK_ABORT);
}

bool hy_ct_active(const hy_ct_t *c)
{
    return c->phase != HY_CT_OFF;
}
