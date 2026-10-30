#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HY_RST_POWERON = 0, HY_RST_OTHER } hy_reset_kind_t; /* caller maps esp_reset_reason() */

/* Decide whether the device may keep running after reset.
 * pwron_status: raw AXP2101 PWRON status byte (register 0x20).
 * Only a power-on reset is gated: it must have been caused by the PWR key (bit 0). Any other reset
 * (software restart, panic, watchdog, flash/RTS reset) keeps running. A power-on reset with no known
 * source bit set (status 0x00 or reserved bits only) boots (ruling R22), so a PMU that reports nothing
 * still gives a working device. EN mode (bit 5) also boots (R24). */
bool hy_power_on_allowed(uint8_t pwron_status, hy_reset_kind_t reset);

/* AXP2101 registers. Addresses: XPowersLib (MIT, lewisxhe) src/REG/AXP2101Constants.h at commit
 * d6997586e68f65afd51baa775903df930db39821. Bit positions: src/XPowersAXP2101.hpp and
 * src/XPowersParams.hpp at the same commit. */
#define HY_AXP_REG_STATUS1      0x00 /* bit 5 VBUS good (isVbusGood), bit 3 battery present (isBatteryConnect) */
#define HY_AXP_REG_STATUS2      0x01 /* bits 2..0 charger status (getChargerStatus): 0 tri, 1 pre, 2 CC, 3 CV, 4 done, 5 stop */
#define HY_AXP_REG_IC_TYPE      0x03 /* chip id 0x4A */
#define HY_AXP_REG_COMMON_CONFIG 0x10 /* bit 0 set = power off */
#define HY_AXP_REG_PWRON_STATUS 0x20 /* power-on source */
#define HY_AXP_REG_PWROFF_EN    0x22 /* bit 0: 0 long press powers off (setLongPressPowerOFF), 1 restarts */
#define HY_AXP_REG_KEY_TIMING   0x27 /* IRQ_OFF_ON_LEVEL_CTRL: bits 5..4 IRQ time (setIrqLevelTime: 0 1 s, 1 1.5 s, 2 2 s, 3 2.5 s),
                                        bits 3..2 power-off hold (setPowerKeyPressOffTime: 0 4 s, 1 6 s, 2 8 s, 3 10 s),
                                        bits 1..0 power-on hold (setPowerKeyPressOnTime: 0 128 ms, 1 512 ms, 2 1 s, 3 2 s) */
#define HY_AXP_REG_ADC_CTRL     0x30 /* ADC_CHANNEL_CTRL: bit 0 battery voltage measure (enableBattVoltageMeasure) */
#define HY_AXP_REG_ADC_VBAT_H   0x34 /* readRegisterH5L8(0x34, 0x35): battery mV, 13 bits */
#define HY_AXP_REG_ADC_VBAT_L   0x35
#define HY_AXP_REG_INTEN1       0x40 /* INTEN1..3 use the INTSTS layout */
#define HY_AXP_REG_INTEN2       0x41
#define HY_AXP_REG_INTEN3       0x42
#define HY_AXP_REG_INTSTS1      0x48 /* write 1 to clear */
#define HY_AXP_REG_INTSTS2      0x49 /* bit 7 VBUS insert, 6 VBUS remove, 5 battery insert, 4 battery remove, 3 PWR short, 2 PWR long */
#define HY_AXP_REG_INTSTS3      0x4A /* bit 4 charge done, bit 3 charge start */
#define HY_AXP_REG_BAT_PERCENT  0xA4 /* BAT_PERCENT_DATA; getBatteryPercent returns -1 when no battery */

typedef enum { HY_CHG_NONE = 0, HY_CHG_CHARGING, HY_CHG_DONE } hy_chg_t; /* tri/pre/CC/CV -> CHARGING */
typedef struct {
    bool present;
    bool vbus;
    hy_chg_t chg;
    int8_t pct; /* -1 unknown */
    uint16_t mv;
} hy_batt_raw_t;

/* st1, st2: STATUS1/2; pct_reg: 0xA4; mv: from 0x34/0x35. pct = -1 when !present or pct_reg > 100;
 * chg = NONE when !present. vbus is STATUS1 bit 5 (VBUS good). */
void hy_axp_decode_status(uint8_t st1, uint8_t st2, uint8_t pct_reg, uint16_t mv, hy_batt_raw_t *out);

enum {
    HY_PMU_PKEY_SHORT = 1u << 0,
    HY_PMU_PKEY_LONG = 1u << 1,
    HY_PMU_VBUS_IN = 1u << 2,
    HY_PMU_VBUS_OUT = 1u << 3,
    HY_PMU_BAT_IN = 1u << 4,
    HY_PMU_BAT_OUT = 1u << 5,
    HY_PMU_CHG_START = 1u << 6,
    HY_PMU_CHG_DONE = 1u << 7
};
uint32_t hy_axp_decode_irq(const uint8_t sts[3]); /* sts = 0x48, 0x49, 0x4A */
void hy_axp_irq_enable_mask(uint8_t en[3]);       /* the 0x40..0x42 bytes enabling exactly these eight */
/* 0x27 value: IRQ (long press) 2 s, power-off hold 10 s, power-on hold and reserved bits 7..6 unchanged from `cur` */
uint8_t hy_axp_key_timing(uint8_t cur);

#ifdef __cplusplus
}
#endif
