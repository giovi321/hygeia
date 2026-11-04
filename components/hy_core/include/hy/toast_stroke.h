#pragma once

#include <stdbool.h>

#include "hy/nav.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a finished stroke does to the logged or skipped toast (the one with Undo) when the stroke began while it
   was showing. Every such stroke is consumed: it never logs a tile and never navigates.
   HY_TS_PASS     the toast was not showing: the stroke is handled as usual
   HY_TS_CONSUME  consumed, the toast stays (a tap on Undo is handled by the button; a long press, a swipe or an
                  edge gesture does nothing; a stroke that only woke the screen, or that began over the power-off
                  or pairing overlay, is left to those)
   HY_TS_CLOSE    consumed, and the toast closes now: the entry stays logged and a shake no longer undoes it
   on_undo: the stroke began on the Undo button. */
typedef enum { HY_TS_PASS = 0, HY_TS_CONSUME, HY_TS_CLOSE } hy_toast_act_t;

hy_toast_act_t hy_toast_stroke(bool toast_showing, hy_gesture_t g, bool on_undo, bool waking, bool overlay);

#ifdef __cplusplus
}
#endif
