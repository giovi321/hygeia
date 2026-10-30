#include "unity.h"
#include "hy/screen.h"

void setUp(void) {}
void tearDown(void) {}

static const hy_scr_cfg_t CFG = { 30, 120, 0 };
#define S(x) ((uint32_t)(x) * 1000u)

static void test_dim_then_off_on_battery(void)
{
    hy_screen_t s; hy_screen_init(&s, false, 0);
    TEST_ASSERT_EQUAL(HY_SCR_ON, s.state);
    TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, HY_SE_TICK, S(29), &CFG));
    TEST_ASSERT_EQUAL(HY_SCR_DIM, hy_screen_event(&s, HY_SE_TICK, S(30), &CFG));
    TEST_ASSERT_EQUAL(HY_SCR_DIM, hy_screen_event(&s, HY_SE_TICK, S(119), &CFG));
    TEST_ASSERT_FALSE(hy_screen_may_sleep(&s));
    TEST_ASSERT_EQUAL(HY_SCR_OFF, hy_screen_event(&s, HY_SE_TICK, S(120), &CFG));
    TEST_ASSERT_TRUE(hy_screen_may_sleep(&s));
}

static void test_usb_never_off(void)
{
    hy_screen_t s; hy_screen_init(&s, true, 0);
    TEST_ASSERT_EQUAL(HY_SCR_DIM, hy_screen_event(&s, HY_SE_TICK, S(3600), &CFG));
    TEST_ASSERT_FALSE(hy_screen_may_sleep(&s));

    hy_scr_cfg_t c = { 30, 120, 600 };
    hy_screen_init(&s, true, 0);
    TEST_ASSERT_EQUAL(HY_SCR_DIM, hy_screen_event(&s, HY_SE_TICK, S(599), &c));
    TEST_ASSERT_EQUAL(HY_SCR_OFF, hy_screen_event(&s, HY_SE_TICK, S(600), &c));
    TEST_ASSERT_FALSE(hy_screen_may_sleep(&s));
}

static void test_pwr_toggles(void)
{
    hy_screen_t s; hy_screen_init(&s, false, 0);
    TEST_ASSERT_EQUAL(HY_SCR_OFF, hy_screen_event(&s, HY_SE_PWR_SHORT, S(1), &CFG));
    TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, HY_SE_PWR_SHORT, S(2), &CFG));
    TEST_ASSERT_EQUAL_UINT32(S(2), s.activity_ms);
    hy_screen_event(&s, HY_SE_TICK, S(40), &CFG);
    TEST_ASSERT_EQUAL(HY_SCR_DIM, s.state);
    TEST_ASSERT_EQUAL(HY_SCR_OFF, hy_screen_event(&s, HY_SE_PWR_SHORT, S(41), &CFG));
}

static void test_wake_events(void)
{
    hy_scr_ev_t evs[] = { HY_SE_TOUCH, HY_SE_USER_KEY, HY_SE_DUE, HY_SE_CHIME, HY_SE_USB_IN };
    for (size_t i = 0; i < sizeof evs / sizeof evs[0]; i++) {
        hy_screen_t s; hy_screen_init(&s, true, 0);   /* usb: CHIME wakes only on USB */
        hy_screen_event(&s, HY_SE_PWR_SHORT, S(1), &CFG);
        TEST_ASSERT_EQUAL(HY_SCR_OFF, s.state);
        TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, evs[i], S(50), &CFG));
        TEST_ASSERT_EQUAL_UINT32(S(50), s.activity_ms);
    }
    hy_screen_t s; hy_screen_init(&s, false, 0);
    hy_screen_event(&s, HY_SE_PWR_SHORT, S(1), &CFG);
    TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, HY_SE_USB_IN, S(5), &CFG));
    TEST_ASSERT_TRUE(s.usb);
}

static void test_chime_on_battery_no_wake(void)
{
    hy_screen_t s; hy_screen_init(&s, false, 0);
    hy_screen_event(&s, HY_SE_PWR_SHORT, S(1), &CFG);
    TEST_ASSERT_EQUAL(HY_SCR_OFF, hy_screen_event(&s, HY_SE_CHIME, S(50), &CFG));
    TEST_ASSERT_EQUAL_UINT32(0, s.activity_ms);

    hy_screen_init(&s, false, 0);
    hy_screen_event(&s, HY_SE_TICK, S(30), &CFG);
    TEST_ASSERT_EQUAL(HY_SCR_DIM, s.state);
    TEST_ASSERT_EQUAL(HY_SCR_DIM, hy_screen_event(&s, HY_SE_CHIME, S(40), &CFG));
    TEST_ASSERT_EQUAL_UINT32(0, s.activity_ms);

    hy_screen_init(&s, false, 0);
    TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, HY_SE_CHIME, S(20), &CFG));
    TEST_ASSERT_EQUAL_UINT32(0, s.activity_ms);
    TEST_ASSERT_EQUAL(HY_SCR_DIM, hy_screen_event(&s, HY_SE_TICK, S(30), &CFG));
}

static void test_unplug_restarts_timer(void)
{
    hy_screen_t s; hy_screen_init(&s, true, 0);
    hy_screen_event(&s, HY_SE_TICK, S(600), &CFG);
    TEST_ASSERT_EQUAL(HY_SCR_DIM, s.state);
    TEST_ASSERT_EQUAL(HY_SCR_DIM, hy_screen_event(&s, HY_SE_USB_OUT, S(600), &CFG));
    TEST_ASSERT_FALSE(s.usb);
    TEST_ASSERT_EQUAL(HY_SCR_DIM, hy_screen_event(&s, HY_SE_TICK, S(600 + 119), &CFG));
    TEST_ASSERT_EQUAL(HY_SCR_OFF, hy_screen_event(&s, HY_SE_TICK, S(600 + 120), &CFG));
}

static void test_hold(void)
{
    hy_screen_t s; hy_screen_init(&s, true, 0);   /* usb: CHIME wakes only on USB */
    hy_screen_set_hold(&s, true, 0);
    TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, HY_SE_TICK, S(3600), &CFG));
    TEST_ASSERT_EQUAL(HY_SCR_OFF, hy_screen_event(&s, HY_SE_PWR_SHORT, S(3601), &CFG));
    TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, HY_SE_CHIME, S(3602), &CFG));
    hy_screen_set_hold(&s, false, S(3700));
    TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, HY_SE_TICK, S(3729), &CFG));
    TEST_ASSERT_EQUAL(HY_SCR_DIM, hy_screen_event(&s, HY_SE_TICK, S(3730), &CFG));
}

/* R1: hold means held ON. An overlay (countdown, power-off confirmation) or a due card on USB that appears on a
   dimmed screen brings it back ON at the next tick; PWR short still turns it OFF and the hold keeps it OFF (the
   countdown keeps running behind a dark panel); PWR short again turns it ON */
static void test_hold_restores_dim_keeps_off(void)
{
    hy_screen_t s; hy_screen_init(&s, false, 0);
    TEST_ASSERT_EQUAL(HY_SCR_DIM, hy_screen_event(&s, HY_SE_TICK, S(30), &CFG));
    hy_screen_set_hold(&s, true, S(31));
    TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, HY_SE_TICK, S(31), &CFG));
    TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, HY_SE_TICK, S(600), &CFG));
    TEST_ASSERT_EQUAL(HY_SCR_OFF, hy_screen_event(&s, HY_SE_PWR_SHORT, S(601), &CFG));
    TEST_ASSERT_EQUAL(HY_SCR_OFF, hy_screen_event(&s, HY_SE_TICK, S(602), &CFG));
    TEST_ASSERT_EQUAL(HY_SCR_OFF, hy_screen_event(&s, HY_SE_CHIME, S(603), &CFG));   /* battery: no wake */
    TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, HY_SE_PWR_SHORT, S(604), &CFG));
    hy_screen_set_hold(&s, false, S(610));
    TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, HY_SE_TICK, S(639), &CFG));
    TEST_ASSERT_EQUAL(HY_SCR_DIM, hy_screen_event(&s, HY_SE_TICK, S(640), &CFG));
}

static void test_wrap(void)
{
    hy_screen_t s; hy_screen_init(&s, false, 0xFFFFF000u);
    TEST_ASSERT_EQUAL(HY_SCR_ON, hy_screen_event(&s, HY_SE_TICK, 0x00001000u, &CFG));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dim_then_off_on_battery);
    RUN_TEST(test_usb_never_off);
    RUN_TEST(test_pwr_toggles);
    RUN_TEST(test_wake_events);
    RUN_TEST(test_chime_on_battery_no_wake);
    RUN_TEST(test_unplug_restarts_timer);
    RUN_TEST(test_hold);
    RUN_TEST(test_hold_restores_dim_keeps_off);
    RUN_TEST(test_wrap);
    return UNITY_END();
}
