#include <string.h>
#include "hy/types.h"

void hy_med_defaults(hy_med_t *m)
{
    memset(m, 0, sizeof(*m));
    m->type = HY_SCHED_FIXED;
    m->weekdays = 0x7F;
    m->color = 0;
    m->early_window_min = 60;
    m->late_after_min = 30;
    m->missed_after_min = 120;
}
