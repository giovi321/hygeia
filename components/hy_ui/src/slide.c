#include "slide.h"

#include "esp_log.h"
#include "ui_internal.h"

#define COMPLETE_PCT 90
#define BACK_MS      150

static void x_exec(void *o, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)o, v);
}

void slide_reset(slide_t *s)
{
    lv_anim_delete(s->knob, x_exec);
    lv_obj_set_x(s->knob, SLIDE_KNOB_PAD);
    s->off = 0;
    s->drag = false;
}

static void knob_back(slide_t *s)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s->knob);
    lv_anim_set_exec_cb(&a, x_exec);
    lv_anim_set_values(&a, lv_obj_get_x(s->knob), SLIDE_KNOB_PAD);
    lv_anim_set_duration(&a, BACK_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    s->off = 0;
}

void slide_end(slide_t *s, bool released)
{
    s->drag = false;
    bool complete = released && s->off * 100 >= SLIDE_TRAVEL * COMPLETE_PCT;
    ESP_LOGI(s->tag, "knob released at %d of %d px: %s", (int)s->off, SLIDE_TRAVEL, complete ? "complete" : "back");
    if (complete) {
        slide_reset(s);
    } else {
        knob_back(s);
    }
    s->end(complete);
}

/* The knob owns its stroke from the first sample (PRESSED and PRESSING both consume: whichever of them the input
 * layer sees after its own PRESSED handler is the one that sticks). A stroke that only wakes the dimmed screen
 * never moves it: the owner's begin refuses it. */
static void knob_cb(lv_event_t *e)
{
    slide_t *s = lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    lv_point_t p;
    switch (c) {
    case LV_EVENT_PRESSED:
        if (!s->begin()) {
            return;
        }
        s->drag = true;
        ui_input_consume();
        lv_anim_delete(s->knob, x_exec);
        s->off = lv_obj_get_x(s->knob) - SLIDE_KNOB_PAD;
        lv_indev_get_point(lv_event_get_indev(e), &p);
        s->x0 = p.x;
        s->off0 = s->off;
        break;
    case LV_EVENT_PRESSING: {
        if (!s->drag) {
            return;
        }
        ui_input_consume();
        lv_indev_get_point(lv_event_get_indev(e), &p);
        int32_t off = s->off0 + (p.x - s->x0);
        s->off = off < 0 ? 0 : off > SLIDE_TRAVEL ? SLIDE_TRAVEL : off;
        lv_obj_set_x(s->knob, SLIDE_KNOB_PAD + s->off);
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (s->drag) {
            slide_end(s, c == LV_EVENT_RELEASED);
        }
        break;
    default:
        break;
    }
}

void slide_attach(slide_t *s)
{
    lv_obj_add_event_cb(s->knob, knob_cb, LV_EVENT_PRESSED, s);
    lv_obj_add_event_cb(s->knob, knob_cb, LV_EVENT_PRESSING, s);
    lv_obj_add_event_cb(s->knob, knob_cb, LV_EVENT_RELEASED, s);
    lv_obj_add_event_cb(s->knob, knob_cb, LV_EVENT_PRESS_LOST, s);
}

#if CONFIG_HY_DEBUG_CONSOLE
void slide_debug_grab(slide_t *s)
{
    s->drag = true;
    s->off = SLIDE_TRAVEL;
    lv_obj_set_x(s->knob, SLIDE_KNOB_PAD + s->off);
}
#endif
