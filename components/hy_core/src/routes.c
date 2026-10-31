#include <string.h>
#include "hy/routes.h"

/* Rows are listed in the order of the route enum. */
static const hy_route_t TABLE[] = {
    { HY_M_GET,    "/api/info",                       HY_R_INFO,          HY_RF_OPEN, "Settings > About" },
    { HY_M_GET,    "/api/state",                      HY_R_STATE,         0, "Home tile" },
    { HY_M_POST,   "/api/pair/start",                 HY_R_PAIR_START,    HY_RF_OPEN, "Pairing code overlay" },
    { HY_M_POST,   "/api/pair",                       HY_R_PAIR,          HY_RF_OPEN, "" },
    { HY_M_GET,    "/api/browsers",                   HY_R_BROWSERS,      0, "Settings > Wi-Fi" },
    { HY_M_DELETE, "/api/browsers",                   HY_R_BROWSERS_DEL,  0, "Settings > Wi-Fi (Forget all)" },
    { HY_M_GET,    "/api/medicines",                  HY_R_MEDS,          0, "Home" },
    { HY_M_POST,   "/api/medicines",                  HY_R_MED_NEW,       0, "Medicine form" },
    { HY_M_GET,    "/api/medicines/{id}",             HY_R_MED,           0, "Medicine detail" },
    { HY_M_PUT,    "/api/medicines/{id}",             HY_R_MED_PUT,       0, "Medicine form" },
    { HY_M_DELETE, "/api/medicines/{id}",             HY_R_MED_DEL,       0, "" },
    { HY_M_POST,   "/api/medicines/{id}/archive",     HY_R_MED_ARCHIVE,   0, "Medicine detail" },
    { HY_M_POST,   "/api/medicines/{id}/unarchive",   HY_R_MED_UNARCHIVE, 0, "" },
    { HY_M_PUT,    "/api/medicines/order",            HY_R_MED_ORDER,     0, "" },
    { HY_M_POST,   "/api/medicines/{id}/pills",       HY_R_MED_PILLS,     0, "" },
    { HY_M_POST,   "/api/medicines/{id}/log",         HY_R_MED_LOG,       0, "Home tile, due card, Medicine detail" },
    { HY_M_POST,   "/api/medicines/{id}/skip",        HY_R_MED_SKIP,      0, "Due card" },
    { HY_M_POST,   "/api/medicines/{id}/snooze",      HY_R_MED_SNOOZE,    0, "Due card" },
    { HY_M_GET,    "/api/log",                        HY_R_LOG,           0, "Medicine detail, History" },
    { HY_M_POST,   "/api/log",                        HY_R_LOG_NEW,       0, "Medicine detail > Log earlier" },
    { HY_M_PUT,    "/api/log/{id}",                   HY_R_LOG_PUT,       0, "" },
    { HY_M_DELETE, "/api/log/{id}",                   HY_R_LOG_DEL,       0, "" },
    { HY_M_POST,   "/api/log/{id}/undo",              HY_R_LOG_UNDO,      0, "Logged toast" },
    { HY_M_GET,    "/api/history",                    HY_R_HISTORY,       0, "History" },
    { HY_M_GET,    "/api/schedule",                   HY_R_SCHEDULE,      0, "Overview" },
    { HY_M_GET,    "/api/export.csv",                 HY_R_EXPORT_CSV,    0, "" },
    { HY_M_GET,    "/api/export.json",                HY_R_EXPORT_JSON,   0, "" },
    { HY_M_POST,   "/api/import",                     HY_R_IMPORT,        0, "" },
    { HY_M_GET,    "/api/settings",                   HY_R_SETTINGS,      0, "Settings, quick settings" },
    { HY_M_PUT,    "/api/settings",                   HY_R_SETTINGS_PUT,  0, "Settings, quick settings" },
    { HY_M_POST,   "/api/time",                       HY_R_TIME,          0, "Settings > Time zone, Date and time" },
    { HY_M_GET,    "/api/wifi",                       HY_R_WIFI,          0, "Settings > Wi-Fi" },
    { HY_M_POST,   "/api/wifi/mode",                  HY_R_WIFI_MODE,     0, "Quick settings, Settings > Wi-Fi" },
    { HY_M_POST,   "/api/wifi/networks",              HY_R_WIFI_NET_ADD,  0, "" },
    { HY_M_DELETE, "/api/wifi/networks",              HY_R_WIFI_NET_DEL,  0, "" },
    { HY_M_PUT,    "/api/wifi/ap",                    HY_R_WIFI_AP,       0, "" },
    { HY_M_POST,   "/api/chime/test",                 HY_R_CHIME_TEST,    0, "Settings > Sound" },
    { HY_M_POST,   "/api/power/off",                  HY_R_POWER_OFF,     0, "Power off screen" },
    { HY_M_POST,   "/api/ota",                        HY_R_OTA,           0, "" },
};

_Static_assert(sizeof TABLE / sizeof TABLE[0] == HY_R_COUNT, "one row per route id");

const hy_route_t *hy_routes(size_t *n)
{
    if (n) *n = sizeof TABLE / sizeof TABLE[0];
    return TABLE;
}

/* The segment [s, s + len) as an id: 1..4294967295, digits only, no leading zero. */
static bool parse_id(const char *s, size_t len, uint32_t *out)
{
    if (len == 0 || len > 10 || s[0] == '0') return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    if (v == 0 || v > 4294967295ULL) return false;
    *out = (uint32_t)v;
    return true;
}

static bool pattern_match(const char *pat, const char *path, size_t plen, uint32_t *id)
{
    size_t i = 0;
    while (*pat) {
        if (strncmp(pat, "{id}", 4) == 0) {
            size_t start = i;
            while (i < plen && path[i] != '/') i++;
            if (!parse_id(path + start, i - start, id)) return false;
            pat += 4;
        } else {
            if (i >= plen || path[i] != *pat) return false;
            i++;
            pat++;
        }
    }
    return i == plen;
}

const hy_route_t *hy_route_match(hy_method_t m, const char *path, uint32_t *id)
{
    if (!path || path[0] != '/') return NULL;
    size_t plen = strcspn(path, "?");
    uint32_t v = 0;
    for (size_t k = 0; k < sizeof TABLE / sizeof TABLE[0]; k++) {
        if (TABLE[k].m != m) continue;
        if (pattern_match(TABLE[k].pattern, path, plen, &v)) {
            if (id) *id = v;
            return &TABLE[k];
        }
    }
    return NULL;
}
