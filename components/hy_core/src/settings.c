#include "hy/settings.h"

void hy_settings_defaults(hy_settings_t *s)
{
    s->sound = true;
    s->dnd = false;
    s->volume = 60;
    s->brightness = 80;
    s->snooze_min = 15;
}

void hy_settings_clamp(hy_settings_t *s)
{
    if (s->volume > 100) s->volume = 100;
    if (s->brightness < 10) s->brightness = 10;
    if (s->brightness > 100) s->brightness = 100;
    if (s->snooze_min < 1) s->snooze_min = 1;
    if (s->snooze_min > 120) s->snooze_min = 120;
}
