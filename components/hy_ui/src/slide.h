#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "sdkconfig.h"

/* The slide knob shared by the due card and the power-off confirmation: an 88 px track, a 72 px knob, a drag that
 * owns its stroke, release at >= 90% of the travel completes, otherwise the knob returns in 150 ms. Everything runs
 * in the LVGL task. */
#define SLIDE_TRACK_W   (480 - 2 * 26)
#define SLIDE_TRACK_H   88
#define SLIDE_KNOB_D    72
#define SLIDE_KNOB_PAD  ((SLIDE_TRACK_H - SLIDE_KNOB_D) / 2)
#define SLIDE_TRAVEL    (SLIDE_TRACK_W - 2 * SLIDE_KNOB_PAD - SLIDE_KNOB_D)

typedef struct {
    lv_obj_t *knob;
    const char *tag;                      /* log tag of the owner */
    bool (*begin)(void);                  /* a stroke started on the knob: false = the owner refuses it (nothing moves) */
    void (*end)(bool complete);           /* the stroke ended and the knob is already back at the start */
    bool drag;
    int32_t x0, off0, off;
} slide_t;

/* Registers the knob's events. Fill knob, tag, begin and end first. */
void slide_attach(slide_t *s);
void slide_reset(slide_t *s);              /* knob to the start, no drag */
void slide_end(slide_t *s, bool released); /* ends a drag as the release (true) or a lost press (false) would */
#if CONFIG_HY_DEBUG_CONSOLE
void slide_debug_grab(slide_t *s);         /* a drag with the knob at the end of the track */
#endif
