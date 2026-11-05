/* Between the motion task (motion_run.c) and the cabinet open runner (cabinet_run.c). Private to hy_motion. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hy/motion.h"

/* motion_run.c, any task */
bool hy_motion_int_on(void);        /* hy_motion_enable(true) is in force (and no debug hold) */
bool hy_motion_int_off_end(void);   /* screen-off work must stop: the screen came on or a debug hold began */
void hy_motion_int_wake(void);      /* notifies the motion task */
void hy_motion_int_wake_isr(void);  /* the same from an ISR (IRAM) */

/* cabinet_run.c. Any task: */
bool hy_cab_int_rot_locked(void);   /* teach or test: the orientation state is not fed */
/* Motion task only: */
/* Screen off, at every pass. Ends teach and test, drops the stream state. */
void hy_cab_int_screen_off(void);
/* Screen off: true when the runner takes over the IMU (the feature is live). */
bool hy_cab_int_sleep_live(void);
/* Screen off and live: arms wake on motion if needed, blocks on the task notification (no timeout, or a holdoff
   or retry time), handles an interrupt (Any: reaction check and holdoff; Taught: capture, match, still wait).
   Returns after one notification or timeout; the motion task re-reads its state and calls again. */
void hy_cab_int_sleep_step(void);
/* Screen off and not live: leaves wake on motion (IMU powered down) if the runner had armed it. */
void hy_cab_int_sleep_end(void);
/* true while the runner holds the IMU in the screen-off path (armed or between steps of a capture) */
bool hy_cab_int_imu_held(void);
/* Screen on, before the motion task powers the IMU up for the stream: the screen-off state is dropped. */
void hy_cab_int_screen_on(void);
/* Once per screen wake: a paused (faulted) feature is tried again, within the retry limits. */
void hy_cab_int_wake(void);
/* Stream path, before each read: true when the IMU is in the capture mode (read accel and gyro). */
bool hy_cab_int_stream_gyro(void);
/* Stream path, before each read: true once when a mode switch failed and the motion task must power the IMU up
   again (as after the screen came on). */
bool hy_cab_int_stream_repower(void);
/* Stream path, after each good read. g is NULL for an accel-only read. suspended: the shake detector is armed. */
void hy_cab_int_stream_sample(uint32_t t_ms, hy_accel_t a, const int16_t *g, bool suspended);
/* Stream path, a failed read */
void hy_cab_int_stream_error(esp_err_t err);
