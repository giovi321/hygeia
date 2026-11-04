#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hy/cabinet.h"
#include "hy/settings.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cabinet open runner. The work runs inside the motion task, the only owner of the IMU; the setters below are
   callable from any task. The feature is live when cab_on, at least one of cab_chime and cab_light, and, in Taught
   mode, a stored template, and no IMU fault since the last config change. Not live: the IMU, the sleep mode and
   the wake set behave as without the feature. The runner never logs, skips, snoozes or undoes a dose: its only
   outputs are the events below and hy_power_motion_wake. */

#define HY_CAB_REACT_COOLDOWN_S 120     /* at most one reaction in this time */
#define HY_CAB_TEST_MAX_S       600     /* test mode ends by itself */
#define HY_CAB_RECENT_N         8       /* recent captures kept in PSRAM for the download */

typedef enum { HY_CABEV_REACT = 0, HY_CABEV_MATCH, HY_CABEV_OTHER, HY_CABEV_TOO_SMALL, HY_CABEV_TOO_LONG,
               HY_CABEV_TRIGGER, HY_CABEV_TEACH_STILL, HY_CABEV_TEACH_GO, HY_CABEV_TEACH_TAKE, HY_CABEV_TEACH_ODD,
               HY_CABEV_TEACH_DONE, HY_CABEV_TEACH_FAIL, HY_CABEV_FAULT,
               HY_CABEV_TEST_END /* the runner ended a test: time, screen off or Forget */ } hy_cabev_kind_t;
/* Why a teach failed: the take field of a HY_CABEV_TEACH_FAIL event */
typedef enum { HY_CAB_FAIL_STILL = 0,   /* not still for 1 s within 10 s ("Hold still") */
               HY_CAB_FAIL_NO_MOVE,     /* no movement within 60 s after GO */
               HY_CAB_FAIL_MODEL,       /* the takes give no usable template (distance not finite) */
               HY_CAB_FAIL_SAVE,        /* the template file could not be written */
               HY_CAB_FAIL_ABORT,       /* the screen went off, the shake toast showed, or the IMU failed */
} hy_cab_fail_t;
/* take: TEACH_STILL, GO, TAKE, ODD: the take 1 to 3 it concerns; TEACH_FAIL: hy_cab_fail_t. dist, thr: MATCH,
   OTHER and REACT after a Taught capture (dist INFINITY when a gate rejected it); 0 otherwise. */
typedef struct { hy_cabev_kind_t kind; uint8_t take; float dist, thr; } hy_cabev_t;
/* event (may be NULL) is called in the LVGL task through lv_async_call. It must re-check the reaction condition
   before acting on HY_CABEV_REACT. */
typedef struct { void (*event)(const hy_cabev_t *e); } hy_cab_ui_t;
/* Loads the template file (/data/cabinet.bin) and installs the IMU interrupt hook. After the store is open and
   before hy_motion_init. A file that cannot be read leaves the runner "not taught" and the file in place. */
esp_err_t hy_cab_run_init(const hy_cab_ui_t *ui);
/* Copies the cabinet fields; a change clears an IMU fault and re-arms as needed. */
void      hy_cab_run_config(const hy_settings_t *s);
/* start: needs the screen on (ESP_ERR_INVALID_STATE otherwise); cancels test mode, locks the rotation and runs the
   teach flow on the 50 Hz stream. A new teach replaces the stored template only when it completes. !start
   cancels a teach in progress. */
esp_err_t hy_cab_run_teach(bool start);
/* on: needs the screen on, no teach in progress and, in Taught mode, a template (ESP_ERR_INVALID_STATE). Counts
   instead of reacting, locks the rotation, ends after HY_CAB_TEST_MAX_S or when the screen goes off. */
esp_err_t hy_cab_run_test(bool on);
/* Deletes the template file; Taught mode then acts as off. Cancels test mode in Taught mode. */
esp_err_t hy_cab_run_forget(void);
void      hy_cab_run_status(hy_cab_status_t *out);
/* Copies the template takes (model_out->valid false when not taught) and up to max_recent recent captures, oldest
   first, under the runner mutex. Returns the number of recent captures copied. Either pointer may be NULL. */
size_t    hy_cab_run_snapshot(hy_cab_model_t *model_out, hy_cab_recent_t *recent_out, size_t max_recent);

#if CONFIG_HY_DEBUG_CONSOLE
/* Console task. "teach", "teach stop", "test on|off", "forget"; "" prints the usage. */
void hy_cab_run_debug(const char *args);
/* Console task. Prints hy_cab_status_t. */
void hy_cab_run_debug_stat(void);
#endif

#ifdef __cplusplus
}
#endif
