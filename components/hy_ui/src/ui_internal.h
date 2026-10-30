#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "sdkconfig.h"
#include "hy/actions.h"
#include "hy/nav.h"
#include "hy/power_run.h"
#include "hy/screen.h"
#include "esp_err.h"
#include "strings.h"

/* colours (global constraints) */
#define HY_C_BG        0x000000
#define HY_C_SURFACE   0x121216
#define HY_C_SURFACE2  0x1A1A20
#define HY_C_INNER     0x0B0B0E   /* disc inside the ring */
#define HY_C_TEXT      0xEDEDF0
#define HY_C_TEXT2     0x9A9AA4
#define HY_C_AMBER     0xF6C453   /* Undo, warnings */
#define HY_C_OK        0x7CE38B   /* logged check */
#define HY_C_RED       0xF87171
#define HY_C_TRACK     0x22222A   /* ring track on new screens */
#define HY_C_MISS      0xE5484D   /* missed marks */
#define HY_C_NA        0x1C1C22   /* history: nothing scheduled */
#define HY_C_ON        0x24323A   /* quick-settings tile on */
#define HY_C_FILL      0x3A3A44   /* slider fill */
#define HY_C_DOT       0x3A3A42   /* page dot */
#define HY_C_GRAB      0x2E2E36   /* pull-down handle, untracked cells */
#define HY_C_PLUS_BG   0x0B0B0E

/* type scale: the only place fonts are chosen (theme.c) */
typedef struct {
    const lv_font_t *small;      /* 14 regular */
    const lv_font_t *status;     /* 17 regular */
    const lv_font_t *status_bold;/* 17 bold */
    const lv_font_t *body;       /* 22 regular */
    const lv_font_t *body_bold;  /* 22 bold */
    const lv_font_t *title;      /* 26 bold */
    const lv_font_t *time;       /* 30 bold */
    const lv_font_t *count;      /* 38 bold, digits and "/" */
    const lv_font_t *code_m;     /* 44 bold */
    const lv_font_t *code;       /* 52 bold */
    const lv_font_t *code_l;     /* 96 bold, A-Z 0-9 */
    const lv_font_t *clock;      /* 120 weight 200, 0-9 ":" "-" */
} hy_type_scale_t;
extern const hy_type_scale_t hy_type;

/* medicine palette, index 0..9 */
lv_color_t hy_med_color(uint8_t idx);

/* geometry (px) */
#define HY_SCREEN       480
#define HY_BAR_H        56
#define HY_CONTENT_H    (HY_SCREEN - HY_BAR_H)
#define HY_MARGIN       22
#define HY_GAP          16
#define HY_TILE_W       210
#define HY_TILE_H       180
#define HY_RING         116
#define HY_RING_W       11
#define HY_TOAST_W      432
#define HY_TOAST_H      88
#define HY_TOAST_Y      358
#define HY_TOO_SOON_WINDOW_MS (HY_UNDO_WINDOW_S * 1000)

/* ui.c */
lv_obj_t *ui_content(void);                      /* y 56..480 container hosting every view */
void      ui_log_hwm(const char *what);          /* logs the LVGL task stack margin once per `what` */
void      ui_fmt_hhmm(int64_t ts, const char *tz, char *out, size_t len); /* "HH:MM" of ts in tz */

/* ui.c: widget helpers shared by the pages */
lv_obj_t *ui_plain(lv_obj_t *parent);            /* unstyled, non-scrolling, non-clickable box */
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *text);
uint32_t  ui_fnv(uint32_t h, const void *p, size_t n); /* FNV-1a step over n bytes, seed 2166136261u */
size_t    ui_log_window(uint32_t med_id, int64_t from, int64_t to, hy_log_t *out, size_t cap); /* newest cap entries of [from, to), ascending */
const hy_med_t *ui_find_med(uint32_t id);        /* the live medicine list entry, NULL when unknown */
void      ui_refresh_all(void);                  /* scheduler tick, root flags, top bar and the shown pages */
void      ui_time_changed(void);                 /* after a time or zone set (no hy event): also the day-based caches */

/* widgets shared by quick settings, settings and the set-the-time screen */
typedef void (*ui_action_cb)(void *ud);
void      ui_on_tap(lv_obj_t *o, ui_action_cb cb, void *ud);   /* runs cb on a still, awake tap only */
lv_obj_t *ui_button(lv_obj_t *parent, int32_t w, int32_t h, const lv_font_t *f, const char *text, ui_action_cb cb, void *ud);
lv_obj_t *ui_view_title(lv_obj_t *view, const char *text);     /* 26 bold header at (24, 2) */
/* A 30 px slider. live runs on every change, commit on release; a stroke that only woke the screen changes nothing.
 * It consumes its strokes (no gesture). */
lv_obj_t *ui_slider(lv_obj_t *parent, int32_t w, int32_t min, int32_t max, void (*live)(int32_t), void (*commit)(int32_t));
lv_obj_t *ui_roller(lv_obj_t *parent, const char *opts, lv_roller_mode_t mode, int32_t w, const lv_font_t *sel_font,
                    lv_event_cb_t changed);

/* nav_host.c */
void      nav_host_init(void);                   /* creates the views in ui_content(), shows the root */
void      nav_host_warm_start(void);             /* after the splash: create the hidden views one at a time */
void      nav_host_warm_all(void);               /* create every hidden view now */
void      due_raise(void);                       /* the due card back above every view */
void      nav_host_tick(void);                   /* root flags from the clock and the storage state */
void      ui_show_view(hy_view_t v);             /* nav_host: hides others, runs the view's refresh */
hy_nav_fx_t ui_nav_gesture(hy_gesture_t g);      /* applies hy_nav_gesture and its effect; WAKE = swallowed */
hy_view_t ui_current_view(void);                  /* the view shown now */
bool      ui_nav_idle(void);                     /* the dimmed screen: the next stroke only wakes */
void      ui_nav_set_idle(bool idle);            /* the raw nav flag; idle.c owns it (the debug note wake test sets it alone) */
void      ui_nav_set_poweroff(bool on);          /* the power-off overlay or the countdown is up: nav rule 0 */
void      ui_nav_set_due(bool due);              /* the due card is up: nav rule 2 (it holds the screen on USB only, R1) */
void      ui_nav_push(hy_view_t v);
#if CONFIG_HY_DEBUG_CONSOLE
void      ui_nav_reset(void);                    /* debug: back to the root whatever overlay is showing */
#endif
void      ui_nav_pop(void);                      /* one level up, whatever overlay is showing */
void      ui_nav_home(void);                     /* back to the root; a pager root becomes HOME */
void      ui_nav_set_form_dirty(bool dirty);     /* medform.c: the draft has unsaved changes (nav rule 4b) */
lv_obj_t *ui_view_box(void);                     /* a hidden 480 x 424 view container in ui_content() */
void      ui_register_view(hy_view_t v, lv_obj_t *obj); /* the view nav_host shows for v */
void      ui_open_detail(uint32_t med_id);
void      ui_set_storage_down(bool down);        /* LVGL task: storage state for the root */
void      ui_qs_slide_done(void);                /* qs.c: the slide ended, nav_host hides the view under it */
void      ui_nav_log(void);                   /* "nav: HOME > QS" */
const char *ui_gesture_name(hy_gesture_t g);

/* input.c
 * The indev RELEASED callback (classification and navigation) runs before the pressed widget gets its own
 * RELEASED / SHORT_CLICKED / CLICKED. So a widget that owns a stroke (a slider, the due card knob, a scrolling
 * list) must call ui_input_consume() on LV_EVENT_PRESSED, LV_EVENT_PRESSING or LV_EVENT_SCROLL_BEGIN; from a
 * RELEASED or CLICKED handler it is too late and the gesture has already navigated. A stroke that only woke
 * the dimmed screen is neither a tap nor still, so no widget acts on it. */
void      input_init(lv_indev_t *indev);
bool      ui_input_last_was_tap(void);           /* the stroke that produced the current click classified as TAP */
bool      ui_input_last_was_double(void);        /* ... and it was the second tap of a double tap (300 ms, 40 px) */
bool      ui_input_still(void);                  /* the current or last stroke never left the tap slop */
bool      ui_input_waking(void);                 /* the current or last stroke started on the dimmed screen: it only wakes */
void      ui_input_consume_force(void);          /* sliders: consumed even from an edge zone (P45) */
void      ui_input_consume(void);                /* the current stroke belongs to a widget: no gesture */
#if CONFIG_HY_DEBUG_CONSOLE
void      input_debug_stroke(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t ms); /* press + release */
void      input_debug_swipe(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t ms, bool consume);
void      input_debug_hold(int16_t x0, int16_t y0);   /* press only */
void      input_debug_release(int16_t x1, int16_t y1); /* release a held stroke */
#endif

/* topbar.c */
void      topbar_create(void);                   /* on lv_layer_top, below every overlay */
void      topbar_refresh(void);                  /* time and the Wi-Fi off icon */
void      topbar_battery_refresh(void);          /* percent, red, bolt; also the idle line and quick settings; no LVGL work when unchanged */
/* Battery text for every place that shows it. *known false: pct holds the placeholder. est empty when hidden. */
void      ui_battery_text(char *pct, size_t pct_len, bool *red, bool *bolt, char *est, size_t est_len, bool *has_est,
                          bool *known);
void      idle_status_refresh(void);             /* idle.c: battery percent in the status line */
void      qs_battery_refresh(void);              /* qs.c: battery line */
void      topbar_set_visible(bool on);
void      topbar_set_grab(bool on);              /* pull-down handle, Home only */

/* pager.c */
lv_obj_t *pager_create(lv_obj_t *parent);        /* the pager root: three pages and the dots */
lv_obj_t *pager_page(int idx);                   /* 0 Home, 1 Overview, 2 History */
void      pager_show(int idx, bool animate);     /* 200 ms ease-out x offset */
int       pager_index(void);

/* overview.c, history.c: pager pages 1 and 2. Refresh runs from the snapshot only while the page is shown. */
void overview_create(lv_obj_t *page);
void overview_refresh(void);
void overview_refresh_if_changed(void);  /* generation changed and the page is shown */
void overview_timing(uint32_t *us);
void history_create(lv_obj_t *page);
void history_refresh(void);
void history_refresh_if_changed(void);
void history_show(void);                 /* the page became visible: refresh when something changed */
void history_hide(void);                 /* the page left the screen: cancel the pending row tap */
void history_invalidate(void);           /* a log, medicine or zone changed: recompute everything on the next refresh */
void history_toggle_range(void);         /* 7 <-> 30 days, redrawn from the LVGL loop */
void history_tap(uint32_t med_id);       /* a TAP ended on a row (med_id) or on the page (0) */
#if CONFIG_HY_DEBUG_CONSOLE
void history_debug_press(bool down);    /* debug: a finger held on / released from the page */
#endif
void history_timing(uint32_t *view_us, uint32_t *lvgl_us);

/* detail.c, note.c, earlier.c: the medicine detail and the two views above it. *_show runs from nav_host each time
 * the view becomes the top of the stack, with the medicine of the nav stack. */
#define HY_NOTE_MAX 63                           /* bytes of a log note, without the NUL */
void detail_create(void);
void detail_show(uint32_t med_id);
void detail_hide(void);                          /* left for a view outside detail, note and earlier */
void detail_invalidate(void);                    /* a log, medicine or zone changed: recompute on the next refresh */
void detail_refresh_if_changed(void);            /* generation changed and the detail is shown */
#if CONFIG_HY_DEBUG_CONSOLE
void detail_debug_scroll(void);                  /* scrolls the detail to its end */
#endif
/* kbd.c: the QWERTY keyboard of the note view, shared with the medicine form: maps, controls, 220 px height, Inter 26
   bold with the Montserrat fallback, no auto-repeat on enter and OK, the waking-stroke text restore and stroke
   consumption, and a one-shot shift key. Bound to `ta`; READY and CANCEL arrive on the text area. State is per keyboard. */
#define HY_KBD_H 220
lv_obj_t *ui_kbd_create(lv_obj_t *parent, lv_obj_t *ta);

void note_create(void);
void note_show(uint32_t med_id);
void note_submit(void);                          /* the Log button: log taken now with the note, back to detail */
void earlier_create(void);
void earlier_show(uint32_t med_id);
void earlier_submit(void);                       /* the Log at button: log taken at the chosen time, back to detail */
#if CONFIG_HY_DEBUG_CONSOLE
void note_debug_ok(void);                        /* the keyboard's OK key */
void note_debug_text(const char *text);          /* sets the note field as typing would */
void earlier_debug_set(int hour, int minute);    /* sets the rollers as turning them would */
#endif

/* medform.c: the step-by-step form that adds or edits a medicine on the device (hy_core hy/medform.h holds the draft
 * and the rules). The draft is static RAM, so it survives dimming, a due card and the power-off overlay. */
void medarchive_create(void);                   /* registers HY_V_MED_ARCHIVE */
void medarchive_show(uint32_t med_id);          /* nav_host: the view became the top, for the nav med_id */
void medarchive_confirm(void);                  /* the Archive button's action (also the debug "archive <code> yes") */
void medform_create(void);                       /* registers HY_V_MEDFORM */
void medform_open_new(void);                     /* hy_mf_new on hy_meds(), push HY_V_MEDFORM */
void medform_open_edit(uint32_t med_id);         /* hy_mf_edit; on error the info toast, no push */
void medform_show(void);                         /* nav_host: the view became the top; redraws the current page */
void medform_back(void);                         /* HY_FX_FORM_BACK: previous page, or "Discard changes?" */
#if CONFIG_HY_DEBUG_CONSOLE
void medform_debug(const char *arg);             /* "new|edit <code>|name <text>|...|state": see hy_ui_debug_mf */
#endif

/* qs.c, settings.c, timeset.c: quick settings, Settings and its sub-views, the set-the-time screen */
void qs_create(void);
void qs_show(void);                              /* the view became the top: values and the 200 ms slide */
void settings_create(void);                      /* Settings, Time zone, Sound, Display and About views */
void settings_show(hy_view_t v);                 /* one of those views became the top: refresh its values */
esp_err_t settings_set_zone(const char *iana);   /* the zone picker's Set; the debug settz calls it too */
void settings_brightness_live(int32_t pct);      /* live brightness, no save */
void settings_brightness_commit(int32_t pct);    /* saves the brightness setting */
void timeset_create(lv_obj_t *parent, bool invalid_mode);
void timeset_show(bool invalid_mode);            /* rollers to the current time (the build time while the clock is invalid) */
#if CONFIG_HY_DEBUG_CONSOLE
void qs_debug(const char *arg);                  /* "sound|dnd|bright <n>": the tile taps and the slider release */
void timeset_debug_set(int year, int month, int day, int hour, int minute); /* the Date and time rollers, then Set */
void settings_debug_log(void);                   /* logs the settings the RAM copy holds */
#endif

/* storage_down.c */
lv_obj_t *storage_down_create(lv_obj_t *parent);

/* home.c */
void ui_fmt_dur(int64_t min, char *buf, size_t len); /* "2 h 14 m" with no-break spaces, "<1 m" under a minute */
bool home_too_soon_hold(const hy_med_t *m, bool valid, int64_t now); /* R29: true = warned, do not log now */
const char *ui_err_text(hy_err_t err);   /* plan-1 error toast text for a failed action */
void home_create(lv_obj_t *page);
void home_rebuild(void);                 /* meds changed */
void home_refresh(bool animate);         /* tiles from the scheduler snapshot; LVGL only for changed ones */
void home_tap_med(uint32_t med_id);      /* logs a dose (debug `tap` calls it directly) */
void home_tile_click(uint32_t med_id);   /* a tile's SHORT_CLICKED: logs only when the stroke was a TAP */
bool home_med_id_by_code(const char *code, uint32_t *id);
void home_scroll_to(uint32_t med_id);           /* scrolls the grid so the tile is fully visible (same as home_show_med) */
void home_show_med(uint32_t med_id);     /* scroll that medicine's tile into view (now, or at the next rebuild) */
void home_refresh_if_changed(void);      /* home_refresh(true) when the snapshot generation changed */
void home_refresh_timing(uint32_t *view_us, uint32_t *lvgl_us); /* last home_refresh, for perf */

/* idle.c: the screen state machine (ON, DIM, OFF) and its views: DIM is the idle clock overlay on lv_layer_top, OFF
 * blanks the panel and asks the power task for the sleep mode. The nav idle flag follows "state is not ON". */
void idle_create(void);                          /* after topbar_create, before poweroff_create */
void screen_init(void);                          /* after poweroff_create: ON, USB from the PMU state */
void screen_event(hy_scr_ev_t ev);
hy_scr_t screen_state(void);
void screen_wake(const char *why);               /* an overlay that wakes the screen on USB and on battery */
void screen_tick(void);                          /* ui.c, every second: USB sync, TICK, the sleep request while OFF */
void screen_sched_evaluated(void);               /* after each scheduler evaluation: CHIME on a chime moment */
void screen_usb(bool in);                        /* hy_power_ui_t.usb: USB_IN / USB_OUT */
void screen_touch_wake(void);                    /* a touch woke the CPU from the sleep mode */
bool screen_wake_guard_take(void);               /* input.c at a press: a stroke right after screen_touch_wake only wakes */
void screen_activity(void);                      /* input.c: a stroke on an awake screen restarts the timers */
void screen_pwr_short(void);                     /* PWR short with no confirmation: toggles ON <-> OFF */
void idle_enter(void);                           /* debug `idle`: DIM now (as if dim_s passed) */
void idle_wake(void);                            /* a waking stroke (fx WAKE): TOUCH, logged "wake (touch)" */
void idle_wake_due(void);                        /* a due entry shows: DUE, logged "wake (due)" */
bool idle_active(void);                          /* the state is DIM or OFF */
bool idle_next_text(char *out, size_t len);      /* "Next: A at 14:00"; false when nothing is scheduled or the clock is invalid */
void idle_brightness_stop(void);                 /* whoever sets the brightness itself stops a running idle fade first */
#if CONFIG_HY_DEBUG_CONSOLE
void screen_debug(const char *arg);              /* `scr [off|nousb|usb]` */
#endif

/* splash.c */
int  splash_remaining_ms(void);                  /* ms until the splash has run 1000 ms, 0 when over or not running */
void splash_wait(void);                          /* blocks (LVGL lock not held) until the splash has run 1000 ms */
void splash_yield(void);                         /* LVGL lock held: lets the LVGL task draw a splash frame between build steps */
void splash_finish(lv_obj_t *next);              /* LVGL lock held: loads next with the 200 ms fade */

/* due.c: the due card, an overlay inside ui_content() above every view */
void due_create(void);
void due_refresh(void);                  /* from the snapshot, when its generation changed; flash request too */
bool due_visible(void);                  /* on USB the screen is held on while true (R1) */
void due_prev(void);                     /* swipe down: previous showing entry, wrapping */
void due_next(void);                     /* swipe up: next showing entry, wrapping */
void due_snooze(void);                   /* swipe left: snooze the shown entry by settings snooze_min */
void due_slide_complete(void);           /* the knob reached 90%: log taken */
void due_skip(void);                     /* the Skip tap: log skipped */
#if CONFIG_HY_DEBUG_CONSOLE
void due_flash_debug(void);
void due_drag_debug(bool drop);          /* debug: simulated knob grab (false) and release (true) */              /* debug: runs the speaker-failure flash now */
#endif

/* poweroff.c: the power-off confirmation and the critical-battery countdown, overlays on lv_layer_top */
void poweroff_create(void);                      /* after idle_create */
void poweroff_show(void);                        /* the confirmation (PWR long); ignored while the countdown shows */
void poweroff_cancel(void);                      /* closes the confirmation; no-op when it is not up */
bool poweroff_visible(void);                     /* the confirmation is up */
void poweroff_countdown_show(void);              /* critical battery: 60 s countdown, only USB cancels it */
bool poweroff_countdown_visible(void);
void poweroff_pwr_short(void);                   /* hy_power_ui_t.pwr_short */
void poweroff_pwr_long(void);                    /* hy_power_ui_t.pwr_long */
void poweroff_usb(bool in);                      /* hy_power_ui_t.usb */
#if CONFIG_HY_DEBUG_CONSOLE
void poweroff_debug(const char *arg);            /* show | slide | cancel | crit */
#endif

/* toast.c */
void toast_create(lv_obj_t *parent);
void toast_show_logged(const char *code, const char *hhmm, uint32_t log_id);
void toast_show_skipped(const char *code, const char *hhmm, uint32_t log_id); /* "V skipped 08:31", Undo, same rules */
/* Low-battery toast: held in one pending slot (a newer percent replaces it) while the Undo toast is open or the
 * screen is not fully on, shown when the Undo toast ends or on the next wake. The flush is the wake hook. */
void ui_battery_toast(int pct);
void ui_battery_toast_flush(void);
void toast_show_info(const char *text);  /* warning style, no Undo */
bool toast_undo_pressed(void);           /* false when no Undo is visible */
bool toast_logged_visible(void);         /* true while the logged toast and its tap-absorbing scrim are up */
