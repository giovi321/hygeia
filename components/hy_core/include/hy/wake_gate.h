#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hy/nav.h"

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
    uint32_t tb;                 /* when the judgment began (hy_wake_gate_begin) */
    bool     begun;
    bool     fdown;              /* hy_wake_gate_sample saw a finger on the glass */
    uint8_t  fails;              /* failed polls in a row */
    int16_t  lx, ly;             /* its last position */
} hy_wake_gate_t;

/* Longest a touch is judged from hy_wake_gate_begin: a double tap fits in about 1.1 s. */
#define HY_WG_MAX_MS 2000

void        hy_wake_gate_reset(hy_wake_gate_t *g);
/* The CPU was woken by a touch interrupt at t_ms and the judgment starts (reset first). */
void        hy_wake_gate_begin(hy_wake_gate_t *g, uint32_t t_ms);
/* One poll of the touch controller (the caller polls every few ms after the begin): ok false = the read failed;
 * down/x/y = the finger. Turns the samples into down, move and up events and ticks the clock. A poll that shows no
 * finger before any touch was seen, two failed polls in a row before any touch (the first read after standby may fail), and a judgment older than HY_WG_MAX_MS give
 * SLEEP (noise, or a controller that does not answer). Otherwise as the three functions below. */
hy_wg_act_t hy_wake_gate_sample(hy_wake_gate_t *g, uint32_t t_ms, bool ok, bool down, int16_t x, int16_t y);
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

/* Gesture wake mode of the touch controller (`pwrx gesture`): the controller reports each finished gesture once, a tap
 * as a tap report with its position, instead of every contact. A double tap is two tap reports. The gap is counted
 * between the two reports (each is sent at a lift), so it is HY_DOUBLE_TAP_MS plus up to 200 ms for the second contact;
 * the two positions follow HY_DOUBLE_TAP_PX. */
#define HY_WG_TAP_PAIR_MS (HY_DOUBLE_TAP_MS + 200)

typedef struct {
    bool     have;    /* a first tap is held */
    uint32_t t;       /* its report time */
    int16_t  x, y;    /* its position */
} hy_tap_pair_t;

void        hy_tap_pair_reset(hy_tap_pair_t *p);
/* One gesture report at t_ms. tap: the report is a tap. single_tap_wakes: a tap gives WAKE at once. Otherwise a tap
 * that follows a held tap within HY_WG_TAP_PAIR_MS and HY_DOUBLE_TAP_PX gives WAKE; any other tap is held and gives
 * WAIT; a report that is not a tap drops the held tap and gives SLEEP. WAKE clears the held tap. */
hy_wg_act_t hy_tap_pair_feed(hy_tap_pair_t *p, bool single_tap_wakes, uint32_t t_ms, bool tap, int16_t x, int16_t y);

#ifdef __cplusplus
}
#endif
