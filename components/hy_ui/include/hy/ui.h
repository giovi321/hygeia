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
 * The boot splash (about 1 s). Call right after hy_board_init. hy_ui_start builds the content, waits until the
 * splash has run 1000 ms and fades the content in.
 */
esp_err_t hy_ui_splash_start(void);

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
void hy_ui_debug_idle(const char *arg);   /* "": enters the idle clock at once; "off": wakes it */
void hy_ui_debug_tapstroke(const char *code); /* a still stroke through the input layer, then that tile's click handler */
void hy_ui_debug_history(const char *arg); /* "toggle": 7/30 days; "tap|dbl|hold [code]": a row tap, double tap or tap + held finger through the input layer; "perf": timings */
void hy_ui_debug_detail(const char *arg); /* "<code>": opens that medicine's detail, as a long press does */
void hy_ui_debug_note(const char *arg);   /* "<code> [text]": the note view; with a text, logs a taken dose now with it */
void hy_ui_debug_earlier(const char *arg); /* "<code> [hh:mm]": the log-earlier view; with a time, logs a taken dose then */
void hy_ui_debug_due(const char *arg);    /* "slide|skip|snooze|next": the due card's actions, as the gestures call them; "flash": the speaker-failure flash */
void hy_ui_debug_qs(const char *arg);     /* "sound|dnd|bright <n>|time <y> <m> <d> <h> <mi>": tile taps and slider release; "view <name>": pushes qs, settings, zone, time, sound, display, about or invalid; "" opens Quick settings */
void hy_ui_debug_settz(const char *iana); /* the zone picker's Set with that IANA name */
void hy_ui_debug_settings(void);          /* logs the settings in the RAM copy (what NVS holds after a reboot) */
void hy_ui_debug_poweroff(const char *arg); /* "show|cancel|slide|crit": the power-off confirmation, its slide (powers off) and the critical countdown */
void hy_ui_debug_gaps(void);              /* the stored untracked gaps of the last 30 days */
void hy_ui_debug_scr(const char *arg);    /* "": screen state and timers; "off": OFF now; "nousb": run as on battery; "usb": back */
void hy_ui_debug_mf(const char *arg);     /* the medicine form: "new|edit <code>|name <text>|dose [text]|code <xy>|color <n>|type fixed|asneeded|time add HH:MM|time del <idx>|interval <min>|next|back|save|state" */
#endif

#ifdef __cplusplus
}
#endif
