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

#define HY_SHK_PEAK_MG     1300  /* high-passed value on one axis that counts as a peak */
#define HY_SHK_REARM_MG     600  /* the axis must fall below this before the next peak counts */
#define HY_SHK_PEAKS          4  /* alternating-sign peaks on the same axis */
#define HY_SHK_MIN_GAP_MS    60  /* closer peaks are a rebound, not a new peak */
#define HY_SHK_MAX_GAP_MS   400  /* a longer pause restarts the count */
#define HY_SHK_WINDOW_MS   1500  /* first to last counted peak */
#define HY_SHK_SETTLE_MS    200  /* after reset: samples only train the baseline */
#define HY_SHK_COOLDOWN_MS 1000  /* after a detection */

/* lat_axis is the axis that latched the last peak. */
typedef struct { int32_t base[3]; uint32_t start_ms, first_ms, last_ms, cool_until_ms; int8_t axis, sign, lat_axis;
                 uint8_t count; bool latched, primed; } hy_shake_t;

void hy_shake_reset(hy_shake_t *s, uint32_t now_ms);
/* Per axis: hp = a - base, then base += (a - base) / 16 (the first sample after reset sets base = a and hp = 0).
   A peak is the axis with the largest |hp| when |hp| >= HY_SHK_PEAK_MG and the detector is not latched; it latches
   until that axis' |hp| < HY_SHK_REARM_MG. A peak on the counting axis with the opposite sign, HY_SHK_MIN_GAP_MS to
   HY_SHK_MAX_GAP_MS after the last one, counts; a peak closer than HY_SHK_MIN_GAP_MS is ignored; anything else
   (same sign, other axis, longer gap, outside the window) starts a new count of 1 with this peak. The count
   reaching HY_SHK_PEAKS returns true once, then cooldown. */
bool hy_shake_feed(hy_shake_t *s, uint32_t t_ms, hy_accel_t a);

#endif
