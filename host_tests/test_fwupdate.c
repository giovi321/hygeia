#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "cJSON.h"
#include "hy/fwupdate.h"

void setUp(void) {}
void tearDown(void) {}

static const char *REPO = "giovi321/hygeia";

static void test_parse_release_ok(void)
{
    hy_semver_t v;
    TEST_ASSERT_TRUE(hy_fw_parse_release("v0.4.0", &v));
    TEST_ASSERT_EQUAL_UINT16(0, v.major);
    TEST_ASSERT_EQUAL_UINT16(4, v.minor);
    TEST_ASSERT_EQUAL_UINT16(0, v.patch);
    TEST_ASSERT_TRUE(hy_fw_parse_release("v65535.0.10", &v));
    TEST_ASSERT_EQUAL_UINT16(65535, v.major);
    TEST_ASSERT_EQUAL_UINT16(10, v.patch);
}

static void test_parse_release_bad(void)
{
    hy_semver_t v;
    const char *bad[] = {"0.4.0", "v0.4", "v0.4.0-rc1", "v00.4.0", "v0.4.0 ", "v65536.0.0", "", "dev-a88c56d",
                         "v0.4.0-3-ga88c56d"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(hy_fw_parse_release(bad[i], &v), bad[i]);
    }
}

static void test_compare_newer(void)
{
    TEST_ASSERT_EQUAL(HY_FW_NEWER, hy_fw_compare("v0.4.0", "v0.4.1"));
    TEST_ASSERT_EQUAL(HY_FW_NEWER, hy_fw_compare("v0.4.9", "v0.10.0"));
    TEST_ASSERT_EQUAL(HY_FW_UP_TO_DATE, hy_fw_compare("v1.0.0", "v0.9.9"));
}

static void test_compare_equal_and_ahead(void)
{
    TEST_ASSERT_EQUAL(HY_FW_UP_TO_DATE, hy_fw_compare("v0.4.0", "v0.4.0"));
    TEST_ASSERT_EQUAL(HY_FW_UP_TO_DATE, hy_fw_compare("v0.5.0", "v0.4.0"));
}

static void test_compare_dev(void)
{
    const char *dev[] = {"dev-a88c56d", "a88c56d", "v0.4.0-3-ga88c56d-dirty", "v0.5.0-rc1", ""};
    for (size_t i = 0; i < sizeof dev / sizeof dev[0]; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(HY_FW_DEV, hy_fw_compare(dev[i], "v0.4.0"), dev[i]);
    }
}

static void test_compare_bad_latest(void)
{
    TEST_ASSERT_EQUAL(HY_FW_UP_TO_DATE, hy_fw_compare("v0.4.0", "v0.4"));
    TEST_ASSERT_EQUAL(HY_FW_UP_TO_DATE, hy_fw_compare("dev-a88c56d", "v0.4"));
}

static void test_tag_from_location_abs(void)
{
    char tag[16];
    const char *loc = "https://github.com/giovi321/hygeia/releases/tag/v0.4.0";
    TEST_ASSERT_EQUAL(HY_OK, hy_fw_tag_from_location(loc, REPO, tag, sizeof tag));
    TEST_ASSERT_EQUAL_STRING("v0.4.0", tag);
    TEST_ASSERT_EQUAL(HY_OK, hy_fw_tag_from_location(loc, "Giovi321/Hygeia", tag, sizeof tag));
    TEST_ASSERT_EQUAL_STRING("v0.4.0", tag);
}

static void test_tag_from_location_rel(void)
{
    char tag[16];
    TEST_ASSERT_EQUAL(HY_OK, hy_fw_tag_from_location("/giovi321/hygeia/releases/tag/v0.4.0", REPO, tag, sizeof tag));
    TEST_ASSERT_EQUAL_STRING("v0.4.0", tag);
}

static void test_tag_from_location_none(void)
{
    char tag[16];
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND,
                      hy_fw_tag_from_location("https://github.com/giovi321/hygeia/releases", REPO, tag, sizeof tag));
}

static void test_tag_from_location_bad(void)
{
    char tag[16];
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_fw_tag_from_location(
        "https://github.com/other/repo/releases/tag/v0.4.0", REPO, tag, sizeof tag));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_fw_tag_from_location(
        "http://github.com/giovi321/hygeia/releases/tag/v0.4.0", REPO, tag, sizeof tag));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_fw_tag_from_location(
        "https://github.com.evil.com/giovi321/hygeia/releases/tag/v0.4.0", REPO, tag, sizeof tag));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_fw_tag_from_location(
        "https://github.com/giovi321/hygeia/releases/tag/v0.4.0-rc1", REPO, tag, sizeof tag));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_fw_tag_from_location(
        "https://github.com/giovi321/hygeia/releases/tag/v0.4.0/x", REPO, tag, sizeof tag));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_fw_tag_from_location(
        "https://github.com/giovi321/hygeia/releases/tag/v0.4.0", REPO, tag, 6));
    TEST_ASSERT_EQUAL(HY_OK, hy_fw_tag_from_location(
        "https://github.com/giovi321/hygeia/releases/tag/v0.4.0", REPO, tag, 7));
}

static void test_redirect_ok(void)
{
    TEST_ASSERT_TRUE(hy_fw_redirect_ok("https://github.com/x"));
    TEST_ASSERT_TRUE(hy_fw_redirect_ok("https://release-assets.githubusercontent.com/a?b=c"));
    TEST_ASSERT_TRUE(hy_fw_redirect_ok("https://objects.githubusercontent.com:443/x"));
    TEST_ASSERT_FALSE(hy_fw_redirect_ok("http://github.com/x"));
    TEST_ASSERT_FALSE(hy_fw_redirect_ok("https://githubusercontent.com.evil.com/x"));
    TEST_ASSERT_FALSE(hy_fw_redirect_ok("https://evilgithubusercontent.com/x"));
    TEST_ASSERT_FALSE(hy_fw_redirect_ok("https://user@github.com/x"));
    TEST_ASSERT_FALSE(hy_fw_redirect_ok("https://github.com:8443/x"));
}

#define H1 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define H2 "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"

static void test_sums_find(void)
{
    const char *t = H1 "  hygeia.bin\n" H2 "  hygeia-full.bin\n";
    uint8_t sha[32];
    TEST_ASSERT_EQUAL(HY_OK, hy_fw_sums_find(t, strlen(t), "hygeia.bin", sha));
    TEST_ASSERT_EQUAL_HEX8(0x01, sha[0]);
    TEST_ASSERT_EQUAL_HEX8(0xef, sha[31]);
    TEST_ASSERT_EQUAL(HY_OK, hy_fw_sums_find(t, strlen(t), "hygeia-full.bin", sha));
    TEST_ASSERT_EQUAL_HEX8(0xfe, sha[0]);
    TEST_ASSERT_EQUAL_HEX8(0x10, sha[31]);
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_fw_sums_find(t, strlen(t), "hygeia", sha));
}

static void test_sums_variants(void)
{
    uint8_t sha[32];
    const char *bin = H1 " *hygeia.bin\n";
    TEST_ASSERT_EQUAL(HY_OK, hy_fw_sums_find(bin, strlen(bin), "hygeia.bin", sha));
    const char *crlf = H2 "  other.bin\r\n" H1 "  hygeia.bin\r\n";
    TEST_ASSERT_EQUAL(HY_OK, hy_fw_sums_find(crlf, strlen(crlf), "hygeia.bin", sha));
    TEST_ASSERT_EQUAL_HEX8(0x01, sha[0]);
    const char *up = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF  hygeia.bin\n";
    TEST_ASSERT_EQUAL(HY_OK, hy_fw_sums_find(up, strlen(up), "hygeia.bin", sha));
    TEST_ASSERT_EQUAL_HEX8(0xef, sha[31]);
    const char *nonl = H1 "  hygeia.bin";
    TEST_ASSERT_EQUAL(HY_OK, hy_fw_sums_find(nonl, strlen(nonl), "hygeia.bin", sha));
}

static void test_sums_bad(void)
{
    uint8_t sha[32];
    const char *short63 = "123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  hygeia.bin\n";
    TEST_ASSERT_EQUAL(63, strchr(short63, ' ') - short63);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_fw_sums_find(short63, strlen(short63), "hygeia.bin", sha));
    const char *dup = H1 "  hygeia.bin\n" H2 "  hygeia.bin\n";
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_fw_sums_find(dup, strlen(dup), "hygeia.bin", sha));
    char big[HY_FW_SUMS_MAX + 1];
    memset(big, '\n', sizeof big);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_fw_sums_find(big, HY_FW_SUMS_MAX + 1, "hygeia.bin", sha));
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_fw_sums_find(big, HY_FW_SUMS_MAX, "hygeia.bin", sha));
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_fw_sums_find("", 0, "hygeia.bin", sha));
}

static void test_err_codes_complete(void)
{
    TEST_ASSERT_NULL(hy_fw_err_code(HY_FWE_NONE));
    TEST_ASSERT_NULL(hy_fw_err_message(HY_FWE_NONE));
    for (int e = HY_FWE_NO_NETWORK; e < HY_FWE_COUNT; e++) {
        const char *c = hy_fw_err_code((hy_fw_err_t)e);
        const char *m = hy_fw_err_message((hy_fw_err_t)e);
        TEST_ASSERT_NOT_NULL(c);
        TEST_ASSERT_NOT_NULL(m);
        TEST_ASSERT_TRUE(strlen(c) > 0 && strlen(m) > 0);
        TEST_ASSERT_NULL(strstr(m, "\xE2\x80\x94"));
    }
    TEST_ASSERT_EQUAL_STRING("no_internet", hy_fw_err_code(HY_FWE_NO_INTERNET));
    TEST_ASSERT_EQUAL_STRING("busy", hy_fw_err_code(HY_FWE_SELF_PENDING));
    TEST_ASSERT_EQUAL_STRING("downloading", hy_fw_state_name(HY_FWS_DOWNLOADING));
    TEST_ASSERT_EQUAL_STRING("up_to_date", hy_fw_state_name(HY_FWS_UP_TO_DATE));
}

static void test_status_json(void)
{
    hy_fw_status_t st;
    char buf[512];
    memset(&st, 0, sizeof st);
    st.state = HY_FWS_AVAILABLE;
    strcpy(st.installed, "dev-a88c56d");
    strcpy(st.latest, "v0.5.0");
    st.checked_s = 100;
    int n = hy_fw_status_json(&st, 112, buf, sizeof buf);
    TEST_ASSERT_EQUAL((int)strlen(buf), n);
    cJSON *o = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_STRING("available", cJSON_GetObjectItem(o, "state")->valuestring);
    TEST_ASSERT_EQUAL_STRING("dev-a88c56d", cJSON_GetObjectItem(o, "installed")->valuestring);
    TEST_ASSERT_EQUAL_STRING("v0.5.0", cJSON_GetObjectItem(o, "latest")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(o, "installed_release")));
    TEST_ASSERT_EQUAL(0, cJSON_GetObjectItem(o, "pct")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(o, "error")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(o, "message")));
    TEST_ASSERT_EQUAL(12, cJSON_GetObjectItem(o, "checked_ago_s")->valueint);
    cJSON_Delete(o);

    st.state = HY_FWS_FAILED;
    st.err = HY_FWE_NO_INTERNET;
    TEST_ASSERT_TRUE(hy_fw_status_json(&st, 112, buf, sizeof buf) > 0);
    o = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_STRING("failed", cJSON_GetObjectItem(o, "state")->valuestring);
    TEST_ASSERT_EQUAL_STRING("no_internet", cJSON_GetObjectItem(o, "error")->valuestring);
    TEST_ASSERT_EQUAL_STRING(hy_fw_err_message(HY_FWE_NO_INTERNET), cJSON_GetObjectItem(o, "message")->valuestring);
    cJSON_Delete(o);

    memset(&st, 0, sizeof st);
    strcpy(st.installed, "v0.4.0");
    st.installed_release = true;
    TEST_ASSERT_TRUE(hy_fw_status_json(&st, 5000, buf, sizeof buf) > 0);
    o = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_STRING("idle", cJSON_GetObjectItem(o, "state")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(o, "latest")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(o, "installed_release")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(o, "checked_ago_s")));
    cJSON_Delete(o);

    /* escaping */
    strcpy(st.installed, "a\"b\\c\n");
    TEST_ASSERT_TRUE(hy_fw_status_json(&st, 5000, buf, sizeof buf) > 0);
    o = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_STRING("a\"b\\c\n", cJSON_GetObjectItem(o, "installed")->valuestring);
    cJSON_Delete(o);

    /* too small */
    int need = hy_fw_status_json(&st, 5000, buf, sizeof buf);
    TEST_ASSERT_EQUAL(-1, hy_fw_status_json(&st, 5000, buf, (size_t)need));
    TEST_ASSERT_EQUAL(need, hy_fw_status_json(&st, 5000, buf, (size_t)need + 1));
    TEST_ASSERT_EQUAL(-1, hy_fw_status_json(&st, 5000, buf, 10));
}

static void test_event_json(void)
{
    hy_fw_status_t st;
    char buf[192];
    memset(&st, 0, sizeof st);
    st.state = HY_FWS_DOWNLOADING;
    st.pct = 42;
    TEST_ASSERT_TRUE(hy_fw_event_json(&st, buf, sizeof buf) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"ev\":\"update\",\"state\":\"downloading\",\"pct\":42}", buf);
    st.state = HY_FWS_FAILED;
    st.err = HY_FWE_CHECKSUM;
    TEST_ASSERT_TRUE(hy_fw_event_json(&st, buf, sizeof buf) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"ev\":\"update\",\"state\":\"failed\",\"pct\":42,\"error\":\"checksum\"}", buf);
    int longest = 0;
    for (int e = HY_FWE_NONE; e < HY_FWE_COUNT; e++) {
        st.err = (hy_fw_err_t)e;
        st.pct = 100;
        int n = hy_fw_event_json(&st, buf, sizeof buf);
        TEST_ASSERT_TRUE(n > 0);
        if (n > longest) longest = n;
    }
    TEST_ASSERT_TRUE(longest < 192);
    TEST_ASSERT_EQUAL(-1, hy_fw_event_json(&st, buf, 20));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_release_ok);
    RUN_TEST(test_parse_release_bad);
    RUN_TEST(test_compare_newer);
    RUN_TEST(test_compare_equal_and_ahead);
    RUN_TEST(test_compare_dev);
    RUN_TEST(test_compare_bad_latest);
    RUN_TEST(test_tag_from_location_abs);
    RUN_TEST(test_tag_from_location_rel);
    RUN_TEST(test_tag_from_location_none);
    RUN_TEST(test_tag_from_location_bad);
    RUN_TEST(test_redirect_ok);
    RUN_TEST(test_sums_find);
    RUN_TEST(test_sums_variants);
    RUN_TEST(test_sums_bad);
    RUN_TEST(test_err_codes_complete);
    RUN_TEST(test_status_json);
    RUN_TEST(test_event_json);
    return UNITY_END();
}
