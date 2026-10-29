#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "hy/actions.h"

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

/* The one place that defines the type scale. Swap the fonts here only. */
typedef struct {
    const lv_font_t *status;       /* 17 px regular: tile status line */
    const lv_font_t *status_bold;  /* 17 px bold: "Due now" */
    const lv_font_t *body;         /* 22 px regular: toast text, battery */
    const lv_font_t *body_bold;    /* 22 px bold: Undo */
    const lv_font_t *time;         /* 30 px bold: top bar time */
    const lv_font_t *code;         /* 52 px bold: medicine code */
} hy_type_scale_t;
extern const hy_type_scale_t hy_type;

/* medicine palette, index 0..9 */
lv_color_t hy_med_color(uint8_t idx);

/* geometry (px) */
#define HY_SCREEN       480
#define HY_BAR_H        56
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

/* home.c */
void home_create(lv_obj_t *parent);
void home_rebuild(void);                 /* meds changed */
void home_refresh(bool animate);         /* redraw every tile and the clock from the scheduler snapshot */
void home_tap_med(uint32_t med_id);      /* the tile tap handler */
bool home_med_id_by_code(const char *code, uint32_t *id);
void home_refresh_if_changed(void);      /* home_refresh(true) when the snapshot generation changed */

/* toast.c */
void toast_create(lv_obj_t *parent);
void toast_show_logged(const char *code, const char *hhmm, uint32_t log_id);
void toast_show_info(const char *text);  /* warning style, no Undo */
bool toast_undo_pressed(void);           /* false when no Undo is visible */
bool toast_logged_visible(void);         /* true while the logged toast and its tap-absorbing scrim are up */

/* ui.c */
void ui_log_hwm(const char *what);       /* logs the LVGL task stack margin once per `what` */
