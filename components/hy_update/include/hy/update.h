/* Firmware update from the GitHub releases of CONFIG_HY_UPDATE_REPO.
 *
 * Nothing here opens a connection on its own: a check or an install runs only when hy_update_check or
 * hy_update_install is called (a user's press). Each run is one task (8 KB internal stack, below the LVGL task's
 * priority) that is deleted when the run ends. The task calls no store, settings, scheduler or LVGL function; the UI
 * follows the status through hy_update_subscribe.
 *
 * Check: GET https://github.com/<repo>/releases/latest; the tag comes from the redirect's Location, the body is not
 * read. Install: SHA256SUMS of that tag, then hygeia.bin of that tag streamed through hy_fwimg (head check with the
 * tag as the expected version, SHA-256 compared with the SHA256SUMS line, esp_ota_end), then a restart through the
 * registered LVGL-task callback 2 s later. Any failure leaves the boot partition unchanged. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "hy/fwupdate.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HY_UPDATE_MAX_SUBS 3

/* After hy_net_init. Reads the installed version (esp_app_get_description) and sets installed_release. */
esp_err_t   hy_update_init(void);
/* LVGL task callback that restarts the device (registered by app_main, the same helper the web upload uses). It
   returns false when it could not post the restart; the update task then restarts by itself. */
void        hy_update_set_restart(bool (*restart)(void));
/* Any task. Starts a check: HY_FWE_NONE when the task started. Immediate refusals, also stored in the status as
   FAILED (except while a check or install of this module runs, whose status is kept): HY_FWE_NO_NETWORK (hy_net state
   is not HY_NS_CONNECTED), HY_FWE_BUSY (a check or install runs, or the heavy slot is held), HY_FWE_MEMORY (internal
   heap pre-check, or the task could not be created). */
hy_fw_err_t hy_update_check(void);
/* Any task. version must equal status.latest, state AVAILABLE, check finished less than 600 s ago, else
   HY_FWE_STALE. Then, in order: NO_NETWORK, SELF_PENDING, BATTERY, BUSY, MEMORY as for a check. HY_FWE_NONE: the
   download started. */
hy_fw_err_t hy_update_install(const char *version);
void        hy_update_status(hy_fw_status_t *out);           /* copy under the lock */
/* Called in the update task on every state change and on each 5 % step of the download; must not block. Immediate
   refusals do not call it (the caller has the result). Max HY_UPDATE_MAX_SUBS; returns -1 when full. */
int         hy_update_subscribe(void (*cb)(const hy_fw_status_t *st, void *ctx), void *ctx);

#if CONFIG_HY_DEBUG_CONSOLE
/* "upd": status, internal free and minimum, last run's stack high-water mark and duration;
   "upd check", "upd install"; "upd fakesum on|off": flips one byte of the expected hash before the compare;
   "upd batt <pct>|off": a fake battery percent for the install rule (on battery) */
void        hy_update_debug(const char *args);
#endif

#ifdef __cplusplus
}
#endif
