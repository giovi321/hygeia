#ifndef HY_EVENTS_H
#define HY_EVENTS_H

#include <stdint.h>
#include "hy/types.h"

typedef enum { HY_EV_TAKEN = 0, HY_EV_SKIPPED, HY_EV_MISSED, HY_EV_DUE, HY_EV_SNOOZED,
               HY_EV_UNDONE, HY_EV_LOG_EDITED, HY_EV_MEDS_CHANGED, HY_EV_UNTRACKED,
               HY_EV_BATTERY_LOW, HY_EV_BATTERY_CRITICAL, HY_EV_CLOCK_INVALID } hy_ev_type_t;

typedef struct { hy_ev_type_t type; int64_t ts; uint32_t med_id; uint32_t log_id; } hy_event_t;
typedef void (*hy_ev_cb_t)(const hy_event_t *ev, void *ctx);

#define HY_EV_MAX_SUBSCRIBERS 16

/* Returns a handle >= 0, or -1 when cb is NULL or all 16 slots are taken. */
int  hy_ev_subscribe(hy_ev_cb_t cb, void *ctx);
/* Frees the slot; an invalid or already freed handle is ignored. */
void hy_ev_unsubscribe(int handle);
/* Synchronous, in the caller's context, subscribers called in slot order. */
void hy_ev_publish(const hy_event_t *ev);

#endif
