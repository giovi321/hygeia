#include "hy/nav.h"

#include <stdlib.h>

/* ---- gestures ---- */

hy_gesture_t hy_gesture_classify(const hy_stroke_t *s)
{
    int dx = (int)s->x1 - (int)s->x0;
    int dy = (int)s->y1 - (int)s->y0;
    int adx = abs(dx), ady = abs(dy);

    int move = adx > ady ? adx : ady;
    if (s->max_dev > move) move = s->max_dev;
    if (move <= HY_TAP_SLOP_PX) {
        uint32_t dur = s->t1_ms - s->t0_ms;
        return dur < HY_LONG_PRESS_MS ? HY_G_TAP : HY_G_LONG_PRESS;
    }
    if (adx >= ady) {
        /* horizontal: dominant axis >= 60 and >= 1.5x the other (2*a >= 3*b in integers) */
        if (adx < HY_SWIPE_MIN_PX || 2 * adx < 3 * ady) return HY_G_NONE;
        if (dx < 0) return HY_G_SWIPE_LEFT;
        return s->x0 < HY_EDGE_PX ? HY_G_EDGE_RIGHT : HY_G_SWIPE_RIGHT;
    }
    if (ady < HY_SWIPE_MIN_PX || 2 * ady < 3 * adx) return HY_G_NONE;
    if (dy > 0) return s->y0 < HY_EDGE_PX ? HY_G_EDGE_DOWN : HY_G_SWIPE_DOWN;
    return s->y0 >= HY_SCREEN_PX - HY_EDGE_PX ? HY_G_EDGE_UP : HY_G_SWIPE_UP;
}

bool hy_gesture_double(const hy_stroke_t *prev_tap, const hy_stroke_t *tap)
{
    int32_t gap = (int32_t)(tap->t0_ms - prev_tap->t1_ms); /* wrap-safe */
    if (gap < 0 || gap > HY_DOUBLE_TAP_MS) return false;
    int dx = (int)tap->x0 - (int)prev_tap->x1;
    int dy = (int)tap->y0 - (int)prev_tap->y1;
    return dx * dx + dy * dy <= HY_DOUBLE_TAP_PX * HY_DOUBLE_TAP_PX;
}

/* ---- navigation ---- */

static bool is_pager(hy_view_t v) { return v == HY_V_HOME || v == HY_V_OVERVIEW || v == HY_V_HISTORY; }

void hy_nav_init(hy_nav_t *n)
{
    n->stack[0] = HY_V_HOME;
    n->depth = 1;
    n->med_id = 0;
    n->due = n->idle = n->clock_invalid = n->storage_down = n->poweroff = n->form_dirty = false;
}

hy_view_t hy_nav_view(const hy_nav_t *n) { return n->stack[n->depth - 1]; }

void hy_nav_push(hy_nav_t *n, hy_view_t v)
{
    if (n->depth >= HY_NAV_DEPTH) return;
    n->stack[n->depth++] = v;
}

void hy_nav_open_detail(hy_nav_t *n, uint32_t med_id)
{
    n->med_id = med_id;
    hy_nav_push(n, HY_V_DETAIL);
}

void hy_nav_set_root_flags(hy_nav_t *n, bool clock_invalid, bool storage_down)
{
    n->clock_invalid = clock_invalid;
    n->storage_down = storage_down;
    hy_view_t old = n->stack[0];
    hy_view_t root;
    if (storage_down) root = HY_V_STORAGE_DOWN;
    else if (clock_invalid) root = HY_V_CLOCK_INVALID;
    else root = is_pager(old) ? old : HY_V_HOME;
    if (root == old) return;
    n->stack[0] = root;
    n->depth = 1;
}

static void pop(hy_nav_t *n)
{
    if (n->depth > 1) n->depth--;
}

hy_nav_fx_t hy_nav_gesture(hy_nav_t *n, hy_gesture_t g)
{
    /* 0a. an overlay up on a DIM or OFF screen (the countdown after PWR short): the stroke only wakes, as rule 1
       does for every other view */
    if (n->poweroff && n->idle) {
        n->idle = false;
        return HY_FX_WAKE;
    }
    /* 0. power-off confirmation: the slider and buttons are widgets, gestures do nothing */
    if (n->poweroff) return HY_FX_NONE;
    /* 1. idle: every gesture only wakes */
    if (n->idle) {
        n->idle = false;
        return HY_FX_WAKE;
    }
    /* 2. due card */
    if (n->due) {
        if (g == HY_G_SWIPE_LEFT) return HY_FX_SNOOZE;
        if (g == HY_G_SWIPE_UP || g == HY_G_EDGE_UP) return HY_FX_DUE_NEXT;
        if (g == HY_G_SWIPE_DOWN || g == HY_G_EDGE_DOWN) return HY_FX_DUE_PREV;
        return HY_FX_NONE;
    }
    hy_view_t v = hy_nav_view(n);
    /* 4. quick settings */
    if (v == HY_V_QS) {
        if (g == HY_G_SWIPE_UP || g == HY_G_EDGE_UP) {
            pop(n);
            return HY_FX_NONE;
        }
    } else if (g == HY_G_EDGE_DOWN) {
        if (n->depth >= HY_NAV_DEPTH) n->stack[HY_NAV_DEPTH - 1] = HY_V_QS; /* QS always opens */
        else hy_nav_push(n, HY_V_QS);
        return HY_FX_NONE;
    }
    /* 4b. medicine form: EDGE_RIGHT is one step back (the UI decides), a dirty form ignores EDGE_UP */
    if (v == HY_V_MEDFORM) {
        if (g == HY_G_EDGE_RIGHT) return HY_FX_FORM_BACK;
        if (g == HY_G_EDGE_UP && n->form_dirty) return HY_FX_NONE;
    }
    /* 5. EDGE_UP elsewhere: back to the root, pager root becomes HOME. A changed form in the
     * stack is kept: the stack is cut back to the form, which asks "Discard changes?" on leaving */
    if (g == HY_G_EDGE_UP) {
        if (n->form_dirty) {
            for (int i = 0; i < n->depth; i++) {
                if (n->stack[i] == HY_V_MEDFORM) {
                    n->depth = i + 1;
                    return HY_FX_NONE;
                }
            }
        }
        n->depth = 1;
        if (is_pager(n->stack[0])) n->stack[0] = HY_V_HOME;
        return HY_FX_NONE;
    }
    /* 6. EDGE_RIGHT above the root: pop one level */
    if (g == HY_G_EDGE_RIGHT) {
        if (n->depth > 1) {
            pop(n);
            return HY_FX_NONE;
        }
        if (is_pager(n->stack[0])) g = HY_G_SWIPE_RIGHT; /* pager root: page back */
        else return HY_FX_NONE;
    }
    /* 7. pager at depth 1 */
    if (n->depth == 1 && is_pager(n->stack[0])) {
        if (g == HY_G_SWIPE_LEFT && n->stack[0] != HY_V_HISTORY)
            n->stack[0] = (n->stack[0] == HY_V_HOME) ? HY_V_OVERVIEW : HY_V_HISTORY;
        else if (g == HY_G_SWIPE_RIGHT && n->stack[0] != HY_V_HOME)
            n->stack[0] = (n->stack[0] == HY_V_HISTORY) ? HY_V_OVERVIEW : HY_V_HOME;
    }
    /* 8. anything else */
    return HY_FX_NONE;
}

/* ---- idle burn-in drift ---- */

static const int8_t drift_x[16] = { 0, 3, 6, 8, 8, 6, 3, 0, -2, -5, -8, -8, -6, -3, -1, 1 };
static const int8_t drift_y[16] = { -8, -6, -3, -1, 2, 5, 7, 8, 6, 3, 1, -2, -5, -7, -8, -7 };

void hy_idle_drift(int64_t minute, int8_t *dx, int8_t *dy)
{
    int i = (int)(((minute % 16) + 16) % 16);
    *dx = drift_x[i];
    *dy = drift_y[i];
}
