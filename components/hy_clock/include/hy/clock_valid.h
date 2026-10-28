#ifndef HY_CLOCK_VALID_H
#define HY_CLOCK_VALID_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Pure plausibility check for a time read from the RTC; no ESP-IDF headers.
 * Plausible = the oscillator-stop flag is clear and rtc_epoch >= build_epoch
 * (a time equal to the build epoch counts as plausible). All values are UTC seconds.
 */
bool hy_clock_is_plausible(int64_t rtc_epoch, bool osc_stopped, int64_t build_epoch);

#endif
