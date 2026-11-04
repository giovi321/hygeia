#include "hy/toast_stroke.h"

bool hy_toast_point_in_area(int x, int y, int x1, int y1, int x2, int y2)
{
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}

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
