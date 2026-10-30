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
    /* rise of 4 is accepted (battery swapped), shown 17: still below the re-arm level */
    TEST_ASSERT_FALSE(feed(false, 17, T0 + 4) & HY_BATT_EV_LOW);
    TEST_ASSERT_EQUAL_INT(17, B.pct);
    TEST_ASSERT_FALSE(feed(false, 15, T0 + 5) & HY_BATT_EV_LOW); /* not re-armed below 18 */
    /* rise of 3 is accepted and reaches 18: re-armed */
    TEST_ASSERT_FALSE(feed(false, 18, T0 + 6) & HY_BATT_EV_LOW);
    TEST_ASSERT_EQUAL_INT(18, B.pct);
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
    feed(false, 7, T0 + 180); /* rise of 3 accepted, shown 7: not re-armed */
    TEST_ASSERT_EQUAL_INT(7, B.pct);
    TEST_ASSERT_FALSE(feed(false, 5, T0 + 240) & HY_BATT_EV_CRIT);
    feed(false, 8, T0 + 300); /* rise of 3 accepted, shown 8: re-armed */
    TEST_ASSERT_EQUAL_INT(8, B.pct);
    f = feed(false, 5, T0 + 360);
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
    r.chg = HY_CHG_DONE; /* full battery on USB: bolt stays on */
    hy_batt_update(&B, &r, T0 + 120);
    hy_batt_text(&B, buf, sizeof buf, "--", &red, &bolt);
    TEST_ASSERT_TRUE(bolt);
    r.vbus = false; /* no USB */
    r.chg = HY_CHG_NONE;
    hy_batt_update(&B, &r, T0 + 180);
    hy_batt_text(&B, buf, sizeof buf, "--", &red, &bolt);
    TEST_ASSERT_FALSE(bolt);
    r.present = false; /* USB but no battery */
    r.vbus = true;
    r.pct = -1;
    hy_batt_update(&B, &r, T0 + 240);
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

static void test_estimate_reanchor(void)
{
    const int64_t H = 3600;
    /* accepted rise: the anchor moves to the new reading, no sample in that pass */
    hy_batt_init(&B, 0);
    feed(false, 80, T0);
    feed(false, 70, T0 + 4 * H);
    TEST_ASSERT_EQUAL_INT32(250, B.rate_cph);
    feed(false, 75, T0 + 5 * H); /* rise of 5 */
    TEST_ASSERT_EQUAL_INT(75, B.pct);
    TEST_ASSERT_EQUAL_INT32(250, B.rate_cph);
    feed(false, 65, T0 + 7 * H); /* 2 h after the new anchor: no sample (old anchor would give one) */
    TEST_ASSERT_EQUAL_INT32(250, B.rate_cph);
    feed(false, 65, T0 + 8 * H); /* 3 h, drop 10 from the new anchor: sample 333 */
    TEST_ASSERT_EQUAL_INT32(291, B.rate_cph);

    /* clock moved back: re-anchor, no sample */
    hy_batt_init(&B, 0);
    feed(false, 80, T0);
    feed(false, 70, T0 + 4 * H);
    TEST_ASSERT_EQUAL_INT32(250, B.rate_cph);
    feed(false, 70, T0 + 3 * H);
    TEST_ASSERT_EQUAL_INT32(250, B.rate_cph);
    feed(false, 65, T0 + 6 * H); /* 3 h and 5 points from the new anchor: sample 166 */
    TEST_ASSERT_EQUAL_INT32(208, B.rate_cph);
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

static hy_batt_crit_confirm_t C;

static bool conf(int64_t t, int pct, int mv, bool usb)
{
    return hy_batt_crit_confirm(&C, t, pct, mv, usb);
}

static void test_crit_confirm_needs_two_readings_10s_apart(void)
{
    memset(&C, 0, sizeof C);
    TEST_ASSERT_FALSE(conf(100, 4, 3400, false));   /* one reading never confirms */
    TEST_ASSERT_FALSE(conf(105, 4, 3400, false));   /* too close */
    TEST_ASSERT_FALSE(conf(109, 4, 3400, false));
    TEST_ASSERT_TRUE(conf(110, 4, 3400, false));    /* 10 s after the first */
    TEST_ASSERT_FALSE(conf(120, 4, 3400, false));   /* confirms once */
}

static void test_crit_confirm_mv_gate(void)
{
    memset(&C, 0, sizeof C);
    TEST_ASSERT_FALSE(conf(100, 3, 3600, false));   /* at the threshold is not below it */
    TEST_ASSERT_FALSE(conf(110, 3, 3600, false));
    TEST_ASSERT_FALSE(conf(120, 3, 3900, false));
    TEST_ASSERT_FALSE(conf(130, 3, 3900, false));
    TEST_ASSERT_FALSE(conf(140, 3, 0, false));      /* mV 0: no reading */
    TEST_ASSERT_FALSE(conf(150, 3, 0, false));
    TEST_ASSERT_FALSE(conf(160, 3, 3599, false));
    TEST_ASSERT_TRUE(conf(170, 3, 3599, false));
}

static void test_crit_confirm_resets(void)
{
    memset(&C, 0, sizeof C);
    TEST_ASSERT_FALSE(conf(100, 4, 3400, false));
    TEST_ASSERT_FALSE(conf(105, 9, 3400, false));   /* a non-critical reading in between */
    TEST_ASSERT_FALSE(conf(110, 4, 3400, false));   /* restarts the 10 s */
    TEST_ASSERT_FALSE(conf(115, 4, 3400, false));
    TEST_ASSERT_TRUE(conf(120, 4, 3400, false));
    memset(&C, 0, sizeof C);
    TEST_ASSERT_FALSE(conf(200, 4, 3400, false));
    TEST_ASSERT_FALSE(conf(205, 4, 3400, true));    /* USB present resets */
    TEST_ASSERT_FALSE(conf(210, 4, 3400, false));
    TEST_ASSERT_TRUE(conf(220, 4, 3400, false));
    TEST_ASSERT_FALSE(conf(230, -1, 3400, false));  /* unknown percent resets */
    TEST_ASSERT_FALSE(conf(240, 4, 3400, false));
    TEST_ASSERT_TRUE(conf(250, 4, 3400, false));
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
    RUN_TEST(test_estimate_reanchor);
    RUN_TEST(test_estimate_days_and_under_hour);
    RUN_TEST(test_estimate_hidden_on_usb);
    RUN_TEST(test_crit_confirm_needs_two_readings_10s_apart);
    RUN_TEST(test_crit_confirm_mv_gate);
    RUN_TEST(test_crit_confirm_resets);
    return UNITY_END();
}
