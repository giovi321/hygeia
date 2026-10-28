#pragma once

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

#if CONFIG_HY_DEBUG_CONSOLE
/*
 * Serial debug entry points. Each one queues its work to the LVGL task (the only task that may
 * call hy_act_*) and returns at once; results are logged from there.
 */
void hy_ui_debug_tap(const char *code);   /* same handler as tapping the tile of that medicine code */
void hy_ui_debug_undo(void);              /* same handler as tapping Undo on the visible toast */
void hy_ui_debug_logs(void);              /* today's and yesterday's log entries */
void hy_ui_debug_seed4(const char *arg);  /* "": adds the example medicine Z (Zinc); "5": also C; "off": archives both */
void hy_ui_debug_shot(void);              /* streams a PNG-convertible snapshot of the active screen */
#endif

#ifdef __cplusplus
}
#endif
