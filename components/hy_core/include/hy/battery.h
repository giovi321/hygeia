#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hy/power.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HY_BATT_LOW_PCT       15
#define HY_BATT_CRIT_PCT      5
#define HY_BATT_LOW_REARM     18 /* low re-arms at >= 18% or when USB is present */
#define HY_BATT_CRIT_REARM    8
#define HY_BATT_EST_MIN_S     (3 * 3600)
#define HY_BATT_EST_MIN_DROP  5

typedef struct {
    hy_batt_raw_t raw;  /* last reading */
    int8_t pct;         /* shown percent, -1 unknown */
    bool low_armed, crit_armed;
    int64_t est_t0;     /* discharge anchor, 0 = none */
    int8_t est_p0;
    int32_t rate_cph;   /* moving average discharge rate, centi-percent per hour; 0 = unknown */
} hy_batt_t;

enum { HY_BATT_CHANGED = 1u << 0, HY_BATT_EV_LOW = 1u << 1, HY_BATT_EV_CRIT = 1u << 2 };

void hy_batt_init(hy_batt_t *b, int32_t saved_rate_cph); /* both armed; rate from NVS */
/* One reading at now (UTC s). Returns the flags above. */
uint32_t hy_batt_update(hy_batt_t *b, const hy_batt_raw_t *r, int64_t now);
/* Top-bar text "78%"; the caller supplies the text for an unknown percent. *red when 0 <= pct < 15;
 * *bolt when raw.chg == CHARGING. */
void hy_batt_text(const hy_batt_t *b, char *buf, size_t len, const char *unknown, bool *red, bool *bolt);
/* Estimate text "~2 d" (>= 48 h, days rounded half up), "~9 h" (>= 1 h, hours rounded half up), "<1 h";
 * false (hidden) when rate unknown, USB present, or pct unknown. */
bool hy_batt_estimate(const hy_batt_t *b, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
