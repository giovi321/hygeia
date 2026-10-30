#include "hy/screen.h"

void hy_screen_init(hy_screen_t *s, bool usb, uint32_t now_ms)
{
    s->state = HY_SCR_ON;
    s->activity_ms = now_ms;
    s->usb = usb;
    s->hold = false;
}

void hy_screen_set_hold(hy_screen_t *s, bool hold, uint32_t now_ms)
{
    if (s->hold && !hold) s->activity_ms = now_ms; /* the idle timer starts when the hold ends */
    s->hold = hold;
}

static void wake(hy_screen_t *s, uint32_t now_ms)
{
    s->state = HY_SCR_ON;
    s->activity_ms = now_ms;
}

hy_scr_t hy_screen_event(hy_screen_t *s, hy_scr_ev_t ev, uint32_t now_ms, const hy_scr_cfg_t *cfg)
{
    switch (ev) {
    case HY_SE_TOUCH:
    case HY_SE_USER_KEY:
    case HY_SE_DUE:
        wake(s, now_ms);
        break;
    case HY_SE_CHIME:
        if (s->usb) wake(s, now_ms);
        break;
    case HY_SE_USB_IN:
        s->usb = true;
        wake(s, now_ms);
        break;
    case HY_SE_USB_OUT:
        s->usb = false;
        s->activity_ms = now_ms;
        break;
    case HY_SE_PWR_SHORT:
        if (s->state == HY_SCR_OFF) wake(s, now_ms);
        else s->state = HY_SCR_OFF;
        break;
    case HY_SE_TICK:
        if (s->hold) {
            if (s->state != HY_SCR_OFF) wake(s, now_ms);
            break;
        }
        {
            uint32_t idle_ms = (uint32_t)(now_ms - s->activity_ms);
            if (s->state == HY_SCR_ON && idle_ms >= (uint32_t)cfg->dim_s * 1000u) s->state = HY_SCR_DIM;
            if (s->state == HY_SCR_DIM) {
                if (!s->usb && idle_ms >= (uint32_t)cfg->off_bat_s * 1000u) s->state = HY_SCR_OFF;
                else if (s->usb && cfg->off_usb_s > 0 && (uint64_t)idle_ms >= (uint64_t)cfg->off_usb_s * 1000u) s->state = HY_SCR_OFF;
            }
        }
        break;
    }
    return s->state;
}

bool hy_screen_may_sleep(const hy_screen_t *s)
{
    return s->state == HY_SCR_OFF && !s->usb;
}
