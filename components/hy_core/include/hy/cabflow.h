#ifndef HY_CABFLOW_H
#define HY_CABFLOW_H

/* Pure mapping from the runner's events to the phase, the status line and the sound cue of the Cabinet open
   teach view (one view, several phases). The view reads the result and draws it; the cue enum is mapped to the
   tones in hy_ui. */

#include <stdbool.h>
#include <stdint.h>
#include "hy/cabinet.h"

typedef enum {
    HY_CF_IDLE = 0,     /* body text, empty dots; Start, Cancel */
    HY_CF_RUNNING,      /* teaching: counter, dots, status; Cancel */
    HY_CF_SAVED,        /* the movement was saved: Test it now?; Test, Done */
    HY_CF_CHECKING,     /* a check runs: status and the check text; Cancel */
    HY_CF_CHECK_OK,     /* the check recognised the movement; Done */
    HY_CF_CHECK_MISS,   /* the check did not recognise the movement; Teach again, Keep */
    HY_CF_CHECK_NOMOVE, /* the check saw no movement; Teach again, Keep */
    HY_CF_FAILED,       /* teaching stopped; Start, Cancel */
    HY_CF_N
} hy_cf_phase_t;

typedef enum {
    HY_CF_S_NONE = 0,
    HY_CF_S_READY,      /* Get ready */
    HY_CF_S_STILL,      /* Hold still */
    HY_CF_S_NOW,        /* Now */
    HY_CF_S_GOT,        /* Got it */
    HY_CF_S_SMALL,      /* too small, do it again */
    HY_CF_S_LONG,       /* too long, do it again */
    HY_CF_S_ODD,        /* looked different, do it again */
    HY_CF_S_NOMOVE,     /* teaching stopped: no movement */
    HY_CF_S_IO,         /* teaching stopped: the file could not be written */
    HY_CF_S_STOPPED,    /* teaching stopped */
    HY_CF_S_C_NOSTILL,  /* a check: not still, test stopped */
    HY_CF_S_C_STOPPED   /* a check: test stopped */
} hy_cf_status_t;

typedef enum { HY_CUE_NONE = 0, HY_CUE_GO, HY_CUE_TAKE, HY_CUE_REDO, HY_CUE_DONE, HY_CUE_FAIL } hy_cab_cue_t;

typedef enum {
    HY_CF_U_START,      /* Start: IDLE or FAILED to RUNNING */
    HY_CF_U_TEST,       /* Test: SAVED to CHECKING */
    HY_CF_U_AGAIN       /* Teach again: CHECK_MISS or CHECK_NOMOVE to RUNNING */
} hy_cf_user_t;

typedef struct { uint8_t phase; uint8_t status; bool redo; } hy_cf_t;   /* redo: the take in hand is asked for again */
typedef struct { uint8_t phase; uint8_t status; hy_cab_cue_t cue; } hy_cf_out_t;

void hy_cab_flow_init(hy_cf_t *f);   /* IDLE, no status */
/* The runner's event kind and take field. Events that do not apply to the phase change nothing and give no cue.
   The cue of the last take's TEACH_TAKE is none, and so is the cue of a take asked for again after an odd one (it is
   the last capture before the build): the DONE, REDO or FAIL cue follows within the build time. The
   returned phase and status are also stored in f. */
hy_cf_out_t hy_cab_flow_event(hy_cf_t *f, hy_cabev_kind_t kind, uint8_t take);
/* A button. Returns the new phase (unchanged when the button does not apply to the phase). The caller starts the
   teach or the check and calls this only when the runner accepted it. */
uint8_t hy_cab_flow_user(hy_cf_t *f, hy_cf_user_t u);
const char *hy_cab_cue_name(hy_cab_cue_t c);

#endif
