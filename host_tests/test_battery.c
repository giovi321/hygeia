#include <string.h>

#include "unity.h"
#include "hy/battery.h"

void setUp(void) {}
void tearDown(void) {}

#define T0 1000000

static hy_batt_t B;

static uint32_t feed(bool vbus, int pct, int64_t now)
{
    hy_batt_raw_t r = {.present = true, .vbus = vbus, .chg = HY_CHG_NONE, .pct = (int8_t)pct, .mv = 3800};
    return hy_batt_update(&B, &r, now);
}

#define EVS (HY_BATT_EV_LOW | HY_BATT_EV_CRIT)

static void test_shown_follows_raw_on_usb(void)
{
    hy_batt_init(&B, 0);
    feed(true, 50, T0);
    TEST_ASSERT_EQUAL_INT(50, B.pct);
    feed(true, 52, T0 + 60);
    TEST_ASSERT_EQUAL_INT(52, B.pct);
}

static void test_no_rise_while_discharging(void)
{
    hy_batt_init(&B, 0);
    feed(false, 60, T0);
    TEST_ASSERT_EQUAL_INT(60, B.pct);
    feed(false, 61, T0 + 60);
    TEST_ASSERT_EQUAL_INT(60, B.pct);
    feed(false, 62, T0 + 120);
    TEST_ASSERT_EQUAL_INT(60, B.pct);
    feed(false, 59, T0 + 180);
    TEST_ASSERT_EQUAL_INT(59, B.pct);
    feed(false, 63, T0 + 240);
    TEST_ASSERT_EQUAL_INT(63, B.pct);
}

static void test_low_once_per_crossing(void)
{
    hy_batt_init(&B, 0);
    TEST_ASSERT_EQUAL_UINT32(0, feed(false, 16, T0) & EVS);
    TEST_ASSERT_TRUE(feed(false, 15, T0 + 1) & HY_BATT_EV_LOW);
    TEST_ASSERT_FALSE(feed(false, 14, T0 + 2) & HY_BATT_EV_LOW);
    TEST_ASSERT_FALSE(feed(false, 13, T0 + 3) & HY_BATT_EV_LOW);
    /* rise of 4 over 13 is accepted as a swap, shown 17: still below the re-arm level */
    TEST_ASSERT_FALSE(feed(false, 17, T0 + 4) & HY_BATT_EV_LOW);
    TEST_ASSERT_EQUAL_INT(17, B.pct);
    /* a rise of 1 is ignored while discharging, so 18 never shows from 17 */
    feed(false, 18, T0 + 5);
    TEST_ASSERT_EQUAL_INT(17, B.pct);
    /* rise of 4 is accepted: 21 re-arms low */
    TEST_ASSERT_FALSE(feed(false, 21, T0 + 6) & HY_BATT_EV_LOW);
    TEST_ASSERT_EQUAL_INT(21, B.pct);
    TEST_ASSERT_TRUE(feed(false, 15, T0 + 7) & HY_BATT_EV_LOW);
}

static void test_no_events_on_usb(void)
{
    hy_batt_init(&B, 0);
    TEST_ASSERT_EQUAL_UINT32(0, feed(true, 4, T0) & EVS);
    uint32_t f = feed(false, 4, T0 + 60);
    TEST_ASSERT_TRUE(f & HY_BATT_EV_LOW);
    TEST_ASSERT_TRUE(f & HY_BATT_EV_CRIT);
}

static void test_usb_rearms(void)
{
    hy_batt_init(&B, 0);
    TEST_ASSERT_TRUE(feed(false, 15, T0) & HY_BATT_EV_LOW);
    TEST_ASSERT_EQUAL_UINT32(0, feed(true, 15, T0 + 60) & EVS);
    TEST_ASSERT_TRUE(feed(false, 15, T0 + 120) & HY_BATT_EV_LOW);
}

static void test_crit_once(void)
{
    hy_batt_init(&B, 0);
    uint32_t f = feed(false, 6, T0);
    TEST_ASSERT_FALSE(f & HY_BATT_EV_CRIT);
    TEST_ASSERT_TRUE(f & HY_BATT_EV_LOW); /* 6 is already below 15 and low was armed at boot */
    f = feed(false, 5, T0 + 60);
    TEST_ASSERT_TRUE(f & HY_BATT_EV_CRIT);
    TEST_ASSERT_FALSE(f & HY_BATT_EV_LOW); /* low already fired */
    TEST_ASSERT_EQUAL_UINT32(0, feed(false, 4, T0 + 120) & EVS);
    feed(false, 8, T0 + 180); /* rise of 4 over 4: accepted, re-arms critical */
    TEST_ASSERT_EQUAL_INT(8, B.pct);
    f = feed(false, 5, T0 + 240);
    TEST_ASSERT_TRUE(f & HY_BATT_EV_CRIT);
    TEST_ASSERT_FALSE(f & HY_BATT_EV_LOW); /* low stays disarmed below 18% */
}

static void test_unknown_battery(void)
{
    hy_batt_raw_t r = {.present = false, .vbus = false, .chg = HY_CHG_NONE, .pct = -1, .mv = 0};
    char buf[16];
    bool red = true, bolt = true;
    hy_batt_init(&B, 0);
    uint32_t f = hy_batt_update(&B, &r, T0);
    TEST_ASSERT_EQUAL_INT(-1, B.pct);
    TEST_ASSERT_EQUAL_UINT32(0, f & EVS);
    hy_batt_text(&B, buf, sizeof buf, "--", &red, &bolt);
    TEST_ASSERT_EQUAL_STRING("--", buf);
    TEST_ASSERT_FALSE(red);
    TEST_ASSERT_FALSE(bolt);
}

static void test_text_red_and_bolt(void)
{
    char buf[16];
    bool red, bolt;
    hy_batt_init(&B, 0);
    feed(false, 14, T0);
    hy_batt_text(&B, buf, sizeof buf, "--", &red, &bolt);
    TEST_ASSERT_EQUAL_STRING("14%", buf);
    TEST_ASSERT_TRUE(red);
    TEST_ASSERT_FALSE(bolt);
    hy_batt_init(&B, 0);
    feed(false, 15, T0);
    hy_batt_text(&B, buf, sizeof buf, "--", &red, &bolt);
    TEST_ASSERT_FALSE(red);
    hy_batt_raw_t r = {.present = true, .vbus = true, .chg = HY_CHG_CHARGING, .pct = 78, .mv = 4000};
    hy_batt_update(&B, &r, T0 + 60);
    hy_batt_text(&B, buf, sizeof buf, "--", &red, &bolt);
    TEST_ASSERT_EQUAL_STRING("78%", buf);
    TEST_ASSERT_TRUE(bolt);
    r.chg = HY_CHG_DONE;
    hy_batt_update(&B, &r, T0 + 120);
    hy_batt_text(&B, buf, sizeof buf, "--", &red, &bolt);
    TEST_ASSERT_FALSE(bolt);
}

static void test_changed_flag(void)
{
    hy_batt_init(&B, 0);
    TEST_ASSERT_TRUE(feed(false, 50, T0) & HY_BATT_CHANGED);
    TEST_ASSERT_FALSE(feed(false, 50, T0 + 60) & HY_BATT_CHANGED);
    TEST_ASSERT_TRUE(feed(true, 50, T0 + 120) & HY_BATT_CHANGED);
    TEST_ASSERT_TRUE(feed(true, 51, T0 + 180) & HY_BATT_CHANGED);
}

static void test_estimate_hidden_then_shown(void)
{
    char buf[16];
    hy_batt_init(&B, 0);
    feed(false, 80, T0);
    feed(false, 70, T0 + 2 * 3600);
    TEST_ASSERT_FALSE(hy_batt_estimate(&B, buf, sizeof buf));
    feed(false, 70, T0 + 4 * 3600);
    TEST_ASSERT_EQUAL_INT32(250, B.rate_cph);
    TEST_ASSERT_TRUE(hy_batt_estimate(&B, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("~28 h", buf);
    feed(false, 60, T0 + 8 * 3600);
    TEST_ASSERT_EQUAL_INT32(250, B.rate_cph);
    TEST_ASSERT_TRUE(hy_batt_estimate(&B, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("~24 h", buf);
}

static void test_estimate_days_and_under_hour(void)
{
    char buf[16];
    hy_batt_init(&B, 100);
    feed(false, 78, T0);
    TEST_ASSERT_TRUE(hy_batt_estimate(&B, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("~3 d", buf);
    hy_batt_init(&B, 2000);
    feed(false, 15, T0);
    TEST_ASSERT_TRUE(hy_batt_estimate(&B, buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("<1 h", buf);
}

static void test_estimate_hidden_on_usb(void)
{
    char buf[16];
    hy_batt_init(&B, 100);
    feed(true, 78, T0);
    TEST_ASSERT_FALSE(hy_batt_estimate(&B, buf, sizeof buf));
    feed(false, 78, T0 + 60);
    TEST_ASSERT_EQUAL_INT32(100, B.rate_cph);
    TEST_ASSERT_TRUE(hy_batt_estimate(&B, buf, sizeof buf));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_shown_follows_raw_on_usb);
    RUN_TEST(test_no_rise_while_discharging);
    RUN_TEST(test_low_once_per_crossing);
    RUN_TEST(test_no_events_on_usb);
    RUN_TEST(test_usb_rearms);
    RUN_TEST(test_crit_once);
    RUN_TEST(test_unknown_battery);
    RUN_TEST(test_text_red_and_bolt);
    RUN_TEST(test_changed_flag);
    RUN_TEST(test_estimate_hidden_then_shown);
    RUN_TEST(test_estimate_days_and_under_hour);
    RUN_TEST(test_estimate_hidden_on_usb);
    return UNITY_END();
}
