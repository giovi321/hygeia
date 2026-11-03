#include "hy/wake_gate.h"

#include "hy/nav.h"

enum { ST_IDLE = 0, ST_DOWN1, ST_GAP, ST_DOWN2 };

void hy_wake_gate_reset(hy_wake_gate_t *g)
{
    g->st = ST_IDLE;
    g->x0 = g->y0 = g->x = g->y = g->px = g->py = 0;
    g->t0 = g->pt = 0;
    g->max_dev = 0;
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
