#ifndef HY_CABINET_H
#define HY_CABINET_H

#include <stdbool.h>
#include <stdint.h>
#include "hy/motion.h"
#include "hy/types.h"

#define HY_CAB_RATE_HZ          50
#define HY_CAB_MAX_N           200     /* 4 s */
#define HY_CAB_MIN_MS          300
#define HY_CAB_QUIET_MS        400
#define HY_CAB_QUIET_DDPS      100     /* 10 dps */
#define HY_CAB_QUIET_MG         40
#define HY_CAB_GYRO_START_MS    60     /* provisional; set from the board measurement */
#define HY_CAB_TEACH_TRIG_MG    40
#define HY_CAB_TRIG_MIN_MG      40
#define HY_CAB_TRIG_MAX_MG     250
#define HY_CAB_ANY_HOLDOFF_MS 2000
#define HY_CAB_STILL_MS       1000
#define HY_CAB_STILL_STEP_MG    30     /* hy_cab_still_feed: a step of this size or more restarts the still period */

typedef struct { int16_t a[3]; int16_t g[3]; } hy_imu6_t;   /* mg; deci-dps; device frame of HY_ROT_0 */
typedef struct { hy_imu6_t s[HY_CAB_MAX_N]; uint16_t n; bool has_gyro; hy_accel_t rest; } hy_cab_seq_t;
typedef struct { float gv, gh, av, ah; } hy_cab_feat_t;      /* dps, dps, mg, mg */

/* One feature row per sample. With g_hat the unit vector of q->rest, a the accel and w the gyro:
   gv = w . g_hat (signed rotation rate about the vertical), gh = |w - gv g_hat|, av = a . g_hat - |rest|,
   ah = |a - (a . g_hat) g_hat|. They depend only on the angle between each vector and gravity, so turning the
   device about the vertical leaves them unchanged. Gyro rows before HY_CAB_GYRO_START_MS and all gyro rows when
   !has_gyro are 0. A rest vector shorter than 1 mg gives all zero rows. out has q->n rows. */
void     hy_cab_features(const hy_cab_seq_t *q, hy_cab_feat_t *out);
uint16_t hy_cab_wom_mg(uint8_t sens);                        /* 250, 160, 100, 64, 40; out of range: 100 */
/* The sensor's sample period in wake on motion: low-power accel at 21 Hz (47.6 ms) */
#define HY_CAB_WOM_PERIOD_MS    48
/* The sensor's wake-on-motion decision for one sample: true when any axis of a differs from prev, the sensor's
   previous sample (HY_CAB_WOM_PERIOD_MS earlier), by more than thr_mg. The QMI8658A compares the absolute slope
   between successive samples with the threshold, not the deviation from a reference, so a slow drift never fires. */
bool       hy_cab_wom_hit(hy_accel_t prev, hy_accel_t a, uint16_t thr_mg);
/* The sensor's previous sample seen from a HY_CAB_RATE_HZ stream: the value HY_CAB_WOM_PERIOD_MS before the
   current sample, interpolated linearly between back2 and back3 (the stream samples 2 and 3 steps earlier). */
hy_accel_t hy_cab_wom_prev(hy_accel_t back2, hy_accel_t back3);
/* hy_cab_wom_hit for sample i of q against hy_cab_wom_prev of its samples i - 2 and i - 3; before sample 0 the
   stream is at q->rest. */
bool       hy_cab_wom_hit_at(const hy_cab_seq_t *q, uint16_t i, uint16_t thr_mg);

typedef enum { HY_CAP_MORE = 0, HY_CAP_DONE, HY_CAP_TOO_SHORT, HY_CAP_TOO_LONG } hy_cap_res_t;
typedef struct { hy_cab_seq_t seq; uint32_t t0_ms, last_move_ms; } hy_cab_cap_t;
/* Starts a capture. The first sample fed is the trigger sample and counts as movement. Samples are expected at
   HY_CAB_RATE_HZ. After a result other than HY_CAP_MORE the capture is finished and must be started again. */
void         hy_cab_cap_start(hy_cab_cap_t *c, hy_accel_t rest, bool has_gyro, uint32_t t_ms);
/* Appends the sample; DONE after HY_CAB_QUIET_MS quiet (quiet tail trimmed), TOO_SHORT when the movement before
   the quiet tail lasted under HY_CAB_MIN_MS, TOO_LONG at HY_CAB_MAX_N samples. A sample is quiet when the gyro
   magnitude is under HY_CAB_QUIET_DDPS (ignored when !has_gyro and within HY_CAB_GYRO_START_MS of the start) and
   every accel axis is within HY_CAB_QUIET_MG of the rest vector. */
hy_cap_res_t hy_cab_cap_feed(hy_cab_cap_t *c, uint32_t t_ms, const hy_imu6_t *x);
/* Rest tracker for the stream path: true once HY_CAB_STILL_MS of samples with every step under
   HY_CAB_STILL_STEP_MG; *rest is the mean of the last 10. */
typedef struct { hy_accel_t last, sum_buf[10]; uint8_t idx, cnt; uint32_t still_since; bool has_last; } hy_cab_still_t;
void hy_cab_still_reset(hy_cab_still_t *st);
bool hy_cab_still_feed(hy_cab_still_t *st, uint32_t t_ms, hy_accel_t a, hy_accel_t *rest);

/* Taught movement matcher. */

#define HY_CAB_TAKES          3
#define HY_CAB_OPEN_BEGIN_N  10      /* 200 ms */
#define HY_CAB_THR_MIN     0.25f     /* provisional; set from the recordings */
#define HY_CAB_THR_MAX     2.0f
#define HY_CAB_ODD_RATIO   3.0f
typedef struct { hy_cab_seq_t take[HY_CAB_TAKES]; float d_self; uint16_t trig_mg; bool valid; } hy_cab_model_t;

/* DTW distance between a template and a capture over the four features, scaled (gv, gh by 1/50 dps; av, ah by
   1/100 mg), local cost the L1 sum, gyro channels left out where masked in either row or absent in either
   sequence. Open begin: the template's first sample may align with capture samples 0 to HY_CAB_OPEN_BEGIN_N.
   Open end: its last sample may align with any capture sample from index L/2 on (L the template length). Band:
   capture index j within i - r to i + HY_CAB_OPEN_BEGIN_N + r of template index i, r = max(10, L/4). Result:
   total cost / L (the open end starts at the last capture sample when that comes before L/2). INFINITY when a
   gate rejects: capture shorter than L/2 (no upper bound: open end absorbs later motion), or the capture's mean
   scaled energy over its first min(n, L) samples outside 1/3 to 3 times the template's. Uses static scratch:
   single caller at a time. */
float    hy_cab_dist(const hy_cab_seq_t *tmpl, const hy_cab_seq_t *cap);
/* Copies the takes into m and trims each to the first sample at which the live trigger fires (hy_cab_wom_hit_at
   at trig_mg; trig_mg = half the smallest peak the trigger sees over the takes, clamped to HY_CAB_TRIG_MIN_MG to
   HY_CAB_TRIG_MAX_MG). For each take, s = its smallest distance as a capture to the other two as templates;
   d_self = the largest s. A take whose s is above HY_CAB_ODD_RATIO times the mean of the other two s and above
   HY_CAB_THR_MIN is the outlier (the largest such s if several). HY_E_INVALID with *odd set to the outlier, or to
   the take with the largest s when d_self is not finite, or to an empty take; else HY_OK, *odd = -1, m->valid.
   On HY_E_INVALID for an outlier or a non-finite d_self, m still holds the trimmed takes, trig_mg and d_self with
   m->valid false: a caller that accepts the takes as they are (after the last redo) sets m->valid. With a
   non-finite d_self the threshold is HY_CAB_THR_MAX. */
hy_err_t hy_cab_model_build(hy_cab_model_t *m, const hy_cab_seq_t takes[HY_CAB_TAKES], int *odd);
/* clamp(k[sens] * d_self, HY_CAB_THR_MIN, HY_CAB_THR_MAX), k = 1.0, 1.3, 1.6, 2.0, 2.5 for sensitivity 1 to 5;
   out of range: 3. */
float    hy_cab_threshold(const hy_cab_model_t *m, uint8_t sens);
/* True when the smallest distance from the capture to the model's takes is at or under the threshold; *best (may
   be NULL) gets that distance. False with INFINITY for a model that is not valid. */
bool     hy_cab_match(const hy_cab_model_t *m, uint8_t sens, const hy_cab_seq_t *cap, float *best);

/* Template file and CSV. Little-endian file: 32-byte header (magic "HYCB", version 1, rate, takes, flags bit 0
   has_gyro, trigger mg u16, gyro start ms u16, d_self f32, created i64, 8 reserved bytes), then per take n u16,
   the rest vector 3 x i16 and n x 6 x i16 (ax, ay, az mg; gx, gy, gz deci-dps), then the CRC-32 (IEEE) of
   everything before it. */
#define HY_CAB_FILE_MAX 7260   /* 32 + 3 x (8 + 200 x 12) + 4 */
/* Bytes written, or 0 when max is too small, the model is not valid or a take has no samples or too many. */
size_t   hy_cab_encode(const hy_cab_model_t *m, int64_t created, uint8_t *buf, size_t max);
/* HY_E_CORRUPT on bad magic, length or CRC; HY_E_VERSION on another version. On success m is valid, each take's
   has_gyro comes from the file's flag, and *created (may be NULL) is set. On failure m->valid is false. */
hy_err_t hy_cab_decode(const uint8_t *buf, size_t len, hy_cab_model_t *m, int64_t *created);
/* One CSV block for a sequence: rows "kind,take,t_ms,ax,ay,az,gx,gy,gz\n" (no header), t_ms from the first
   sample. Returns the bytes written (the text is also NUL-terminated), 0 if max is too short. */
size_t   hy_cab_csv_rows(const hy_cab_seq_t *q, const char *kind, int take, char *buf, size_t max);
#define  HY_CAB_CSV_HEADER "kind,take,t_ms,ax,ay,az,gx,gy,gz\n"

#endif
