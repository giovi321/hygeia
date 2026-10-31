#ifndef HY_MOTION_H
#define HY_MOTION_H

#include <stdbool.h>
#include <stdint.h>

/* Acceleration in milli-g in the screen frame of HY_ROT_0: +x toward the right edge, +y toward the bottom edge,
   +z out of the screen. At rest the sensor reads the reaction to gravity: upright in HY_ROT_0 gives (0, -1000, 0). */
typedef struct { int16_t x, y, z; } hy_accel_t;
/* The UI turned clockwise on the panel. The edge that is up: 0 top (-y up), 90 right (+x up), 180 bottom (+y up),
   270 left (-x up). */
typedef enum { HY_ROT_0 = 0, HY_ROT_90, HY_ROT_180, HY_ROT_270 } hy_rot_t;

#define HY_OR_DWELL_MS       1000
#define HY_OR_WAKE_DWELL_MS   250
#define HY_OR_GAP_MS          200   /* a gap between samples longer than this restarts the dwell */
#define HY_OR_PLANE_MIN_MG    500   /* x^2 + y^2 >= 500^2 */
#define HY_OR_MAG_MIN_MG      800   /* 800^2 <= x^2 + y^2 + z^2 <= 1200^2 */
#define HY_OR_MAG_MAX_MG     1200
#define HY_OR_TAN_PERMILLE    577   /* within 30 degrees of an axis: |minor| * 1000 <= |major| * 577 */

/* quick_since_ms is the time of the first sample after hy_orient_resume. */
typedef struct {
    hy_rot_t cur, cand;
    bool has_cand;
    bool quick;
    uint32_t cand_since_ms, last_ms;
    bool has_last;
    uint32_t quick_since_ms;
} hy_orient_t;

/* The orientation the sample points to, false when none (flat, moving, near a diagonal). Integer math only. */
bool     hy_orient_candidate(hy_accel_t a, hy_rot_t *out);
/* Starts with the full HY_OR_DWELL_MS dwell (quick is false); only hy_orient_resume enables the short one. */
void     hy_orient_init(hy_orient_t *o, hy_rot_t cur);
/* After the IMU was off: forgets the candidate and the last sample; the next switch needs HY_OR_WAKE_DWELL_MS. */
void     hy_orient_resume(hy_orient_t *o);
/* Feeds one sample. A candidate different from cur that holds on every sample for the dwell switches cur; the
   dwell is HY_OR_WAKE_DWELL_MS until the first switch or the first full HY_OR_DWELL_MS of samples after resume,
   then HY_OR_DWELL_MS. No candidate, another candidate, or a gap > HY_OR_GAP_MS restarts the dwell. Returns true
   when cur changed. */
bool     hy_orient_feed(hy_orient_t *o, uint32_t t_ms, hy_accel_t a);
hy_rot_t hy_orient_current(const hy_orient_t *o);
void     hy_orient_set(hy_orient_t *o, hy_rot_t r);   /* forced (lock, debug): cur = r, candidate cleared */

#define HY_SHK_SWING_MG    3000  /* a stroke: the change between two successive turning points on one axis */
#define HY_SHK_REVERSAL_MG  500  /* the signal must back off this far from its extreme to confirm a turning point */
#define HY_SHK_STROKES        3  /* consecutive strokes on one axis that make a shake */
#define HY_SHK_STROKE_MS    200  /* a stroke longer than this is slow motion, not a shake */
#define HY_SHK_SETTLE_MS    200  /* after reset: strokes are tracked but not counted */
#define HY_SHK_COOLDOWN_MS 1000  /* after a detection, at least this long without detections */
#define HY_SHK_CALM_MS      400  /* and until this long has passed since the last stroke */

/* Per axis: hi and lo are the extremes since the last turning point (t_hi, t_lo their times), dir is 1 while rising,
   -1 while falling, 0 before the first turning point; prev_v and prev_ms are the last confirmed turning point, valid
   when have_prev; count is the run of consecutive strokes. cooling is true from a detection until the cooldown ends. */
typedef struct { int32_t hi, lo, prev_v; uint32_t t_hi, t_lo, prev_ms; int8_t dir; bool have_prev; uint8_t count; }
    hy_shake_axis_t;
typedef struct { hy_shake_axis_t ax[3]; uint32_t start_ms, cool_until_ms, last_stroke_ms; bool primed, cooling; }
    hy_shake_t;

void hy_shake_reset(hy_shake_t *s, uint32_t now_ms);
/* The raw acceleration of each axis (gravity included, no filter) is followed for turning points: a maximum is
   confirmed when the value falls HY_SHK_REVERSAL_MG below it, a minimum when it rises that far above it. The
   change from the previous turning point to a new one is a stroke when it is at least HY_SHK_SWING_MG and took at
   most HY_SHK_STROKE_MS; the first turning point after reset only starts the chain. A stroke that does not qualify
   sets that axis' count to 0; a qualifying one adds 1 to it. A count reaching HY_SHK_STROKES on any axis returns
   true once; every count is cleared and nothing is counted until HY_SHK_COOLDOWN_MS have passed since the
   detection and HY_SHK_CALM_MS since the last qualifying stroke on any axis (so one long shake gives one
   detection). Within HY_SHK_SETTLE_MS of the reset nothing is counted (a stroke there clears the count). */
bool hy_shake_feed(hy_shake_t *s, uint32_t t_ms, hy_accel_t a);

#endif
