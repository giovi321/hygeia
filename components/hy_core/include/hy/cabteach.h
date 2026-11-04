#ifndef HY_CABTEACH_H
#define HY_CABTEACH_H

/* Teach and check sequencer of Cabinet open. Pure: no IO, no clock, no tasks. The runner feeds it one stream
   sample at a time (timestamps in ms, wrapping allowed) and acts on what it returns; every function returns the
   one thing to do next.

   Phases of a take, in order:
     LEAD   once, after the start: HY_CT_LEAD_MS with the stream ignored, so the person can put the device or the
            cabinet back after tapping Start. Posts HY_CABEV_TEACH_READY.
     STILL  the still tracker must report stillness (the runner resets it when STILL begins, so stillness means
            HY_CAB_STILL_MS of it); fails after HY_CT_STILL_MS.
     GO     the person does the movement: a trigger hit starts the capture; fails after HY_CT_GO_MS.
     CAP    the runner captures until the movement ends.
     PAUSE  after a counted take, a too small or too long capture or an odd take: HY_CT_PAUSE_MS with the stream
            ignored (the person goes back to where they started), then STILL again for the take in question.
   TEACH takes HY_CAB_TAKES takes, then the runner builds the model (hy_ct_build_result) and saves it
   (hy_ct_saved). CHECK takes one, then the runner matches it (hy_ct_check_result). */

#include <stdbool.h>
#include <stdint.h>
#include "hy/cabinet.h"

#define HY_CT_LEAD_MS    5000
#define HY_CT_STILL_MS   20000
#define HY_CT_GO_MS      60000
#define HY_CT_PAUSE_MS   5000
#define HY_CT_REDOS      2       /* an odd take is asked for again at most this many times in all */

typedef enum { HY_CT_TEACH = 0, HY_CT_CHECK } hy_ct_mode_t;
typedef enum { HY_CT_OFF = 0, HY_CT_LEAD, HY_CT_STILL, HY_CT_GO, HY_CT_CAP, HY_CT_PAUSE, HY_CT_BUILD } hy_ct_phase_t;

typedef struct {
    uint8_t mode, phase;
    uint8_t takes_n;   /* takes counted so far */
    uint8_t cur;       /* the take in progress, 0 based */
    uint8_t redos;
    uint32_t t0;       /* start of the phase; the end of the pause while PAUSE */
} hy_ct_t;

/* What the runner does next. Any combination of the fields, in this order: post the event, reset the still
   tracker, take the rest vector, start the capture, store the capture, build, save, match, end. */
typedef struct {
    bool has_ev;
    hy_cabev_kind_t ev;
    uint8_t take;          /* the event's take field */
    bool reset_still;      /* STILL began: restart the still tracker */
    bool take_rest;        /* GO began: remember the still tracker's rest vector for the capture */
    bool start_capture;    /* a trigger hit during GO */
    bool store;            /* the capture is a counted take: keep it in slot (TEACH) or as the check capture */
    uint8_t slot;          /* the take slot for store */
    bool build;            /* TEACH: all takes are in: hy_cab_model_build, then hy_ct_build_result */
    bool match;            /* CHECK: the capture is in: match it, then hy_ct_check_result */
    bool save;             /* TEACH: the model is good (or accepted as it is): save it, then hy_ct_saved */
    bool accept_as_is;     /* with save: set the model valid first (an odd take that stays odd) */
    bool end;              /* finished: the runner ends the run */
} hy_ct_out_t;

/* Starts a run at time t. Posts TEACH_READY (take 1). */
hy_ct_out_t hy_ct_begin(hy_ct_t *c, hy_ct_mode_t mode, uint32_t t);
/* One stream sample while not capturing. still: the still tracker reports stillness; hit: the trigger fires. */
hy_ct_out_t hy_ct_step(hy_ct_t *c, uint32_t t, bool still, bool hit);
/* The capture ended (res other than HY_CAP_MORE). TOO_SHORT and TOO_LONG ask for the take again. */
hy_ct_out_t hy_ct_capture_done(hy_ct_t *c, uint32_t t, hy_cap_res_t res);
/* The switch to the capture failed: the take is asked for again. */
hy_ct_out_t hy_ct_capture_failed(hy_ct_t *c, uint32_t t);
/* After build: ok (hy_cab_model_build returned HY_OK), odd (its *odd), d_self_finite (the model's d_self). Posts
   TEACH_ODD and pauses for an odd take, at most HY_CT_REDOS times; after that an odd take with a finite d_self is
   saved as it is, anything else fails with HY_CAB_FAIL_MODEL. */
hy_ct_out_t hy_ct_build_result(hy_ct_t *c, uint32_t t, bool ok, int odd, bool d_self_finite);
/* After the save: TEACH_DONE, or TEACH_FAIL with HY_CAB_FAIL_SAVE. Ends the run. */
hy_ct_out_t hy_ct_saved(hy_ct_t *c, bool ok);
/* After the match: CHECK_END with HY_CAB_CHECK_OK or HY_CAB_CHECK_MISS. Ends the run. */
hy_ct_out_t hy_ct_check_result(hy_ct_t *c, bool match);
/* The run is cancelled from outside the sequencer (screen off, shake toast, IMU fault): TEACH_FAIL with
   HY_CAB_FAIL_ABORT, or CHECK_END with HY_CAB_CHECK_ABORT. Ends the run. */
hy_ct_out_t hy_ct_abort(hy_ct_t *c);
bool hy_ct_active(const hy_ct_t *c);

#endif
