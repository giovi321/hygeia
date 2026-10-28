#include "hy/power.h"

#define PWRON_KNOWN_MASK 0x3F /* bits 0..5 are defined sources; 6..7 are not */
#define PWRON_SRC_KEY    0x01 /* bit 0: PWRON key low */

bool hy_power_on_allowed(uint8_t pwron_status, hy_reset_kind_t reset)
{
    if (reset != HY_RST_POWERON) {
        return true;
    }
    uint8_t known = pwron_status & PWRON_KNOWN_MASK;
    if (known == 0) {
        return true;
    }
    return (known & PWRON_SRC_KEY) != 0;
}
