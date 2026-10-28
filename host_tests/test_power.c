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
    return UNITY_END();
}
