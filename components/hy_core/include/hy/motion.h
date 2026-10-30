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

#endif
