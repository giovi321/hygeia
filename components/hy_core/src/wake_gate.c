#include "hy/wake_gate.h"

#include <stdlib.h>

#include "hy/nav.h"

enum { ST_IDLE = 0, ST_DOWN1, ST_GAP, ST_DOWN2 };

void hy_wake_gate_reset(hy_wake_gate_t *g)
{
    g->st = ST_IDLE;
    g->x0 = g->y0 = g->x = g->y = g->px = g->py = 0;
    g->t0 = g->pt = 0;
    g->max_dev = 0;
    g->tb = 0;
    g->begun = false;
    g->fdown = false;
    g->fails = 0;
    g->lx = g->ly = 0;
}

void hy_wake_gate_begin(hy_wake_gate_t *g, uint32_t t)
{
    hy_wake_gate_reset(g);
    g->begun = true;
    g->tb = t;
}

static void begin(hy_wake_gate_t *g, uint32_t t, int16_t x, int16_t y)
{
    g->x0 = g->x = x;
    g->y0 = g->y = y;
    g->t0 = t;
    g->max_dev = 0;
}

hy_wg_act_t hy_wake_gate_down(hy_wake_gate_t *g, bool single_tap_wakes, uint32_t t, int16_t x, int16_t y)
{
    if (single_tap_wakes) {
        hy_wake_gate_reset(g);
        return HY_WG_WAKE;
    }
    if (g->st == ST_GAP) {
        hy_stroke_t prev = {.x1 = g->px, .y1 = g->py, .t1_ms = g->pt};
        hy_stroke_t cur = {.x0 = x, .y0 = y, .t0_ms = t};
        if (hy_gesture_double(&prev, &cur)) {
            begin(g, t, x, y);
            g->st = ST_DOWN2;
            return HY_WG_WAIT;
        }
    }
    begin(g, t, x, y);   /* a first touch; a tap that came too late or too far is forgotten */
    g->st = ST_DOWN1;
    return HY_WG_WAIT;
}

void hy_wake_gate_move(hy_wake_gate_t *g, int16_t x, int16_t y)
{
    if (g->st != ST_DOWN1 && g->st != ST_DOWN2) return;
    int dx = x - g->x0, dy = y - g->y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int d = dx > dy ? dx : dy;
    if (d > g->max_dev) g->max_dev = (uint16_t)(d > UINT16_MAX ? UINT16_MAX : d);
    g->x = x;
    g->y = y;
}

hy_wg_act_t hy_wake_gate_up(hy_wake_gate_t *g, uint32_t t, int16_t x, int16_t y)
{
    if (g->st != ST_DOWN1 && g->st != ST_DOWN2) return HY_WG_NONE;
    hy_wake_gate_move(g, x, y);
    hy_stroke_t s = {.x0 = g->x0, .y0 = g->y0, .x1 = x, .y1 = y, .t0_ms = g->t0, .t1_ms = t, .max_dev = g->max_dev};
    bool tap = hy_gesture_classify(&s) == HY_G_TAP;
    bool second = g->st == ST_DOWN2;
    if (!tap) {
        hy_wake_gate_reset(g);
        return HY_WG_SLEEP;
    }
    if (second) {
        hy_wake_gate_reset(g);
        return HY_WG_WAKE;
    }
    g->px = x;
    g->py = y;
    g->pt = t;
    g->st = ST_GAP;
    return HY_WG_WAIT;
}

hy_wg_act_t hy_wake_gate_tick(hy_wake_gate_t *g, uint32_t t)
{
    switch (g->st) {
    case ST_DOWN1:
    case ST_DOWN2:
        if ((int32_t)(t - g->t0) >= HY_LONG_PRESS_MS) {
            hy_wake_gate_reset(g);
            return HY_WG_SLEEP;
        }
        return HY_WG_WAIT;
    case ST_GAP:
        if ((int32_t)(t - g->pt) > HY_DOUBLE_TAP_MS) {
            hy_wake_gate_reset(g);
            return HY_WG_SLEEP;
        }
        return HY_WG_WAIT;
    default:
        return HY_WG_NONE;
    }
}

hy_wg_act_t hy_wake_gate_sample(hy_wake_gate_t *g, uint32_t t, bool ok, bool down, int16_t x, int16_t y)
{
    hy_wg_act_t act = HY_WG_NONE;
    if (g->begun && (int32_t)(t - g->tb) >= HY_WG_MAX_MS) {
        hy_wake_gate_reset(g);
        return HY_WG_SLEEP;
    }
    if (ok) {
        g->fails = 0;
        if (down && !g->fdown) {
            g->fdown = true;
            act = hy_wake_gate_down(g, false, t, x, y);
        } else if (down) {
            hy_wake_gate_move(g, x, y);
        } else if (g->fdown) {
            g->fdown = false;
            act = hy_wake_gate_up(g, t, g->lx, g->ly);
        } else if (g->st == ST_IDLE) {
            hy_wake_gate_reset(g);
            return HY_WG_SLEEP;   /* the interrupt came with no finger on the glass: noise */
        }
        if (down) {
            g->lx = x;
            g->ly = y;
        }
    } else if (g->st == ST_IDLE && ++g->fails >= 2) {
        hy_wake_gate_reset(g);
        return HY_WG_SLEEP;   /* the controller does not answer and nothing was seen: give up */
    }
    if (act == HY_WG_WAKE || act == HY_WG_SLEEP) return act;
    hy_wg_act_t tk = hy_wake_gate_tick(g, t);
    return tk != HY_WG_NONE ? tk : act;
}

void hy_tap_pair_reset(hy_tap_pair_t *p)
{
    p->have = false;
    p->t = 0;
    p->x = p->y = 0;
}

hy_wg_act_t hy_tap_pair_feed(hy_tap_pair_t *p, bool single_tap_wakes, uint32_t t, bool tap, int16_t x, int16_t y)
{
    if (!tap) {
        hy_tap_pair_reset(p);
        return HY_WG_SLEEP;
    }
    if (single_tap_wakes) {
        hy_tap_pair_reset(p);
        return HY_WG_WAKE;
    }
    if (p->have) {
        int32_t dt = (int32_t)(t - p->t);
        int dx = x - p->x, dy = y - p->y;
        if (dt >= 0 && dt <= HY_WG_TAP_PAIR_MS && abs(dx) <= HY_DOUBLE_TAP_PX && abs(dy) <= HY_DOUBLE_TAP_PX) {
            hy_tap_pair_reset(p);
            return HY_WG_WAKE;
        }
    }
    p->have = true;
    p->t = t;
    p->x = x;
    p->y = y;
    return HY_WG_WAIT;
}
