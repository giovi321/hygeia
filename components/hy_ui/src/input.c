/* Touch input layer: every stroke from press to release becomes an hy_stroke_t, is classified on release
 * (hy_gesture_classify) and drives the navigation (ui_nav_gesture) unless a widget consumed it.
 *
 * Stroke boundaries come from the indev PRESSED and RELEASED events, which LVGL sends before the pressed
 * widget gets its own RELEASED / SHORT_CLICKED, so a tile's click handler can ask ui_input_last_was_tap().
 * LVGL does not forward PRESSING to indev event callbacks, so max_dev is tracked in a wrapper around
 * the indev read callback, which sees every touch sample.
 *
 * Scroll inertia: the scrolling lists glide on after a fling and bounce at their ends (ui_scroller). A touch
 * that lands on a list while it is still moving only stops it, as on a phone: LVGL halts the glide itself, and this
 * layer marks the stroke so that, if it stays a tap or a long press, no widget acts on it and it does not
 * navigate. A touch outside the moving list (a quick settings tile, a toast, an overlay) is an ordinary stroke. A stroke that moves on from there is an ordinary drag or swipe. */

#include "esp_log.h"
#include "hy/net.h"
#include "hy/wake_gate.h"
#include "ui_internal.h"
#if CONFIG_HY_DEBUG_CONSOLE
#include "esp_timer.h"
#include "hy/board.h"
#endif

static const char *TAG = "ui_input";

/* Scroll throw: the glide speed keeps (100 - n)% per animation frame. LVGL steps the glide on its animation timer,
 * one step per LV_DEF_REFR_PERIOD (33 ms here), so n is tuned for 30 fps: 8 gives a time constant near 400 ms and
 * a throw of about 23 frames of finger speed (the release speed LVGL measures is about twice one sample's move,
 * from its 99 ms history). The LVGL default 10 stops sooner; 6 matches the iOS deceleration time but throws
 * about twice as far on a 480 px screen. */
#define SCROLL_THROW_PCT   8
/* A press within this time of the last scroll movement (a glide frame or the bounce at an end) stops a moving
 * list: two animation frames */
#define GLIDE_RECENT_MS    70

static lv_obj_t *s_glide_scroller;  /* read_wrap: the list that is moving under the new touch */
static lv_obj_t *s_last_scroller;   /* the ui_scroller that moved last */
static uint32_t s_scroll_ms;        /* lv_tick of that movement */
static bool s_glide_pending;        /* read_wrap: the new touch landed on a moving list */
static bool s_glide_stop;           /* the current or last stroke began by stopping a moving list */

static lv_indev_t *s_indev;
static lv_indev_read_cb_t s_orig_read;
static hy_stroke_t s_st;
static bool s_active;      /* between PRESSED and RELEASED */
static bool s_consumed;    /* a widget owns the current stroke */
static bool s_last_tap;    /* the last finished stroke classified as TAP */
static bool s_phys_down;   /* the last sample was pressed */
static bool s_last_double; /* the last finished stroke was the second tap of a double tap */
static hy_stroke_t s_prev_tap;
static bool s_have_prev;   /* s_prev_tap is a tap that may start a double tap */
static bool s_waking;      /* the stroke started on the dimmed screen: it only wakes */
static bool s_gated;       /* ... and "wake on single tap" is off: only a double tap of such strokes wakes (s_gate) */
static hy_wake_gate_t s_gate;

#if CONFIG_HY_DEBUG_CONSOLE
/* `touch trace on|off`: what the touch controller reports at the start of a stroke, for setting the edge zone */
static bool s_trace;
static struct {
    bool have;              /* a first point was read for the current stroke */
    bool second;            /* ... and a second, different one */
    int64_t isr_us;         /* touch INT before the first read, 0 unknown */
    int64_t t1_us, t2_us;   /* time of the first and the second point */
    int16_t x1, y1, x2, y2;
} s_tr;

void input_debug_trace(bool on)
{
    s_trace = on;
    s_tr.have = false;
}

static void trace_sample(bool down, int16_t x, int16_t y, bool new_touch)
{
    if (!s_trace) {
        return;
    }
    if (!down) {
        (void)hy_board_touch_isr_take();   /* a release edge is not a press */
        return;
    }
    int64_t now = esp_timer_get_time();
    if (new_touch) {
        s_tr.have = true;
        s_tr.second = false;
        s_tr.isr_us = hy_board_touch_isr_take();
        s_tr.t1_us = now;
        s_tr.x1 = x;
        s_tr.y1 = y;
    } else if (s_tr.have && !s_tr.second && (x != s_tr.x1 || y != s_tr.y1)) {
        s_tr.second = true;
        s_tr.t2_us = now;
        s_tr.x2 = x;
        s_tr.y2 = y;
    }
}

static void trace_end(int32_t x, int32_t y)
{
    if (!s_trace || !s_tr.have) {
        return;
    }
    s_tr.have = false;
    long long lat = s_tr.isr_us ? (long long)(s_tr.t1_us - s_tr.isr_us) : -1;
    if (s_tr.second) {
        ESP_LOGI(TAG, "trace first %d,%d +%lld us after the press, second %d,%d +%lld us after the first, end %d,%d",
                 s_tr.x1, s_tr.y1, lat, s_tr.x2, s_tr.y2, (long long)(s_tr.t2_us - s_tr.t1_us), (int)x, (int)y);
    } else {
        ESP_LOGI(TAG, "trace first %d,%d +%lld us after the press, second none, end %d,%d", s_tr.x1, s_tr.y1, lat,
                 (int)x, (int)y);
    }
}
#endif

static uint16_t dev_of(int32_t x, int32_t y)
{
    int32_t dx = x - s_st.x0, dy = y - s_st.y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int32_t d = dx > dy ? dx : dy;
    return (uint16_t)(d > UINT16_MAX ? UINT16_MAX : d);
}

static void read_wrap(lv_indev_t *indev, lv_indev_data_t *data)
{
    s_orig_read(indev, data);
    bool down = data->state == LV_INDEV_STATE_PRESSED;
    if (down && !s_phys_down && s_active) {
        /* a new touch while the previous stroke never got its RELEASED (the pressed widget was deleted) */
        ESP_LOGW(TAG, "stroke dropped: no release event");
        s_active = false;
    }
    if (down && !s_phys_down) {
        /* a new touch, read before LVGL processes it: LVGL stops a glide in that processing. scroll_obj stays set
         * through the glide, the timestamp covers the bounce animation after it. */
        lv_obj_t *moving = lv_indev_get_scroll_obj(indev);
        if (!moving && s_last_scroller && lv_tick_elaps(s_scroll_ms) <= GLIDE_RECENT_MS) {
            moving = s_last_scroller;
        }
        s_glide_scroller = moving;
        s_glide_pending = moving != NULL;
    }
    /* data->point is the raw sample, before LVGL rotates it into display coordinates (lv_display_rotate_point
     * in lv_indev.c), while x0/y0 come from the rotated lv_indev_get_point. They agree only because the
     * display is registered with ESP_LV_ADAPTER_ROTATE_0 (board.c; swap and mirror happen in the touch
     * driver). A rotated display must rotate a copy of the point here first. */
#if CONFIG_HY_DEBUG_CONSOLE
    trace_sample(down, (int16_t)data->point.x, (int16_t)data->point.y, down && !s_phys_down);
#endif
    if (down && s_active) {
        if (s_gated) {
            hy_wake_gate_move(&s_gate, (int16_t)data->point.x, (int16_t)data->point.y);
        }
        uint16_t d = dev_of(data->point.x, data->point.y);
        if (d > s_st.max_dev) {
            s_st.max_dev = d;
        }
    }
    s_phys_down = down;
}

static void finish(int32_t x, int32_t y, uint32_t t);

/* A stroke starting now only wakes: the screen is DIM or OFF, or a touch just woke the CPU from the sleep mode
 * (that wake ran before LVGL read the stroke, so the state is already ON) */
static bool stroke_waking(void)
{
    bool guard = screen_wake_guard_take();
    bool idle = ui_nav_idle();
    /* a stroke that follows a CPU wake by touch (guard) is already past the decision; a dimmed screen under an
     * overlay that wakes on any touch (power-off confirmation, pairing code) is not gated either */
    s_gated = idle && !guard && idle_double_tap_required();
    return idle || guard;
}

/* The pressed object is inside the list that is moving and the list is on screen */
static bool glide_target(lv_obj_t *pressed)
{
    lv_obj_t *list = s_glide_scroller;
    if (!list || !pressed || !lv_obj_is_visible(list)) {
        return false;
    }
    for (lv_obj_t *o = pressed; o; o = lv_obj_get_parent(o)) {
        if (o == list) {
            return true;
        }
    }
    return false;
}

/* The pressed object sits in a list whose content overflows: its view scrolls, so the bottom edge zone is thin */
static bool pressed_in_scroller(lv_obj_t *pressed)
{
    for (lv_obj_t *o = pressed; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_is_scrollable(o) && (lv_obj_get_scroll_top(o) > 0 || lv_obj_get_scroll_bottom(o) > 0)) {
            return true;
        }
    }
    return false;
}

static void indev_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(s_indev, &p);
    if (code == LV_EVENT_PRESSED) {
        if (s_active) {
            return;   /* LVGL searched a new widget mid-stroke: the stroke keeps its start */
        }
        s_active = true;
        lv_display_trigger_activity(NULL);
        hy_net_activity();   /* someone is using the device: the radio stays up on battery */
        s_waking = stroke_waking();
        if (s_gated) {
            hy_wake_gate_down(&s_gate, false, lv_tick_get(), (int16_t)p.x, (int16_t)p.y);
        } else {
            hy_wake_gate_reset(&s_gate);
        }
        screen_activity();
        s_glide_stop = s_glide_pending && !s_waking && glide_target(lv_indev_get_active_obj());
        s_glide_pending = false;
        s_st.glide_stop = s_glide_stop;
        s_st.scroll_view = pressed_in_scroller(lv_indev_get_active_obj());
        /* while the logged toast and its scrim are up a swipe does not navigate */
        s_consumed = toast_logged_visible();
        s_last_tap = false;
        s_last_double = false;
        s_st.x0 = s_st.x1 = (int16_t)p.x;
        s_st.y0 = s_st.y1 = (int16_t)p.y;
        s_st.t0_ms = s_st.t1_ms = lv_tick_get();
        s_st.max_dev = 0;
        return;
    }
    if (code != LV_EVENT_RELEASED || !s_active) {
        return;
    }
    finish(p.x, p.y, lv_tick_get());
}

/* Release: classify, log, navigate. A stroke the navigation swallowed as WAKE is not a tap, so the
 * SHORT_CLICKED that LVGL sends next to the pressed tile does nothing. */
static void finish(int32_t x, int32_t y, uint32_t t)
{
    s_active = false;
#if CONFIG_HY_DEBUG_CONSOLE
    trace_end(x, y);
#endif
    hy_net_activity();
    s_st.x1 = (int16_t)x;
    s_st.y1 = (int16_t)y;
    s_st.t1_ms = t;
    uint16_t d = dev_of(x, y);
    if (d > s_st.max_dev) {
        s_st.max_dev = d;
    }
    hy_gesture_t g = hy_gesture_classify(&s_st);
    s_last_tap = g == HY_G_TAP;
    if (s_glide_stop && (g == HY_G_TAP || g == HY_G_LONG_PRESS)) {
        /* the touch only stopped a moving list: not a tap for any widget, and no gesture */
        s_last_tap = false;
        s_consumed = true;
    }
    if (s_glide_stop) {
        /* LVGL drops the glide at the press without its end step, so a list caught past an end stays there unless
         * this stroke scrolls it on and LVGL throws it again at the release */
        lv_obj_t *o = s_glide_scroller;
        if (o && !lv_indev_get_scroll_obj(s_indev)) {
            int32_t top = lv_obj_get_scroll_top(o), bottom = lv_obj_get_scroll_bottom(o);
            if (top < 0) {
                lv_obj_scroll_by(o, 0, top, LV_ANIM_ON);
            } else if (bottom < 0) {
                lv_obj_scroll_by(o, 0, -bottom, LV_ANIM_ON);
            }
        }
    }
    ESP_LOGI(TAG, "stroke dx=%d dy=%d ms=%u -> %s (max_dev %u%s%s)", s_st.x1 - s_st.x0, s_st.y1 - s_st.y0,
             (unsigned)(s_st.t1_ms - s_st.t0_ms), ui_gesture_name(g), (unsigned)s_st.max_dev,
             s_consumed ? ", consumed" : "", s_glide_stop ? ", stopped a glide" : "");
    if (!s_waking) {
        screen_activity();   /* a long stroke restarts the timers at its release too */
    }
    if (s_waking) {
        /* the stroke began on the dimmed screen: it only wakes. When a due card woke the screen under the
         * finger the navigation is no longer idle and the stroke is dropped, so it cannot snooze or step it. */
        if (ui_nav_idle()) {
            /* gated: the second tap of a double tap wakes; a lone tap, a long press or a swipe is dropped and the
             * screen stays as it is (the gate remembers a first tap for HY_DOUBLE_TAP_MS) */
            if (!s_gated || hy_wake_gate_up(&s_gate, t, (int16_t)x, (int16_t)y) == HY_WG_WAKE) {
                ui_nav_gesture(g);
            } else {
                ESP_LOGI(TAG, "waking stroke dropped: not a double tap");
            }
        } else {
            ESP_LOGI(TAG, "waking stroke dropped: the screen woke under the finger");
        }
        s_last_tap = false;
    } else if (!s_consumed) {
        if (ui_nav_gesture(g) == HY_FX_WAKE) {
            s_last_tap = false;
        }
    }
    s_last_double = false;
    if (s_last_tap && !s_consumed) {
        if (s_have_prev && hy_gesture_double(&s_prev_tap, &s_st)) {
            s_last_double = true;
            s_have_prev = false;
        } else {
            s_prev_tap = s_st;
            s_have_prev = true;
        }
    } else {
        s_have_prev = false;
    }
}

void input_init(lv_indev_t *indev)
{
    if (!indev) {
        ESP_LOGE(TAG, "no touch input device");
        return;
    }
    s_indev = indev;
    s_orig_read = lv_indev_get_read_cb(indev);
    lv_indev_set_read_cb(indev, read_wrap);
    lv_indev_add_event_cb(indev, indev_cb, LV_EVENT_PRESSED, NULL);
    lv_indev_add_event_cb(indev, indev_cb, LV_EVENT_RELEASED, NULL);
    lv_indev_set_scroll_throw(indev, SCROLL_THROW_PCT);
}

static void scroller_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        if (s_last_scroller == o) {
            s_last_scroller = NULL;
        }
        if (s_glide_scroller == o) {
            s_glide_scroller = NULL;
        }
        return;
    }
    s_last_scroller = o;
    s_scroll_ms = lv_tick_get();
}

/* A vertical list with inertia: it glides on after a fling and, when its content overflows, gives at its ends and
 * springs back. The caller still consumes its strokes on LV_EVENT_SCROLL_BEGIN. */
void ui_scroller(lv_obj_t *o)
{
    lv_obj_set_scroll_momentum(o, true);
    lv_obj_set_scroll_elastic(o, true);
    lv_obj_add_event_cb(o, scroller_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(o, scroller_cb, LV_EVENT_DELETE, NULL);
}

bool ui_input_last_was_tap(void)
{
    return !s_active && s_last_tap;
}

bool ui_input_last_was_double(void)
{
    return !s_active && s_last_double;
}

bool ui_input_still(void)
{
    return !s_waking && !s_glide_stop && s_st.max_dev <= HY_TAP_SLOP_PX;
}

bool ui_input_waking(void)
{
    return s_waking;
}

/* A stroke that starts in the hard edge zone (HY_EDGE_PX, and HY_EDGE_SCROLL_PX at the bottom over scrolling content)
 * is never a widget's: EDGE_RIGHT, EDGE_DOWN and EDGE_UP navigate from any screen, also over scrolling content, the
 * keyboard and sliders. A stroke that starts in the wider band up to HY_EDGE_GESTURE_PX belongs to a widget that
 * consumes it (a list scroll, a key press, a roller turn); one that no widget consumes is classified as the edge
 * gesture in hy_gesture_classify. */
/* A widget that drags horizontally (a slider) owns its stroke even from an edge zone: a drag that starts at
 * x < 40 must not pop the view mid-drag. */
void ui_input_consume_force(void)
{
    s_consumed = true;
}

void ui_input_consume(void)
{
    int bottom = s_st.scroll_view ? HY_EDGE_SCROLL_PX : HY_EDGE_PX;
    if (s_st.x0 < HY_EDGE_PX || s_st.y0 < HY_EDGE_PX || s_st.y0 >= HY_SCREEN_PX - bottom) {
        return;
    }
    s_consumed = true;
}

#if CONFIG_HY_DEBUG_CONSOLE
/* The same state changes as a real PRESSED then RELEASED, with no read samples in between */
void input_debug_stroke(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t ms)
{
    input_debug_swipe(x0, y0, x1, y1, ms, false);
}

/* consume: a scrolling widget under the finger asks for the stroke right after the press, as SCROLL_BEGIN does */
void input_debug_swipe(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t ms, bool consume)
{
    uint32_t now = lv_tick_get();
    s_active = true;
    lv_display_trigger_activity(NULL);
    s_waking = stroke_waking();
    screen_activity();
    s_consumed = toast_logged_visible();
    s_glide_stop = false;
    s_st.glide_stop = s_st.scroll_view = false;
    s_last_tap = false;
    s_st.x0 = x0;
    s_st.y0 = y0;
    s_st.t0_ms = now - ms;
    s_st.max_dev = 0;
    if (consume) {
        ui_input_consume();
    }
    finish(x1, y1, now);
}

/* A stroke held down: the press without the release. A due entry that arrives before input_debug_release
 * (remindtest) wakes the screen under the finger. */
void input_debug_hold(int16_t x0, int16_t y0)
{
    s_active = true;
    lv_display_trigger_activity(NULL);
    s_waking = stroke_waking();
    screen_activity();
    s_consumed = toast_logged_visible();
    s_glide_stop = false;
    s_st.glide_stop = s_st.scroll_view = false;
    s_last_tap = false;
    s_st.x0 = x0;
    s_st.y0 = y0;
    s_st.t0_ms = lv_tick_get();
    s_st.max_dev = 0;
    ESP_LOGI(TAG, "hold: stroke down at %d,%d%s", x0, y0, s_waking ? " (waking)" : "");
}

void input_debug_release(int16_t x1, int16_t y1)
{
    if (!s_active) {
        ESP_LOGW(TAG, "release: no held stroke");
        return;
    }
    finish(x1, y1, lv_tick_get());
}
#endif
