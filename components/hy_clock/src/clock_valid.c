#include "hy/clock_valid.h"

bool hy_clock_is_plausible(int64_t rtc_epoch, bool osc_stopped, int64_t build_epoch)
{
    return !osc_stopped && rtc_epoch >= build_epoch;
}
