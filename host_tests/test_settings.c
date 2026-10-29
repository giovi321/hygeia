#include "unity.h"
#include "hy/settings.h"

void setUp(void) {}
void tearDown(void) {}

static void test_settings_defaults_and_clamp(void)
{
    hy_settings_t s;
    hy_settings_defaults(&s);
    TEST_ASSERT_TRUE(s.sound);
    TEST_ASSERT_FALSE(s.dnd);
    TEST_ASSERT_EQUAL_UINT8(60, s.volume);
    TEST_ASSERT_EQUAL_UINT8(80, s.brightness);
    TEST_ASSERT_EQUAL_UINT16(15, s.snooze_min);

    s.volume = 150; s.brightness = 0; s.snooze_min = 0;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT8(100, s.volume);
    TEST_ASSERT_EQUAL_UINT8(10, s.brightness);
    TEST_ASSERT_EQUAL_UINT16(1, s.snooze_min);
    s.snooze_min = 500;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT16(120, s.snooze_min);
    s.brightness = 200;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT8(100, s.brightness);
}

static void test_power_defaults(void)
{
    hy_settings_t s;
    hy_settings_defaults(&s);
    TEST_ASSERT_EQUAL_UINT16(30, s.dim_s);
    TEST_ASSERT_EQUAL_UINT16(120, s.off_bat_s);
    TEST_ASSERT_EQUAL_UINT32(0, s.off_usb_s);
}

static void test_power_clamp(void)
{
    hy_settings_t s;
    hy_settings_defaults(&s);
    s.dim_s = 5;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT16(10, s.dim_s);
    s.dim_s = 700;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT16(600, s.dim_s);

    hy_settings_defaults(&s);
    s.off_bat_s = 20;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT16(40, s.off_bat_s);
    hy_settings_defaults(&s);
    s.dim_s = 95; s.off_bat_s = 100;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT16(105, s.off_bat_s);
    hy_settings_defaults(&s);
    s.off_bat_s = 5000;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT16(3600, s.off_bat_s);

    hy_settings_defaults(&s);
    s.off_usb_s = 30;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT32(60, s.off_usb_s);
    s.off_usb_s = 100000;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT32(86400, s.off_usb_s);
    s.off_usb_s = 0;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT32(0, s.off_usb_s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_settings_defaults_and_clamp);
    RUN_TEST(test_power_defaults);
    RUN_TEST(test_power_clamp);
    return UNITY_END();
}
