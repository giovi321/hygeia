#include "hy/power.h"

#define PWRON_KNOWN_MASK 0x3F /* bits 0..5 are defined sources; 6..7 are not */
#define PWRON_SRC_KEY    0x01 /* bit 0: PWRON key low */
#define PWRON_SRC_EN     0x20 /* bit 5: EN mode, PWRON always high; fails open (R24) */

bool hy_power_on_allowed(uint8_t pwron_status, hy_reset_kind_t reset)
{
    if (reset != HY_RST_POWERON) {
        return true;
    }
    uint8_t known = pwron_status & PWRON_KNOWN_MASK;
    if (known == 0) {
        return true;
    }
    return (known & (PWRON_SRC_KEY | PWRON_SRC_EN)) != 0;
}

#define ST1_VBUS_GOOD 0x20
#define ST1_BAT_PRESENT 0x08
#define ST2_CHG_MASK 0x07

void hy_axp_decode_status(uint8_t st1, uint8_t st2, uint8_t pct_reg, uint16_t mv, hy_batt_raw_t *out)
{
    out->present = (st1 & ST1_BAT_PRESENT) != 0;
    out->vbus = (st1 & ST1_VBUS_GOOD) != 0;
    out->mv = mv;
    out->pct = (out->present && pct_reg <= 100) ? (int8_t)pct_reg : -1;
    uint8_t c = st2 & ST2_CHG_MASK;
    if (!out->present) {
        out->chg = HY_CHG_NONE;
    } else if (c <= 3) {
        out->chg = HY_CHG_CHARGING;
    } else if (c == 4) {
        out->chg = HY_CHG_DONE;
    } else {
        out->chg = HY_CHG_NONE;
    }
}

/* INTSTS2 (0x49) and INTSTS3 (0x4A) bit positions, see power.h */
uint32_t hy_axp_decode_irq(const uint8_t sts[3])
{
    uint32_t f = 0;
    if (sts[1] & 0x08) f |= HY_PMU_PKEY_SHORT;
    if (sts[1] & 0x04) f |= HY_PMU_PKEY_LONG;
    if (sts[1] & 0x80) f |= HY_PMU_VBUS_IN;
    if (sts[1] & 0x40) f |= HY_PMU_VBUS_OUT;
    if (sts[1] & 0x20) f |= HY_PMU_BAT_IN;
    if (sts[1] & 0x10) f |= HY_PMU_BAT_OUT;
    if (sts[2] & 0x08) f |= HY_PMU_CHG_START;
    if (sts[2] & 0x10) f |= HY_PMU_CHG_DONE;
    return f;
}

void hy_axp_irq_enable_mask(uint8_t en[3])
{
    en[0] = 0x00;
    en[1] = 0xFC; /* VBUS in/out, battery in/out, PWR short, PWR long */
    en[2] = 0x18; /* charge start, charge done */
}

uint8_t hy_axp_key_timing(uint8_t cur)
{
    /* keep bits 7..6 and the power-on hold (1..0); set IRQ time = 2 s (2 << 4), power-off hold = 10 s (3 << 2) */
    return (uint8_t)((cur & 0xC3) | (2u << 4) | (3u << 2));
}
