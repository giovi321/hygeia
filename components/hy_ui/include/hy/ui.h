#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Creates the home screen, loads it and subscribes to the event bus. Call after the board,
 * clock, store and actions are up. All state changes happen in the LVGL task.
 */
esp_err_t hy_ui_start(void);

/*
 * Storage state for the root view. false when the storage partition cannot be mounted or the store cannot
 * be opened: the root shows "Storage unavailable". Call before hy_ui_start or from any task (applied by the
 * LVGL task within a second).
 */
void hy_ui_set_storage_ok(bool ok);

#if CONFIG_HY_DEBUG_CONSOLE
/*
 * Serial debug entry points. Each one queues its work to the LVGL task (the only task that may
 * call hy_act_*) and returns at once; results are logged from there.
 */
void hy_ui_debug_tap(const char *code);   /* same handler as tapping the tile of that medicine code */
void hy_ui_debug_undo(void);              /* same handler as tapping Undo on the visible toast */
void hy_ui_debug_logs(void);              /* today's and yesterday's log entries */
void hy_ui_debug_seed4(const char *arg);  /* "": adds Z (Zinc); "5": also C; "<n>": up to n active (Z1.., Y1..); "off": archives them */
void hy_ui_debug_shot(void);              /* streams a PNG-convertible snapshot of the active screen */
void hy_ui_debug_remindtest(const char *arg); /* "<minutes> [count] [miss2]": T (and U) due then; "off": archives both */
void hy_ui_debug_perf(void);              /* times one forced scheduler evaluation and one home_refresh */
void hy_ui_debug_gesture(const char *name); /* tap|long|left|right|up|down|edgedown|edgeup|edgeright -> ui_nav_gesture */
void hy_ui_debug_nav(void);               /* logs the navigation stack, e.g. "nav: HOME > QS" */
void hy_ui_debug_storefail(const char *arg); /* "": storage-down root; "off": storage ok again */
#endif

#ifdef __cplusplus
}
#endif
