#include <string.h>

#include "unity.h"
#include "hy/netcfg.h"

void setUp(void) {}
void tearDown(void) {}

static void test_ssid_rules(void)
{
    char s33[34], s32[33];
    memset(s33, 'a', 33); s33[33] = 0;
    memset(s32, 'a', 32); s32[32] = 0;
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_net_check_ssid(""));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_net_check_ssid(s33));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_net_check_ssid("HomeNet"));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_net_check_ssid(s32));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_net_check_ssid("\x01"));
}

static void test_pass_rules(void)
{
    char p63[64], p64[65];
    memset(p63, 'x', 63); p63[63] = 0;
    memset(p64, 'x', 64); p64[64] = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_net_check_pass("", false));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_net_check_pass("1234567", false));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_net_check_pass("12345678", false));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_net_check_pass(p63, false));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_net_check_pass(p64, false));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_net_check_pass("\x7f", false));
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_net_check_pass("", true));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_net_check_pass("12345678", true));
}

static void test_try_order(void)
{
    hy_known_net_t k[4] = {{"a", 0}, {"b", 5}, {"c", 9}, {"d", 0}};
    uint8_t o[4];
    TEST_ASSERT_EQUAL_UINT(4, hy_net_try_order(k, 4, o));
    TEST_ASSERT_EQUAL_UINT8(2, o[0]);
    TEST_ASSERT_EQUAL_UINT8(1, o[1]);
    TEST_ASSERT_EQUAL_UINT8(0, o[2]);
    TEST_ASSERT_EQUAL_UINT8(3, o[3]);
}

static void test_backoff(void)
{
    static const uint32_t want[] = {5, 10, 20, 40, 60, 60};
    for (uint32_t i = 0; i < 6; i++) TEST_ASSERT_EQUAL_UINT32(want[i], hy_net_backoff_s(i + 1));
}

static void test_ap_ssid(void)
{
    const uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x4a, 0x0f};
    char out[12];
    hy_net_ap_ssid(mac, out);
    TEST_ASSERT_EQUAL_STRING("hygeia-4A0F", out);
}

static void test_ap_pass_alphabet(void)
{
    static const char alpha[] = "23456789abcdefghjkmnpqrstuvwxyz";
    uint8_t rnd[10];
    char out[11];
    for (int i = 0; i < 10; i++) rnd[i] = (uint8_t)i;
    hy_net_ap_pass(rnd, out);
    TEST_ASSERT_EQUAL_UINT(10, strlen(out));
    for (int i = 0; i < 10; i++) TEST_ASSERT_NOT_NULL(strchr(alpha, out[i]));
    memset(rnd, 255, sizeof rnd);
    hy_net_ap_pass(rnd, out);
    TEST_ASSERT_EQUAL_UINT(10, strlen(out));
    for (int i = 0; i < 10; i++) TEST_ASSERT_NOT_NULL(strchr(alpha, out[i]));
    TEST_ASSERT_EQUAL_CHAR(alpha[255 % 31], out[0]);
}

static void test_wifi_qr(void)
{
    char out[64];
    int n = hy_net_wifi_qr("HomeNet", "a;b:c", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:HomeNet;P:a\\;b\\:c;;", out);
    TEST_ASSERT_EQUAL_INT((int)strlen(out), n);
    n = hy_net_wifi_qr("HomeNet", "", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("WIFI:T:nopass;S:HomeNet;;", out);
    TEST_ASSERT_EQUAL_INT((int)strlen(out), n);
    TEST_ASSERT_EQUAL_INT(-1, hy_net_wifi_qr("HomeNet", "a;b:c", out, 10));
    TEST_ASSERT_EQUAL_INT(-1, hy_net_wifi_qr("HomeNet", "", out, 0));
    /* exact fit: length plus NUL */
    n = hy_net_wifi_qr("HomeNet", "", out, 26);
    TEST_ASSERT_EQUAL_INT(25, n);
    TEST_ASSERT_EQUAL_INT(-1, hy_net_wifi_qr("HomeNet", "", out, 25));
    n = hy_net_wifi_qr("a\\b,c\"d", "12345678", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:a\\\\b\\,c\\\"d;P:12345678;;", out);
    TEST_ASSERT_EQUAL_INT((int)strlen(out), n);
}

static hy_radio_in_t base(hy_net_mode_t saved)
{
    hy_radio_in_t in = {.saved = saved, .idle_s = HY_NET_BATT_IDLE_S, .now = 10000, .last_activity = 10000};
    return in;
}

static void test_radio_usb(void)
{
    hy_net_mode_t modes[] = {HY_NET_OFF, HY_NET_CLIENT, HY_NET_AP};
    for (int m = 0; m < 3; m++) {
        for (int s = 0; s < 2; s++) {
            hy_radio_in_t in = base(modes[m]);
            in.usb = true;
            in.started_on_battery = (s == 1);
            in.now = in.last_activity + 100000;
            TEST_ASSERT_EQUAL_INT(modes[m], hy_net_radio(&in));
        }
    }
}

static void test_radio_battery(void)
{
    hy_radio_in_t in = base(HY_NET_CLIENT);
    TEST_ASSERT_EQUAL_INT(HY_NET_OFF, hy_net_radio(&in));
    in.started_on_battery = true;
    in.now = in.last_activity + 599;
    TEST_ASSERT_EQUAL_INT(HY_NET_CLIENT, hy_net_radio(&in));
    in.now = in.last_activity + 600;
    TEST_ASSERT_EQUAL_INT(HY_NET_OFF, hy_net_radio(&in));
    in.keep_on_battery = true;
    in.started_on_battery = false;
    TEST_ASSERT_EQUAL_INT(HY_NET_CLIENT, hy_net_radio(&in));
}

static void test_radio_unplug_keeps_running(void)
{
    const int64_t t = 5000;
    hy_radio_in_t in = base(HY_NET_CLIENT);
    in.now = t - 100;
    in.usb = true;
    hy_net_on_usb_change(&in, true);
    TEST_ASSERT_EQUAL_INT(HY_NET_CLIENT, hy_net_radio(&in));
    in.now = t;
    in.usb = false;
    hy_net_on_usb_change(&in, false);
    TEST_ASSERT_TRUE(in.started_on_battery);
    TEST_ASSERT_EQUAL_INT64(t, in.last_activity);
    in.now = t + 599;
    TEST_ASSERT_EQUAL_INT(HY_NET_CLIENT, hy_net_radio(&in));
    in.now = t + 600;
    TEST_ASSERT_EQUAL_INT(HY_NET_OFF, hy_net_radio(&in));
    in.usb = true;
    hy_net_on_usb_change(&in, true);
    TEST_ASSERT_FALSE(in.started_on_battery);
    TEST_ASSERT_EQUAL_INT(HY_NET_CLIENT, hy_net_radio(&in));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ssid_rules);
    RUN_TEST(test_pass_rules);
    RUN_TEST(test_try_order);
    RUN_TEST(test_backoff);
    RUN_TEST(test_ap_ssid);
    RUN_TEST(test_ap_pass_alphabet);
    RUN_TEST(test_wifi_qr);
    RUN_TEST(test_radio_usb);
    RUN_TEST(test_radio_battery);
    RUN_TEST(test_radio_unplug_keeps_running);
    return UNITY_END();
}
