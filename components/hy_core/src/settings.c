#include "hy/settings.h"

void hy_settings_defaults(hy_settings_t *s)
{
    s->sound = true;
    s->dnd = false;
    s->volume = 60;
    s->brightness = 80;
    s->snooze_min = 15;
    s->dim_s = 30;
    s->off_bat_s = 120;
    s->off_usb_s = 0;
}

void hy_settings_clamp(hy_settings_t *s)
{
    if (s->volume > 100) s->volume = 100;
    if (s->brightness < 10) s->brightness = 10;
    if (s->brightness > 100) s->brightness = 100;
    if (s->snooze_min < 1) s->snooze_min = 1;
    if (s->snooze_min > 120) s->snooze_min = 120;
    if (s->dim_s < 10) s->dim_s = 10;
    if (s->dim_s > 600) s->dim_s = 600;
    if (s->off_bat_s < 30) s->off_bat_s = 30;
    if (s->off_bat_s > 3600) s->off_bat_s = 3600;
    if (s->off_bat_s < s->dim_s + 10) s->off_bat_s = (uint16_t)(s->dim_s + 10);
    if (s->off_usb_s != 0) {
        if (s->off_usb_s < 60) s->off_usb_s = 60;
        if (s->off_usb_s > 86400) s->off_usb_s = 86400;
    }
}
