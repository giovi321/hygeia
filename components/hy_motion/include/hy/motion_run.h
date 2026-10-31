#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "hy/motion.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Callbacks into the UI. Each is called in the LVGL task through lv_async_call, never from the motion task. A callback
   posted before the screen went off is dropped. shake may be NULL. */
typedef struct { void (*rotate)(hy_rot_t r); void (*shake)(void); } hy_motion_ui_t;

/* Creates the motion task (priority 4, below the LVGL task), IMU off, task blocked. cur is the applied orientation. */
esp_err_t hy_motion_init(const hy_motion_ui_t *ui, hy_rot_t cur);
/* Any task. on: the task powers the IMU up, resumes the orientation state and samples every 20 ms. A call while already
   on changes nothing. off: the task powers the IMU down (CTRL7 = 0) and blocks on its notification with no timeout. */
void hy_motion_enable(bool on);
/* Any task. locked: samples are not fed to the orientation state. */
void hy_motion_set_lock(bool locked);
/* Any task. After a forced or locked rotation: the orientation state takes r and loses its short wake dwell. */
void hy_motion_set_current(hy_rot_t r);
/* Any task. on: the shake detector restarts and samples are fed to it. off: they are not. */
void hy_motion_shake_arm(bool on);

#if CONFIG_HY_DEBUG_CONSOLE
/* Console task. "" logs the state; "arm" arms the detector for 30 s and only logs detections. */
void hy_motion_debug(const char *args);
/* Console task. Prints samples from the motion task's own reads: samples 10 for `imu` and `imu raw` (raw: sensor
   frame), rec: raw lines `imu,<ms>,x,y,z`. Prints "imu: screen off" and touches nothing while motion is disabled. */
void hy_motion_imu(bool raw, bool rec, int samples);
#endif

#ifdef __cplusplus
}
#endif
