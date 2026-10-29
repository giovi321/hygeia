#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HY_EDGE_PX        40
#define HY_SWIPE_MIN_PX   60
#define HY_TAP_SLOP_PX    16
#define HY_LONG_PRESS_MS  500
#define HY_DOUBLE_TAP_MS  300
#define HY_DOUBLE_TAP_PX  40

typedef enum { HY_G_NONE = 0, HY_G_TAP, HY_G_LONG_PRESS, HY_G_SWIPE_LEFT, HY_G_SWIPE_RIGHT, HY_G_SWIPE_UP,
               HY_G_SWIPE_DOWN, HY_G_EDGE_DOWN, HY_G_EDGE_UP, HY_G_EDGE_RIGHT } hy_gesture_t;
typedef struct { int16_t x0, y0, x1, y1; uint32_t t0_ms, t1_ms; } hy_stroke_t;

/* Tap: movement <= 16 px both axes and duration < 500 ms. Long press: <= 16 px and >= 500 ms.
   Swipe: dominant axis >= 60 px and >= 1.5x the other axis, else NONE. Edge variants: EDGE_DOWN when a
   downward swipe starts at y0 < 40; EDGE_UP when an upward swipe starts at y0 >= 440; EDGE_RIGHT when a
   rightward swipe starts at x0 < 40. Screen 480x480. */
hy_gesture_t hy_gesture_classify(const hy_stroke_t *s);
/* Two taps within 300 ms (prev t1 to cur t0) and 40 px. */
bool hy_gesture_double(const hy_stroke_t *prev_tap, const hy_stroke_t *tap);

typedef enum { HY_V_HOME = 0, HY_V_OVERVIEW, HY_V_HISTORY, HY_V_DETAIL, HY_V_NOTE, HY_V_EARLIER, HY_V_QS,
               HY_V_SETTINGS, HY_V_SET_ZONE, HY_V_SET_TIME, HY_V_SET_SOUND, HY_V_SET_DISPLAY, HY_V_ABOUT,
               HY_V_CLOCK_INVALID, HY_V_STORAGE_DOWN } hy_view_t;
typedef enum { HY_FX_NONE = 0, HY_FX_WAKE, HY_FX_SNOOZE, HY_FX_DUE_NEXT } hy_nav_fx_t;
#define HY_NAV_DEPTH 6
typedef struct { hy_view_t stack[HY_NAV_DEPTH]; uint8_t depth; uint32_t med_id;
                 bool due, idle, clock_invalid, storage_down; } hy_nav_t;

void        hy_nav_init(hy_nav_t *n);                 /* root HOME, depth 1 */
hy_view_t   hy_nav_view(const hy_nav_t *n);           /* top of the stack */
void        hy_nav_push(hy_nav_t *n, hy_view_t v);    /* ignored when full */
void        hy_nav_open_detail(hy_nav_t *n, uint32_t med_id);
void        hy_nav_set_root_flags(hy_nav_t *n, bool clock_invalid, bool storage_down);
hy_nav_fx_t hy_nav_gesture(hy_nav_t *n, hy_gesture_t g);

/* Burn-in offset for minute index `minute`: a 16-step closed path within -8..+8 px on both axes, a different
   offset each consecutive minute. */
void hy_idle_drift(int64_t minute, int8_t *dx, int8_t *dy);

#ifdef __cplusplus
}
#endif
