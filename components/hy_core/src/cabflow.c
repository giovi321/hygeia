#include "hy/cabflow.h"

void hy_cab_flow_init(hy_cf_t *f)
{
    f->phase = HY_CF_IDLE;
    f->status = HY_CF_S_NONE;
}

static hy_cf_out_t out(hy_cf_t *f, uint8_t phase, uint8_t status, hy_cab_cue_t cue)
{
    f->phase = phase;
    f->status = status;
    return (hy_cf_out_t){phase, status, cue};
}

hy_cf_out_t hy_cab_flow_event(hy_cf_t *f, hy_cabev_kind_t kind, uint8_t take)
{
    uint8_t p = f->phase, s = f->status;
    bool teach = p == HY_CF_RUNNING;
    bool check = p == HY_CF_CHECKING;
    if (!teach && !check) return (hy_cf_out_t){p, s, HY_CUE_NONE};
    switch (kind) {
    case HY_CABEV_TEACH_READY:
        return out(f, p, HY_CF_S_READY, HY_CUE_NONE);
    case HY_CABEV_TEACH_STILL:
        return out(f, p, HY_CF_S_STILL, HY_CUE_NONE);
    case HY_CABEV_TEACH_GO:
        return out(f, p, HY_CF_S_NOW, HY_CUE_GO);
    case HY_CABEV_TOO_SMALL:
        return out(f, p, HY_CF_S_SMALL, HY_CUE_REDO);
    case HY_CABEV_TOO_LONG:
        return out(f, p, HY_CF_S_LONG, HY_CUE_REDO);
    default:
        break;
    }
    if (teach) {
        switch (kind) {
        case HY_CABEV_TEACH_TAKE:
            return out(f, p, HY_CF_S_GOT, take < HY_CAB_TAKES ? HY_CUE_TAKE : HY_CUE_NONE);
        case HY_CABEV_TEACH_ODD:
            return out(f, p, HY_CF_S_ODD, HY_CUE_REDO);
        case HY_CABEV_TEACH_DONE:
            return out(f, HY_CF_SAVED, HY_CF_S_NONE, HY_CUE_DONE);
        case HY_CABEV_TEACH_FAIL:
            return out(f, HY_CF_FAILED,
                       take == HY_CAB_FAIL_NO_MOVE ? HY_CF_S_NOMOVE : take == HY_CAB_FAIL_SAVE ? HY_CF_S_IO : HY_CF_S_STOPPED,
                       HY_CUE_FAIL);
        default:
            break;
        }
    } else if (kind == HY_CABEV_CHECK_END) {
        switch (take) {
        case HY_CAB_CHECK_OK:
            return out(f, HY_CF_CHECK_OK, HY_CF_S_NONE, HY_CUE_DONE);
        case HY_CAB_CHECK_MISS:
            return out(f, HY_CF_CHECK_MISS, HY_CF_S_NONE, HY_CUE_FAIL);
        case HY_CAB_CHECK_NO_MOVE:
            return out(f, HY_CF_CHECK_NOMOVE, HY_CF_S_NONE, HY_CUE_FAIL);
        default:   /* aborted: back to the saved screen, so the test can be started again */
            return out(f, HY_CF_SAVED, HY_CF_S_NONE, HY_CUE_NONE);
        }
    }
    return (hy_cf_out_t){p, s, HY_CUE_NONE};
}

uint8_t hy_cab_flow_user(hy_cf_t *f, hy_cf_user_t u)
{
    switch (u) {
    case HY_CF_U_START:
        if (f->phase == HY_CF_IDLE || f->phase == HY_CF_FAILED) {
            f->phase = HY_CF_RUNNING;
            f->status = HY_CF_S_NONE;
        }
        break;
    case HY_CF_U_TEST:
        if (f->phase == HY_CF_SAVED) {
            f->phase = HY_CF_CHECKING;
            f->status = HY_CF_S_NONE;
        }
        break;
    case HY_CF_U_AGAIN:
        if (f->phase == HY_CF_CHECK_MISS || f->phase == HY_CF_CHECK_NOMOVE) {
            f->phase = HY_CF_RUNNING;
            f->status = HY_CF_S_NONE;
        }
        break;
    }
    return f->phase;
}

const char *hy_cab_cue_name(hy_cab_cue_t c)
{
    static const char *const n[] = {"none", "go", "take", "redo", "done", "fail"};
    return (unsigned)c < sizeof n / sizeof n[0] ? n[c] : "?";
}
