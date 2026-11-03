#include "hy/settings.h"

void hy_settings_defaults(hy_settings_t *s)
{
    s->sound = true;
    s->dnd = false;
    s->volume = 60;
    s->brightness = 50;
    s->snooze_min = 15;
    s->dim_s = 30;
    s->off_bat_s = 120;
    s->off_usb_s = 0;
    s->rot_lock = false;
    s->rot = 0;
    s->quiet_from = 0;
    s->quiet_to = 0;
    s->wifi_keep_batt = false;
    s->cab_on = false;
    s->cab_mode = HY_CAB_ANY;
    s->cab_chime = true;
    s->cab_light = true;
    s->cab_sens = 3;
    s->cab_window_min = 15;
    s->wake_single_tap = false;
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
    if (s->rot > 3) s->rot = 0;
    if (s->quiet_from > 1439) s->quiet_from = 0;
    if (s->quiet_to > 1439) s->quiet_to = 0;
    if (s->cab_mode > HY_CAB_TAUGHT) s->cab_mode = HY_CAB_ANY;
    if (s->cab_sens < 1 || s->cab_sens > 5) s->cab_sens = 3;
    if (s->cab_window_min > HY_CAB_WINDOW_MAX_MIN) s->cab_window_min = HY_CAB_WINDOW_MAX_MIN;
    s->cab_window_min = (uint8_t)(s->cab_window_min - s->cab_window_min % HY_CAB_WINDOW_STEP_MIN);
}
