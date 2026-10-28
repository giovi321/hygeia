#include "hy/events.h"

static struct { hy_ev_cb_t cb; void *ctx; } subs[HY_EV_MAX_SUBSCRIBERS];

int hy_ev_subscribe(hy_ev_cb_t cb, void *ctx)
{
    if (!cb) return -1;
    for (int i = 0; i < HY_EV_MAX_SUBSCRIBERS; i++) {
        if (!subs[i].cb) {
            subs[i].cb = cb;
            subs[i].ctx = ctx;
            return i;
        }
    }
    return -1;
}

void hy_ev_unsubscribe(int handle)
{
    if (handle < 0 || handle >= HY_EV_MAX_SUBSCRIBERS) return;
    subs[handle].cb = NULL;
    subs[handle].ctx = NULL;
}

void hy_ev_publish(const hy_event_t *ev)
{
    if (!ev) return;
    for (int i = 0; i < HY_EV_MAX_SUBSCRIBERS; i++) {
        /* a callback may unsubscribe itself; re-check each slot just before the call */
        if (subs[i].cb) subs[i].cb(ev, subs[i].ctx);
    }
}
