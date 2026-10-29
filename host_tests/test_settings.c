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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_settings_defaults_and_clamp);
    return UNITY_END();
}
