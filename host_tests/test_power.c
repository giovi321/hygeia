#include "unity.h"
#include "hy/power.h"

#define SRC_KEY  0x01 /* PWRON key low */
#define SRC_IRQ  0x02
#define SRC_VBUS 0x04
#define SRC_BATG 0x08 /* battery charged / normal */
#define SRC_BATI 0x10 /* battery insert */
#define SRC_EN   0x20

void setUp(void) {}
void tearDown(void) {}

static void test_key_on_poweron_boots(void)
{
    TEST_ASSERT_TRUE(hy_power_on_allowed(SRC_KEY, HY_RST_POWERON));
}

static void test_vbus_insert_on_poweron_stays_off(void)
{
    TEST_ASSERT_FALSE(hy_power_on_allowed(SRC_VBUS, HY_RST_POWERON));
}

static void test_battery_insert_on_poweron_stays_off(void)
{
    TEST_ASSERT_FALSE(hy_power_on_allowed(SRC_BATI, HY_RST_POWERON));
    TEST_ASSERT_FALSE(hy_power_on_allowed(SRC_BATG, HY_RST_POWERON));
}

static void test_other_sources_on_poweron_stay_off(void)
{
    TEST_ASSERT_FALSE(hy_power_on_allowed(SRC_IRQ, HY_RST_POWERON));
}

/* Ruling R24: EN mode (PWRON always high) alone or with non-key bits fails open */
static void test_en_mode_on_poweron_boots(void)
{
    TEST_ASSERT_TRUE(hy_power_on_allowed(SRC_EN, HY_RST_POWERON));
    TEST_ASSERT_TRUE(hy_power_on_allowed(SRC_EN | SRC_VBUS, HY_RST_POWERON));
    TEST_ASSERT_TRUE(hy_power_on_allowed(SRC_EN | SRC_BATI | SRC_IRQ, HY_RST_POWERON));
}

static void test_key_with_other_source_boots(void)
{
    TEST_ASSERT_TRUE(hy_power_on_allowed(SRC_KEY | SRC_VBUS, HY_RST_POWERON));
}

static void test_any_status_on_other_reset_keeps_running(void)
{
    for (int s = 0; s < 256; s++) {
        TEST_ASSERT_TRUE(hy_power_on_allowed((uint8_t)s, HY_RST_OTHER));
    }
}

/* Ruling R22: nothing reported on a power-on reset means boot */
static void test_zero_status_on_poweron_boots(void)
{
    TEST_ASSERT_TRUE(hy_power_on_allowed(0x00, HY_RST_POWERON));
}

static void test_reserved_bits_only_on_poweron_boots(void)
{
    TEST_ASSERT_TRUE(hy_power_on_allowed(0xC0, HY_RST_POWERON));
}

#define ST1_VBUS 0x20
#define ST1_BAT  0x08
#define ALL_FLAGS (HY_PMU_PKEY_SHORT | HY_PMU_PKEY_LONG | HY_PMU_VBUS_IN | HY_PMU_VBUS_OUT |                    HY_PMU_BAT_IN | HY_PMU_BAT_OUT | HY_PMU_CHG_START | HY_PMU_CHG_DONE)

static void test_decode_status_battery_charging(void)
{
    hy_batt_raw_t r;
    hy_axp_decode_status(ST1_VBUS | ST1_BAT, 0x02, 0x4E, 3950, &r);
    TEST_ASSERT_TRUE(r.present);
    TEST_ASSERT_TRUE(r.vbus);
    TEST_ASSERT_EQUAL_INT(HY_CHG_CHARGING, r.chg);
    TEST_ASSERT_EQUAL_INT(78, r.pct);
    TEST_ASSERT_EQUAL_UINT16(3950, r.mv);
}

static void test_decode_status_done_and_none(void)
{
    hy_batt_raw_t r;
    hy_axp_decode_status(ST1_BAT, 0x04, 100, 4200, &r);
    TEST_ASSERT_EQUAL_INT(HY_CHG_DONE, r.chg);
    hy_axp_decode_status(ST1_BAT, 0x05, 50, 3800, &r);
    TEST_ASSERT_EQUAL_INT(HY_CHG_NONE, r.chg);
    for (int c = 0; c <= 3; c++) {
        hy_axp_decode_status(ST1_BAT, (uint8_t)c, 50, 3800, &r);
        TEST_ASSERT_EQUAL_INT(HY_CHG_CHARGING, r.chg);
    }
    /* upper STATUS2 bits do not change the charger field */
    hy_axp_decode_status(ST1_BAT, 0xE2, 50, 3800, &r);
    TEST_ASSERT_EQUAL_INT(HY_CHG_CHARGING, r.chg);
}

static void test_decode_status_no_battery(void)
{
    hy_batt_raw_t r;
    hy_axp_decode_status(ST1_VBUS, 0x02, 0x4E, 3950, &r);
    TEST_ASSERT_FALSE(r.present);
    TEST_ASSERT_TRUE(r.vbus);
    TEST_ASSERT_EQUAL_INT(-1, r.pct);
    TEST_ASSERT_EQUAL_INT(HY_CHG_NONE, r.chg);
    hy_axp_decode_status(0x00, 0x02, 0x4E, 0, &r);
    TEST_ASSERT_FALSE(r.vbus);
}

static void test_decode_status_bad_pct(void)
{
    hy_batt_raw_t r;
    hy_axp_decode_status(ST1_BAT, 0x02, 101, 3950, &r);
    TEST_ASSERT_EQUAL_INT(-1, r.pct);
    hy_axp_decode_status(ST1_BAT, 0x02, 0xFF, 3950, &r);
    TEST_ASSERT_EQUAL_INT(-1, r.pct);
    hy_axp_decode_status(ST1_BAT, 0x02, 100, 3950, &r);
    TEST_ASSERT_EQUAL_INT(100, r.pct);
    hy_axp_decode_status(ST1_BAT, 0x02, 0, 3950, &r);
    TEST_ASSERT_EQUAL_INT(0, r.pct);
}

static void test_decode_irq_each_bit(void)
{
    uint8_t z[3] = {0, 0, 0};
    TEST_ASSERT_EQUAL_UINT32(0, hy_axp_decode_irq(z));
    uint8_t a[3] = {0x00, 0x08, 0x00};
    TEST_ASSERT_EQUAL_UINT32(HY_PMU_PKEY_SHORT, hy_axp_decode_irq(a));
    uint8_t b[3] = {0x00, 0x04, 0x00};
    TEST_ASSERT_EQUAL_UINT32(HY_PMU_PKEY_LONG, hy_axp_decode_irq(b));
    uint8_t c[3] = {0x00, 0x80, 0x00};
    TEST_ASSERT_EQUAL_UINT32(HY_PMU_VBUS_IN, hy_axp_decode_irq(c));
    uint8_t d[3] = {0x00, 0x40, 0x00};
    TEST_ASSERT_EQUAL_UINT32(HY_PMU_VBUS_OUT, hy_axp_decode_irq(d));
    uint8_t e[3] = {0x00, 0x20, 0x00};
    TEST_ASSERT_EQUAL_UINT32(HY_PMU_BAT_IN, hy_axp_decode_irq(e));
    uint8_t f[3] = {0x00, 0x10, 0x00};
    TEST_ASSERT_EQUAL_UINT32(HY_PMU_BAT_OUT, hy_axp_decode_irq(f));
    uint8_t g[3] = {0x00, 0x00, 0x08};
    TEST_ASSERT_EQUAL_UINT32(HY_PMU_CHG_START, hy_axp_decode_irq(g));
    uint8_t h[3] = {0x00, 0x00, 0x10};
    TEST_ASSERT_EQUAL_UINT32(HY_PMU_CHG_DONE, hy_axp_decode_irq(h));
    uint8_t o[3] = {0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_UINT32(ALL_FLAGS, hy_axp_decode_irq(o));
    uint8_t u[3] = {0xFF, 0x03, 0xE7}; /* only unwatched bits */
    TEST_ASSERT_EQUAL_UINT32(0, hy_axp_decode_irq(u));
}

static void test_irq_enable_mask_roundtrip(void)
{
    uint8_t en[3] = {0xFF, 0xFF, 0xFF};
    hy_axp_irq_enable_mask(en);
    TEST_ASSERT_EQUAL_UINT8(0x00, en[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFC, en[1]);
    TEST_ASSERT_EQUAL_UINT8(0x18, en[2]);
    TEST_ASSERT_EQUAL_UINT32(ALL_FLAGS, hy_axp_decode_irq(en));
}

static void test_key_timing(void)
{
    uint8_t v = hy_axp_key_timing(0x00);
    TEST_ASSERT_EQUAL_UINT8(2, (v >> 4) & 3);
    TEST_ASSERT_EQUAL_UINT8(3, (v >> 2) & 3);
    TEST_ASSERT_EQUAL_UINT8(0, v & 3);
    v = hy_axp_key_timing(0x03);
    TEST_ASSERT_EQUAL_UINT8(3, v & 3);
    v = hy_axp_key_timing(0xC0 | 0x03);
    TEST_ASSERT_EQUAL_UINT8(0xC0, v & 0xC0);
    TEST_ASSERT_EQUAL_UINT8(3, v & 3);
    TEST_ASSERT_EQUAL_UINT8(0xFF & (0xC0 | (2 << 4) | (3 << 2) | 3), v);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_key_on_poweron_boots);
    RUN_TEST(test_vbus_insert_on_poweron_stays_off);
    RUN_TEST(test_battery_insert_on_poweron_stays_off);
    RUN_TEST(test_other_sources_on_poweron_stay_off);
    RUN_TEST(test_en_mode_on_poweron_boots);
    RUN_TEST(test_key_with_other_source_boots);
    RUN_TEST(test_any_status_on_other_reset_keeps_running);
    RUN_TEST(test_zero_status_on_poweron_boots);
    RUN_TEST(test_reserved_bits_only_on_poweron_boots);
    RUN_TEST(test_decode_status_battery_charging);
    RUN_TEST(test_decode_status_done_and_none);
    RUN_TEST(test_decode_status_no_battery);
    RUN_TEST(test_decode_status_bad_pct);
    RUN_TEST(test_decode_irq_each_bit);
    RUN_TEST(test_irq_enable_mask_roundtrip);
    RUN_TEST(test_key_timing);
    return UNITY_END();
}
