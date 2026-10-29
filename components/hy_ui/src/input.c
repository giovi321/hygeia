/* Touch input layer: every stroke from press to release becomes an hy_stroke_t, is classified on release
 * (hy_gesture_classify) and drives the navigation (ui_nav_gesture) unless a widget consumed it.
 *
 * Stroke boundaries come from the indev PRESSED and RELEASED events, which LVGL sends before the pressed
 * widget gets its own RELEASED / SHORT_CLICKED, so a tile's click handler can ask ui_input_last_was_tap().
 * LVGL does not forward PRESSING to indev event callbacks, so max_dev (P24) is tracked in a wrapper around
 * the indev read callback, which sees every touch sample. */

#include "esp_log.h"
#include "ui_internal.h"

static const char *TAG = "ui_input";

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
    /* data->point is the raw sample, before LVGL rotates it into display coordinates (lv_display_rotate_point
     * in lv_indev.c), while x0/y0 come from the rotated lv_indev_get_point. They agree only because the
     * display is registered with ESP_LV_ADAPTER_ROTATE_0 (board.c; swap and mirror happen in the touch
     * driver). A rotated display must rotate a copy of the point here first. */
    if (down && s_active) {
        uint16_t d = dev_of(data->point.x, data->point.y);
        if (d > s_st.max_dev) {
            s_st.max_dev = d;
        }
    }
    s_phys_down = down;
}

static void finish(int32_t x, int32_t y, uint32_t t);

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
        s_waking = ui_nav_idle();
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
 * SHORT_CLICKED that LVGL sends next to the pressed tile does nothing (Review Focus 3). */
static void finish(int32_t x, int32_t y, uint32_t t)
{
    s_active = false;
    s_st.x1 = (int16_t)x;
    s_st.y1 = (int16_t)y;
    s_st.t1_ms = t;
    uint16_t d = dev_of(x, y);
    if (d > s_st.max_dev) {
        s_st.max_dev = d;
    }
    hy_gesture_t g = hy_gesture_classify(&s_st);
    s_last_tap = g == HY_G_TAP;
    ESP_LOGI(TAG, "stroke dx=%d dy=%d ms=%u -> %s (max_dev %u%s)", s_st.x1 - s_st.x0, s_st.y1 - s_st.y0,
             (unsigned)(s_st.t1_ms - s_st.t0_ms), ui_gesture_name(g), (unsigned)s_st.max_dev,
             s_consumed ? ", consumed" : "");
    if (!s_consumed || s_waking) {
        /* NONE too: on the dimmed screen every touch wakes */
        if (ui_nav_gesture(g) == HY_FX_WAKE || s_waking) {
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
    return !s_waking && s_st.max_dev <= HY_TAP_SLOP_PX;
}

void ui_input_consume(void)
{
    s_consumed = true;
}

#if CONFIG_HY_DEBUG_CONSOLE
/* The same state changes as a real PRESSED then RELEASED, with no read samples in between */
void input_debug_stroke(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t ms)
{
    uint32_t now = lv_tick_get();
    s_active = true;
    s_waking = ui_nav_idle();
    s_consumed = toast_logged_visible();
    s_last_tap = false;
    s_st.x0 = x0;
    s_st.y0 = y0;
    s_st.t0_ms = now - ms;
    s_st.max_dev = 0;
    finish(x1, y1, now);
}
#endif
