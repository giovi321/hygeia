#ifndef HY_CABINET_H
#define HY_CABINET_H

#include <stdbool.h>
#include <stdint.h>
#include "hy/motion.h"

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
/* Emulates the sensor's wake-on-motion decision for one accel sample, per the rule recorded from the datasheet
   (initially: any axis differs from rest by more than thr_mg). */
bool     hy_cab_wom_hit(hy_accel_t rest, hy_accel_t a, uint16_t thr_mg);

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

#endif
