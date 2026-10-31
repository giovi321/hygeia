#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hy/settings.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pairing over HTTP: POST /api/pair/start shows a 6-digit code for 120 s (a new start replaces a running code). A
 * start answers 429 "too_many" with Retry-After and keeps any running code when it comes within 10 s of the previous
 * start, within 60 s of a Cancel on the device or of a code that expired unused, or as the fourth start within 10
 * minutes, so a browser on the network cannot keep the code on the screen. While 5 wrong codes block pairing (60 s) a
 * start answers 429 "blocked". While a due card or the battery power-off countdown shows, a start answers 409 "busy"
 * and no code runs. POST /api/pair {"code":".."} pairs the browser and sets its session cookie.
 *
 * Why the pairing overlay hides: a browser paired, the screen's Cancel, the code expired, the fifth wrong code blocked
 * pairing for a minute, or the right code arrived but the session could not be saved (the code is used up). */
typedef enum { HY_WEB_PAIR_DONE = 0, HY_WEB_PAIR_CANCEL, HY_WEB_PAIR_EXPIRED, HY_WEB_PAIR_BLOCKED,
               HY_WEB_PAIR_FAILED } hy_web_pair_end_t;

/* Screen side, registered by the UI. hy_web does not depend on the UI: every callback is called in the LVGL task
 * (through lv_async_call), and a NULL member is skipped. pair_show: a code runs until expires_mono (monotonic
 * seconds, esp_timer); the overlay wakes the screen. The code pointer is valid only during the call (the buffer is
 * wiped afterwards): the overlay copies it. pair_hide: reason is a hy_web_pair_end_t. */
typedef struct {
    void (*pair_show)(const char *code, int64_t expires_mono);
    /* True while the overlay must not cover the screen (a due card or the power-off countdown shows). */
    bool (*pair_busy)(void);
    void (*pair_hide)(int reason);
    void (*toast_ok)(const char *text);
    void (*toast_info)(const char *text);
    void (*settings_applied)(const hy_settings_t *old, const hy_settings_t *now);
    void (*power_off)(void);
    /* After a web time or zone set; runs in the LVGL task inside the request's job. Refresh what shows the clock. */
    void (*time_changed)(void);
} hy_web_ui_t;

/* The pointer is kept; the struct must outlive the server. Any task. */
void      hy_web_set_ui(const hy_web_ui_t *ui);

/* After hy_net_start: loads the paired browsers and follows the network state. The HTTP server runs in every network
 * state except OFF and PAUSED, and stops in those. */
esp_err_t hy_web_start(void);

/* LVGL task, from the UI. The pairing overlay's Cancel: the running code ends (the fail count is kept) and starts wait
 * 60 s; the overlay hides itself. */
void      hy_web_pair_cancel(void);
/* Settings' "Forget paired browsers": every session ends, open live-event sockets close. ESP_ERR_INVALID_STATE
 * before hy_web_start. */
esp_err_t hy_web_forget_all(void);
/* Number of paired browsers (0..4). Any task. */
size_t    hy_web_paired_count(void);

#if CONFIG_HY_DEBUG_CONSOLE
/* "web": server, sockets, sessions, the last 5 requests with route and status, live-event queue */
void hy_web_debug(const char *args);
/* "webjob slow <ms>": the next job sleeps that long in the LVGL task before it runs */
void hy_web_debug_job(const char *args);
/* "pair" with no argument: prints the running code (console only, never logged) */
void hy_web_debug_pair(void);
#endif

#ifdef __cplusplus
}
#endif
