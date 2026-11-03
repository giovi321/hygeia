#include "unity.h"
#include <string.h>
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
    TEST_ASSERT_EQUAL_UINT8(50, s.brightness);
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

static void test_quiet_defaults_clamp(void)
{
    hy_settings_t s;
    hy_settings_defaults(&s);
    TEST_ASSERT_EQUAL_UINT16(0, s.quiet_from);
    TEST_ASSERT_EQUAL_UINT16(0, s.quiet_to);
    s.quiet_from = 1440;
    s.quiet_to = 1439;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT16(0, s.quiet_from);
    TEST_ASSERT_EQUAL_UINT16(1439, s.quiet_to);
    s.quiet_to = 60000;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT16(0, s.quiet_to);
}

static void test_wifi_keep_default_false(void)
{
    hy_settings_t s;
    s.wifi_keep_batt = true;
    hy_settings_defaults(&s);
    TEST_ASSERT_FALSE(s.wifi_keep_batt);
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

static void test_settings_rot_defaults(void)
{
    hy_settings_t s;
    s.rot_lock = true;
    s.rot = 2;
    hy_settings_defaults(&s);
    TEST_ASSERT_FALSE(s.rot_lock);
    TEST_ASSERT_EQUAL_UINT8(0, s.rot);
}

static void test_settings_rot_clamp(void)
{
    hy_settings_t s;
    hy_settings_defaults(&s);
    s.rot = 7;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT8(0, s.rot);
    s.rot = 3;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT8(3, s.rot);
}

static void test_settings_cab_defaults(void)
{
    hy_settings_t s;
    memset(&s, 0xff, sizeof s);
    hy_settings_defaults(&s);
    TEST_ASSERT_FALSE(s.cab_on);
    TEST_ASSERT_EQUAL_UINT8(HY_CAB_ANY, s.cab_mode);
    TEST_ASSERT_TRUE(s.cab_chime);
    TEST_ASSERT_TRUE(s.cab_light);
    TEST_ASSERT_EQUAL_UINT8(3, s.cab_sens);
    TEST_ASSERT_EQUAL_UINT8(15, s.cab_window_min);
}

static void test_settings_cab_clamp(void)
{
    hy_settings_t s;
    hy_settings_defaults(&s);
    s.cab_mode = 7;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT8(HY_CAB_ANY, s.cab_mode);
    s.cab_mode = HY_CAB_TAUGHT;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT8(HY_CAB_TAUGHT, s.cab_mode);
    s.cab_sens = 0;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT8(3, s.cab_sens);
    s.cab_sens = 9;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT8(3, s.cab_sens);
    s.cab_sens = 5;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT8(5, s.cab_sens);
    s.cab_window_min = 200;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT8(120, s.cab_window_min);
    s.cab_window_min = 17;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT8(15, s.cab_window_min);
    s.cab_window_min = 0;
    hy_settings_clamp(&s);
    TEST_ASSERT_EQUAL_UINT8(0, s.cab_window_min);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_settings_defaults_and_clamp);
    RUN_TEST(test_quiet_defaults_clamp);
    RUN_TEST(test_wifi_keep_default_false);
    RUN_TEST(test_power_defaults);
    RUN_TEST(test_power_clamp);
    RUN_TEST(test_settings_rot_defaults);
    RUN_TEST(test_settings_rot_clamp);
    RUN_TEST(test_settings_cab_defaults);
    RUN_TEST(test_settings_cab_clamp);
    return UNITY_END();
}
