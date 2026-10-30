#ifndef HY_TYPES_H
#define HY_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Placement of large static buffers that no ISR, DMA or cache-disabled code touches: the device build defines
 * it (hy_core CMakeLists.txt, public to every component that requires hy_core) to put them in PSRAM; empty on
 * the host. */
#ifndef HY_EXT_BSS
#define HY_EXT_BSS
#endif

typedef enum { HY_OK = 0, HY_E_INVALID, HY_E_CODE, HY_E_CODE_DUP, HY_E_NAME,
               HY_E_TIMES, HY_E_NOT_FOUND, HY_E_CLOCK, HY_E_UNDO_EXPIRED,
               HY_E_IO, HY_E_FULL, HY_W_TOO_SOON, HY_E_INTERVAL, HY_E_DOSE } hy_err_t;

/* Short name of an hy_err_t, for logs ("HY_E_IO"). "HY_E_?" for an unknown value. */
const char *hy_err_str(hy_err_t e);

typedef enum { HY_SCHED_FIXED = 0, HY_SCHED_AS_NEEDED } hy_sched_type_t;
typedef enum { HY_TAKEN = 0, HY_SKIPPED, HY_MISSED } hy_status_t;
typedef enum { HY_SRC_SCREEN = 0, HY_SRC_WEB, HY_SRC_MQTT, HY_SRC_AUTO } hy_source_t;

#define HY_MAX_TIMES 8
#define HY_TZ_LEN 48

typedef struct {
    uint32_t id; char code[3]; char name[33]; char dose[17]; uint8_t color; /* 0..9 */
    hy_sched_type_t type; uint16_t times[HY_MAX_TIMES]; /* minutes after local midnight */
    uint8_t n_times; uint8_t weekdays; /* bit0 = Monday ... bit6 = Sunday */
    uint16_t min_interval_min, early_window_min, late_after_min, missed_after_min;
    bool pills_enabled; int16_t pills_left, pills_alert;
    int32_t active_from, active_until; /* local date as YYYYMMDD, 0 = unset */
    bool archived; uint16_t order;
} hy_med_t;

typedef struct {
    uint32_t id; int64_t ts; char tz[HY_TZ_LEN]; uint32_t med_id; hy_status_t status;
    int64_t slot; /* 0 = free log */ int32_t delta_min; char note[64];
    hy_source_t source; bool edited;
} hy_log_t;

typedef struct { int64_t start, end; } hy_gap_t;

typedef enum { HY_OCC_UPCOMING = 0, HY_OCC_DUE, HY_OCC_LATE, HY_OCC_MISSED,
               HY_OCC_TAKEN, HY_OCC_SKIPPED, HY_OCC_UNTRACKED } hy_occ_state_t;

void hy_med_defaults(hy_med_t *m);  /* weekdays 0x7F, windows 60/30/120, color 0 */

#endif /* HY_TYPES_H */
