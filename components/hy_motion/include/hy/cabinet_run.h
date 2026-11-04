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

/* The event kinds, hy_cab_fail_t, hy_cab_check_t and hy_cabev_t are in hy/cabinet.h (hy_core). */
/* event (may be NULL) is called in the LVGL task through lv_async_call. It must re-check the reaction condition
   before acting on HY_CABEV_REACT. */
typedef struct { void (*event)(const hy_cabev_t *e); } hy_cab_ui_t;
/* Loads the template file (/data/cabinet.bin) and installs the IMU interrupt hook. After the store is open and
   before hy_motion_init. A file that cannot be read leaves the runner "not taught" and the file in place. */
esp_err_t hy_cab_run_init(const hy_cab_ui_t *ui);
/* Copies the cabinet fields; a change clears an IMU fault and re-arms as needed. */
void      hy_cab_run_config(const hy_settings_t *s);
/* start: needs the screen on (ESP_ERR_INVALID_STATE otherwise); cancels test mode, locks the rotation and runs the
   teach flow on the 50 Hz stream (hy/cabteach.h: lead-in, HY_CAB_TAKES takes, build, save). A new teach replaces
   the stored template only when it completes. !start cancels a teach in progress. */
esp_err_t hy_cab_run_teach(bool start);
/* start: one take of the taught movement matched against the saved template, with the same cues as a teach
   (HY_CABEV_TEACH_READY, TEACH_STILL, TEACH_GO, TOO_SMALL, TOO_LONG), ended by HY_CABEV_CHECK_END. Needs a taught
   template, no fault, no teach, no test and the screen on (ESP_ERR_INVALID_STATE otherwise; the decision and the
   state change are one step). Locks the rotation and holds the screen on. !start cancels a check in progress. */
esp_err_t hy_cab_run_check(bool start);
/* on: needs the screen on, no teach or check in progress and, in Taught mode, a template (ESP_ERR_INVALID_STATE). Counts
   instead of reacting, locks the rotation, ends after HY_CAB_TEST_MAX_S or when the screen goes off. */
esp_err_t hy_cab_run_test(bool on);
/* Deletes the template file; Taught mode then acts as off. Cancels test mode in Taught mode. */
esp_err_t hy_cab_run_forget(void);
/* hy_cab_run_forget, then the trigger is set back to Any movement when it was Taught movement (settings saved,
   runner configured). One path for the device and the web UI; call it from the LVGL task (it writes the settings).
   Returns the forget result; a settings save failure is only logged. */
esp_err_t hy_cab_run_forget_apply(void);
void      hy_cab_run_status(hy_cab_status_t *out);
/* Copies the template takes (model_out->valid false when not taught) and up to max_recent recent captures, oldest
   first, under the runner mutex. Returns the number of recent captures copied. Either pointer may be NULL. */
size_t    hy_cab_run_snapshot(hy_cab_model_t *model_out, hy_cab_recent_t *recent_out, size_t max_recent);

#if CONFIG_HY_DEBUG_CONSOLE
/* Console task. "teach", "teach stop", "check", "check stop", "test on|off", "forget"; "" prints the usage. */
void hy_cab_run_debug(const char *args);
/* Console task. Prints hy_cab_status_t. */
void hy_cab_run_debug_stat(void);
#endif

#ifdef __cplusplus
}
#endif
