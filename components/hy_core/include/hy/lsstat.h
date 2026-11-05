#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure helpers of the battery investigation tools (debug console only, see design/hardware-notes.md): the sleep
   length histogram, the time on battery per screen state, the ring of 60 s battery readings and the parsing of
   the `pwrx` command. No device calls, so they run in the host tests. */

/* ---- sleep length histogram ---- */

/* Buckets of the requested light-sleep length: under 50 ms, 50 to 200, 200 to 600, 600 to 1100, 1100 to 2100 and over
   2100 ms. The lower bound of each bucket is included. The bucket names the period of the waker behind the sleep. */
#define HY_LS_BUCKETS 6
int         hy_ls_bucket(int64_t requested_us);
const char *hy_ls_bucket_label(int bucket);   /* "?" outside 0..HY_LS_BUCKETS-1 */

/* How a light-sleep entry ended, from the time the exit callback reports. IDF computes that time whether the sleep ran
   or was rejected, so a rejected sleep arrives with a short positive value. */
typedef enum {
    HY_LS_NOT_TRIED = 0,   /* 0 us or less: the idle period was too short, no sleep was attempted */
    HY_LS_REJECTED,        /* under HY_LS_MIN_SLEPT_US: rejected, or a wake pin was already active */
    HY_LS_RAN              /* a sleep that ran */
} hy_ls_exit_t;
#define HY_LS_MIN_SLEPT_US 1000
hy_ls_exit_t hy_ls_exit_class(int64_t slept_us);

/* ---- time on battery per screen state ---- */

typedef enum {
    HY_ST_ON = 0,       /* screen ON */
    HY_ST_DIM,          /* screen DIM */
    HY_ST_OFF_ASLEEP,   /* screen OFF, LVGL paused by the sleep mode */
    HY_ST_OFF_HELD,     /* screen OFF, the sleep mode held off by the radio */
    HY_ST_OFF_AWAKE,    /* screen OFF, not asleep for any other reason */
    HY_ST_N
} hy_scrstate_t;
#define HY_ST_NONE (-1)   /* on USB: not counted */

typedef struct {
    uint64_t ms[HY_ST_N];
    int8_t   cur;         /* the state being timed, HY_ST_NONE when none */
    int64_t  last_ms;     /* when cur began, or the last mark */
} hy_scrtime_t;

/* scr: 0 on, 1 dim, 2 off (the batlog column). Time is counted without vbus only. held: the radio holds the sleep
   mode off. */
int  hy_scrtime_category(bool vbus, uint8_t scr, bool asleep, bool held);
/* The state changes to `cat` at now_ms: the time since the last mark goes to the state that was timed until now.
   A call with the same state only closes the stretch up to now_ms. A now_ms before the last mark adds nothing. */
void hy_scrtime_mark(hy_scrtime_t *t, int64_t now_ms, int cat);
void hy_scrtime_reset(hy_scrtime_t *t);
const char *hy_scrstate_name(int state);

/* ---- ring of battery readings ---- */

#define HY_MV_RING 256
typedef struct {
    uint32_t t_s;     /* seconds since boot */
    uint16_t mv;
    int8_t   pct;
    uint8_t  scr;     /* 0 on, 1 dim, 2 off */
} hy_mv_pt_t;

typedef struct {
    uint32_t   total;           /* points ever pushed; the slot of the next one is total % HY_MV_RING */
    hy_mv_pt_t pt[HY_MV_RING];
} hy_mv_ring_t;

void     hy_mv_ring_clear(hy_mv_ring_t *r);
void     hy_mv_ring_push(hy_mv_ring_t *r, uint32_t t_s, uint16_t mv, int8_t pct, uint8_t scr);
uint32_t hy_mv_ring_len(const hy_mv_ring_t *r);                                   /* points kept, at most HY_MV_RING */
bool     hy_mv_ring_get(const hy_mv_ring_t *r, uint32_t i, hy_mv_pt_t *out);       /* i = 0 is the oldest kept point */

/* ---- pwrx ---- */

/* Debug flags, RAM only, off at boot. */
typedef enum { HY_PX_TOUCH = 0, HY_PX_NONET, HY_PX_PANEL, HY_PX_WAKERS, HY_PX_LSVBUS, HY_PX_N } hy_px_t;
const char *hy_px_name(int flag);   /* "?" outside 0..HY_PX_N-1 */

typedef enum {
    HY_PXC_LIST,       /* pwrx */
    HY_PXC_SET,        /* pwrx <flag> on|off */
    HY_PXC_ALL_OFF,    /* pwrx off */
    HY_PXC_MV,         /* pwrx mv */
    HY_PXC_MV_CLEAR,   /* pwrx mv clear */
    HY_PXC_BAD
} hy_pxc_kind_t;

typedef struct {
    hy_pxc_kind_t kind;
    int           flag;   /* HY_PXC_SET: the flag */
    bool          on;     /* HY_PXC_SET: on or off */
} hy_pxc_t;

hy_pxc_t hy_px_parse(const char *args);
/* "none", or the names of the flags in mask (bit n = flag n), separated by single spaces */
void     hy_px_names(uint32_t mask, char *out, size_t len);

#ifdef __cplusplus
}
#endif
