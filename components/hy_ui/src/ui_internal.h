#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "sdkconfig.h"
#include "hy/actions.h"
#include "hy/nav.h"
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

/* nav_host.c */
void      nav_host_init(void);                   /* creates the views in ui_content(), shows the root */
void      nav_host_tick(void);                   /* root flags from the clock and the storage state */
void      ui_show_view(hy_view_t v);             /* nav_host: hides others, runs the view's refresh */
hy_nav_fx_t ui_nav_gesture(hy_gesture_t g);      /* applies hy_nav_gesture and its effect; WAKE = swallowed */
bool      ui_nav_idle(void);                     /* the dimmed screen: the next stroke only wakes */
void      ui_nav_set_idle(bool idle);            /* stub until the idle module (Task 17) */
void      ui_nav_push(hy_view_t v);
void      ui_open_detail(uint32_t med_id);
void      ui_set_storage_down(bool down);        /* LVGL task: storage state for the root */
void      ui_nav_log(void);                      /* "nav: HOME > QS" */
const char *ui_gesture_name(hy_gesture_t g);

/* input.c
 * The indev RELEASED callback (classification and navigation) runs before the pressed widget gets its own
 * RELEASED / SHORT_CLICKED / CLICKED. So a widget that owns a stroke (a slider, the due card knob, a scrolling
 * list) must call ui_input_consume() on LV_EVENT_PRESSED, LV_EVENT_PRESSING or LV_EVENT_SCROLL_BEGIN; from a
 * RELEASED or CLICKED handler it is too late and the gesture has already navigated. A stroke that only woke
 * the dimmed screen is neither a tap nor still, so no widget acts on it. */
void      input_init(lv_indev_t *indev);
bool      ui_input_last_was_tap(void);           /* the stroke that produced the current click classified as TAP */
bool      ui_input_still(void);                  /* the current or last stroke never left the tap slop */
void      ui_input_consume(void);                /* the current stroke belongs to a widget: no gesture */
#if CONFIG_HY_DEBUG_CONSOLE
void      input_debug_stroke(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t ms); /* press + release */
#endif

/* topbar.c */
void      topbar_create(void);                   /* on lv_layer_top, below every overlay */
void      topbar_refresh(void);                  /* time, Wi-Fi off icon, battery placeholder */
void      topbar_set_visible(bool on);
void      topbar_set_grab(bool on);              /* pull-down handle, Home only */

/* pager.c */
lv_obj_t *pager_create(lv_obj_t *parent);        /* the pager root: three pages and the dots */
lv_obj_t *pager_page(int idx);                   /* 0 Home, 1 Overview, 2 History */
void      pager_show(int idx, bool animate);     /* 200 ms ease-out x offset */
int       pager_index(void);

/* storage_down.c */
lv_obj_t *storage_down_create(lv_obj_t *parent);

/* home.c */
void home_create(lv_obj_t *page);
void home_rebuild(void);                 /* meds changed */
void home_refresh(bool animate);         /* tiles from the scheduler snapshot; LVGL only for changed ones */
void home_tap_med(uint32_t med_id);      /* logs a dose (debug `tap` calls it directly) */
void home_tile_click(uint32_t med_id);   /* a tile's SHORT_CLICKED: logs only when the stroke was a TAP */
bool home_med_id_by_code(const char *code, uint32_t *id);
void home_refresh_if_changed(void);      /* home_refresh(true) when the snapshot generation changed */
void home_refresh_timing(uint32_t *view_us, uint32_t *lvgl_us); /* last home_refresh, for perf */

/* toast.c */
void toast_create(lv_obj_t *parent);
void toast_show_logged(const char *code, const char *hhmm, uint32_t log_id);
void toast_show_info(const char *text);  /* warning style, no Undo */
bool toast_undo_pressed(void);           /* false when no Undo is visible */
bool toast_logged_visible(void);         /* true while the logged toast and its tap-absorbing scrim are up */
