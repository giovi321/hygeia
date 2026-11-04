#include "hy/toast_stroke.h"

hy_toast_act_t hy_toast_stroke(bool toast_showing, hy_gesture_t g, bool on_undo, bool waking, bool overlay)
{
    if (!toast_showing) {
        return HY_TS_PASS;
    }
    if (waking || overlay || on_undo || g != HY_G_TAP) {
        return HY_TS_CONSUME;
    }
    return HY_TS_CLOSE;
}
