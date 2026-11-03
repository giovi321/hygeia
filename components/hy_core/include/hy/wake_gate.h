#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wake decision for a touch on a dimmed or dark screen. Pure: the caller passes a millisecond clock and the touch
 * events. With single_tap_wakes off, only a double tap (HY_DOUBLE_TAP_MS, HY_DOUBLE_TAP_PX, the rule of
 * hy_gesture_double) wakes; a lone tap, a long press or a swipe as the first touch do not. */
typedef enum {
    HY_WG_NONE = 0,   /* nothing to do (no touch is being judged) */
    HY_WG_WAIT,       /* judging: a touch is down, or a second tap may still come */
    HY_WG_WAKE,       /* wake the screen */
    HY_WG_SLEEP       /* not a wake: the sequence is over, go back to sleep */
} hy_wg_act_t;

typedef struct {
    uint8_t  st;                 /* internal state */
    int16_t  x0, y0, x, y;       /* the stroke in progress: start and last position */
    uint32_t t0;                 /* its start */
    uint16_t max_dev;            /* the largest |x - x0| or |y - y0| seen */
    int16_t  px, py;             /* the end of the first tap */
    uint32_t pt;                 /* its end time */
} hy_wake_gate_t;

void        hy_wake_gate_reset(hy_wake_gate_t *g);
/* Finger down. single_tap_wakes: WAKE at once. Otherwise WAIT. */
hy_wg_act_t hy_wake_gate_down(hy_wake_gate_t *g, bool single_tap_wakes, uint32_t t_ms, int16_t x, int16_t y);
/* Finger moved while down (optional; the release position counts anyway). */
void        hy_wake_gate_move(hy_wake_gate_t *g, int16_t x, int16_t y);
/* Finger up. A first tap gives WAIT; a second tap that follows the double tap rule gives WAKE; a first or second
 * touch that is not a tap (long press, swipe) gives SLEEP. */
hy_wg_act_t hy_wake_gate_up(hy_wake_gate_t *g, uint32_t t_ms, int16_t x, int16_t y);
/* Time passing. SLEEP when no second tap came within HY_DOUBLE_TAP_MS of the first tap's release, or when a finger
 * has been down for HY_LONG_PRESS_MS (not a tap, no need to wait for the release). Otherwise WAIT or NONE. */
hy_wg_act_t hy_wake_gate_tick(hy_wake_gate_t *g, uint32_t t_ms);

#ifdef __cplusplus
}
#endif
