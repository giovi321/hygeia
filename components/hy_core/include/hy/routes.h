#ifndef HY_ROUTES_H
#define HY_ROUTES_H

#include <stddef.h>
#include <stdint.h>
#include "hy/auth.h"

typedef enum {
    HY_R_INFO = 0, HY_R_STATE, HY_R_PAIR_START, HY_R_PAIR, HY_R_BROWSERS, HY_R_BROWSERS_DEL,
    HY_R_MEDS, HY_R_MED_NEW, HY_R_MED, HY_R_MED_PUT, HY_R_MED_DEL, HY_R_MED_ARCHIVE, HY_R_MED_UNARCHIVE,
    HY_R_MED_ORDER, HY_R_MED_PILLS, HY_R_MED_LOG, HY_R_MED_SKIP, HY_R_MED_SNOOZE,
    HY_R_LOG, HY_R_LOG_NEW, HY_R_LOG_PUT, HY_R_LOG_DEL, HY_R_LOG_UNDO, HY_R_HISTORY, HY_R_SCHEDULE,
    HY_R_EXPORT_CSV, HY_R_EXPORT_JSON, HY_R_IMPORT, HY_R_SETTINGS, HY_R_SETTINGS_PUT, HY_R_TIME,
    HY_R_WIFI, HY_R_WIFI_MODE, HY_R_WIFI_NET_ADD, HY_R_WIFI_NET_DEL, HY_R_WIFI_AP, HY_R_CHIME_TEST,
    HY_R_POWER_OFF, HY_R_OTA, HY_R_UPDATE, HY_R_UPDATE_CHECK, HY_R_UPDATE_INSTALL,
    HY_R_CABINET, HY_R_CABINET_TEST, HY_R_CABINET_SAMPLES, HY_R_CABINET_DEL, HY_R_COUNT
} hy_route_id_t;

#define HY_RF_OPEN 0x01 /* no session needed */

typedef struct {
    hy_method_t m;
    const char *pattern;
    hy_route_id_t id;
    uint8_t flags;
    const char *screen; /* where the screen does the same, "" = web only */
} hy_route_t;

/* The whole table; *n receives the number of rows. */
const hy_route_t *hy_routes(size_t *n);

/* Exact match after stripping the query ("?" and everything after). "{id}" stands for one path segment of
 * 1..4294967295 in decimal, no sign, no leading zero, written to *id (may be NULL). Deletes of Wi-Fi networks
 * name the SSID in the body, so there is no "{ssid}" segment. NULL when nothing matches. */
const hy_route_t *hy_route_match(hy_method_t m, const char *path, uint32_t *id);

#endif /* HY_ROUTES_H */
