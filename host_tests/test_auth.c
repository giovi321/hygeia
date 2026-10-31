#include <string.h>

#include "unity.h"
#include "hy/auth.h"

void setUp(void) {}
void tearDown(void) {}

static void test_pair_code_format(void)
{
    hy_pair_t p;
    hy_pair_init(&p);
    TEST_ASSERT_EQUAL(HY_OK, hy_pair_start(&p, 42, 100));
    TEST_ASSERT_EQUAL_STRING("000042", p.code);
    TEST_ASSERT_TRUE(hy_pair_active(&p, 100));
    TEST_ASSERT_EQUAL(HY_OK, hy_pair_start(&p, 1234567, 100));
    TEST_ASSERT_EQUAL_STRING("234567", p.code);
}

static void test_pair_ok_once(void)
{
    hy_pair_t p;
    hy_pair_init(&p);
    TEST_ASSERT_FALSE(hy_pair_active(&p, 0));
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_pair_try(&p, "000000", 0));
    hy_pair_start(&p, 482913, 10);
    TEST_ASSERT_EQUAL(HY_OK, hy_pair_try(&p, "482913", 11));
    TEST_ASSERT_FALSE(hy_pair_active(&p, 11));
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_pair_try(&p, "482913", 12));
}

static void test_pair_expiry(void)
{
    hy_pair_t p;
    hy_pair_init(&p);
    hy_pair_start(&p, 482913, 1000);
    TEST_ASSERT_TRUE(hy_pair_active(&p, 1119));
    TEST_ASSERT_FALSE(hy_pair_active(&p, 1120));
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_pair_try(&p, "482913", 1120));
    hy_pair_start(&p, 482913, 1000);
    TEST_ASSERT_EQUAL(HY_OK, hy_pair_try(&p, "482913", 1119));
}

static void test_pair_spaces(void)
{
    hy_pair_t p;
    hy_pair_init(&p);
    hy_pair_start(&p, 482913, 0);
    TEST_ASSERT_EQUAL(HY_OK, hy_pair_try(&p, " 482 913 ", 1));
    hy_pair_start(&p, 482913, 0);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_pair_try(&p, "48291", 1));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_pair_try(&p, "4829130", 1));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_pair_try(&p, "48291x", 1));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_pair_try(&p, "482-913", 1));
    TEST_ASSERT_EQUAL_UINT8(4, p.fails);
    TEST_ASSERT_EQUAL(HY_OK, hy_pair_try(&p, "482913", 1));
    hy_pair_start(&p, 482913, 0);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_pair_try(&p, "", 1));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_pair_try(&p, NULL, 1));
    TEST_ASSERT_EQUAL_UINT8(2, p.fails);
}

static void test_pair_lockout(void)
{
    hy_pair_t p;
    hy_pair_init(&p);
    hy_pair_start(&p, 482913, 0);
    for (int i = 0; i < 4; i++)
        TEST_ASSERT_EQUAL(HY_E_INVALID, hy_pair_try(&p, "111111", 5));
    TEST_ASSERT_TRUE(hy_pair_active(&p, 5));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_pair_try(&p, "111111", 100)); /* fifth */
    TEST_ASSERT_FALSE(hy_pair_active(&p, 100));
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_pair_try(&p, "482913", 101));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_pair_start(&p, 482913, 159));
    TEST_ASSERT_EQUAL(HY_OK, hy_pair_start(&p, 482913, 160));
    TEST_ASSERT_EQUAL(HY_OK, hy_pair_try(&p, "482913", 161));
}

static void test_pair_restart_keeps_fails(void)
{
    hy_pair_t p;
    hy_pair_init(&p);
    hy_pair_start(&p, 482913, 0);
    for (int i = 0; i < 4; i++)
        TEST_ASSERT_EQUAL(HY_E_INVALID, hy_pair_try(&p, "111111", 1));
    TEST_ASSERT_EQUAL(HY_OK, hy_pair_start(&p, 555555, 2));
    TEST_ASSERT_EQUAL_UINT8(4, p.fails);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_pair_try(&p, "111111", 3)); /* fifth overall: block to 63 */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_pair_start(&p, 555555, 62));
    TEST_ASSERT_EQUAL(HY_OK, hy_pair_start(&p, 555555, 63));
    TEST_ASSERT_EQUAL_UINT8(0, p.fails);
    TEST_ASSERT_EQUAL(HY_OK, hy_pair_try(&p, "555555", 64));
}

static void test_pair_success_resets_fails(void)
{
    hy_pair_t p;
    hy_pair_init(&p);
    hy_pair_start(&p, 482913, 0);
    for (int i = 0; i < 4; i++)
        hy_pair_try(&p, "111111", 1);
    TEST_ASSERT_EQUAL(HY_OK, hy_pair_try(&p, "482913", 2));
    TEST_ASSERT_EQUAL_UINT8(0, p.fails);
}

static void test_host_first_label(void)
{
    TEST_ASSERT_TRUE(hy_host_first_label_is("hygeia", "hygeia"));
    TEST_ASSERT_TRUE(hy_host_first_label_is("hygeia.local", "hygeia"));
    TEST_ASSERT_TRUE(hy_host_first_label_is("HYGEIA.lan:80", "hygeia"));
    TEST_ASSERT_TRUE(hy_host_first_label_is("hygeia:80", "hygeia"));
    TEST_ASSERT_TRUE(hy_host_first_label_is("hygeia.", "hygeia"));
    TEST_ASSERT_FALSE(hy_host_first_label_is("hygeia-x.lan", "hygeia"));
    TEST_ASSERT_FALSE(hy_host_first_label_is("xhygeia.lan", "hygeia"));
    TEST_ASSERT_FALSE(hy_host_first_label_is("hyge", "hygeia"));
    TEST_ASSERT_FALSE(hy_host_first_label_is("", "hygeia"));
    TEST_ASSERT_FALSE(hy_host_first_label_is(NULL, "hygeia"));
    TEST_ASSERT_FALSE(hy_host_first_label_is("[hygeia]", "hygeia"));
    TEST_ASSERT_FALSE(hy_host_first_label_is("hygeia", ""));
}

static void test_host_bracket_rejected(void)
{
    const char *allowed[] = {"[", "[::1]"};
    TEST_ASSERT_FALSE(hy_host_allowed("[::1]:80", allowed, 2));
    TEST_ASSERT_FALSE(hy_host_allowed("[", allowed, 2));
}

static void test_ct_equal(void)
{
    const uint8_t a[3] = {1, 2, 3}, b[3] = {1, 2, 3}, c[3] = {1, 2, 4};
    TEST_ASSERT_TRUE(hy_ct_equal(a, b, 3));
    TEST_ASSERT_FALSE(hy_ct_equal(a, c, 3));
    TEST_ASSERT_TRUE(hy_ct_equal(a, c, 2));
}

static void test_cookie_get(void)
{
    char out[8];
    TEST_ASSERT_TRUE(hy_cookie_get("a=1; hy_s=ab", "hy_s", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("ab", out);
    TEST_ASSERT_TRUE(hy_cookie_get("hy_s=ab; a=1", "hy_s", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("ab", out);
    TEST_ASSERT_TRUE(hy_cookie_get("a=1;hy_s=zz;b=2", "hy_s", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("zz", out);
    TEST_ASSERT_TRUE(hy_cookie_get("a=1; b=", "b", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(hy_cookie_get("hy_sx=1", "hy_s", out, sizeof out));
    TEST_ASSERT_FALSE(hy_cookie_get("xhy_s=1", "hy_s", out, sizeof out));
    TEST_ASSERT_FALSE(hy_cookie_get("a=1", "hy_s", out, sizeof out));
    TEST_ASSERT_FALSE(hy_cookie_get(NULL, "hy_s", out, sizeof out));
    TEST_ASSERT_TRUE(hy_cookie_get("hy_s=1234567", "hy_s", out, sizeof out));      /* 7 chars + NUL fits */
    TEST_ASSERT_FALSE(hy_cookie_get("hy_s=12345678", "hy_s", out, sizeof out));    /* 8 chars does not */
}

static void test_token_form(void)
{
    char t[65];
    memset(t, 'a', 64);
    t[64] = 0;
    TEST_ASSERT_TRUE(hy_token_well_formed(t));
    memcpy(t, "0123456789abcdef", 16);
    TEST_ASSERT_TRUE(hy_token_well_formed(t));
    t[10] = 'A';
    TEST_ASSERT_FALSE(hy_token_well_formed(t));
    t[10] = 'g';
    TEST_ASSERT_FALSE(hy_token_well_formed(t));
    t[10] = 'a';
    t[63] = 0;
    TEST_ASSERT_FALSE(hy_token_well_formed(t));
    TEST_ASSERT_FALSE(hy_token_well_formed(""));
    TEST_ASSERT_FALSE(hy_token_well_formed(NULL));
    char longer[66];
    memset(longer, 'a', 65);
    longer[65] = 0;
    TEST_ASSERT_FALSE(hy_token_well_formed(longer));
}

static void test_host_allowed(void)
{
    const char *allowed[] = {"192.168.1.23", "hygeia.local", "hygeia-3F2A", "192.168.4.1"};
    TEST_ASSERT_TRUE(hy_host_allowed("HYGEIA.local:80", allowed, 4));
    TEST_ASSERT_TRUE(hy_host_allowed("hygeia.local", allowed, 4));
    TEST_ASSERT_TRUE(hy_host_allowed("192.168.4.1:8080", allowed, 4));
    TEST_ASSERT_TRUE(hy_host_allowed("hygeia-3f2a", allowed, 4));
    TEST_ASSERT_FALSE(hy_host_allowed("evil.com", allowed, 4));
    TEST_ASSERT_FALSE(hy_host_allowed("hygeia.local.evil.com", allowed, 4));
    TEST_ASSERT_FALSE(hy_host_allowed("hygeia.loca", allowed, 4));
    TEST_ASSERT_FALSE(hy_host_allowed("", allowed, 4));
    TEST_ASSERT_FALSE(hy_host_allowed(NULL, allowed, 4));
    TEST_ASSERT_FALSE(hy_host_allowed("hygeia.local", allowed, 0));
}

static hy_guard_in_t base(hy_method_t m)
{
    hy_guard_in_t g = {m, false, true, "1", NULL, "192.168.1.23"};
    return g;
}

static void test_guard_cross_site_post(void)
{
    hy_guard_in_t g = base(HY_M_POST);
    g.x_hygeia = NULL;
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    g.x_hygeia = "0";
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    g.x_hygeia = "1";
    TEST_ASSERT_EQUAL(HY_GUARD_OK, hy_auth_guard(&g));
    g.m = HY_M_PUT;
    g.x_hygeia = NULL;
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    g.m = HY_M_DELETE;
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
}

static void test_guard_origin_mismatch(void)
{
    hy_guard_in_t g = base(HY_M_POST);
    g.origin = "http://evil.com";
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    g.origin = "http://192.168.1.23:80";
    TEST_ASSERT_EQUAL(HY_GUARD_OK, hy_auth_guard(&g));
    g.origin = "http://192.168.1.23";
    TEST_ASSERT_EQUAL(HY_GUARD_OK, hy_auth_guard(&g));
    g.origin = "http://192.168.1.23/";
    TEST_ASSERT_EQUAL(HY_GUARD_OK, hy_auth_guard(&g));
    g.origin = "http://192.168.1.234";
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    g.origin = "https://192.168.1.23";
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    g.origin = "null";
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    g.origin = "";
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    g.origin = "http://HYGEIA.local:80";
    g.host = "hygeia.local:80";
    TEST_ASSERT_EQUAL(HY_GUARD_OK, hy_auth_guard(&g));
    g.host = NULL;
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
}

static void test_guard_get_unpaired(void)
{
    hy_guard_in_t g = base(HY_M_GET);
    g.session_ok = false;
    g.x_hygeia = NULL;
    TEST_ASSERT_EQUAL(HY_GUARD_UNPAIRED, hy_auth_guard(&g));
    g.session_ok = true;
    TEST_ASSERT_EQUAL(HY_GUARD_OK, hy_auth_guard(&g));
    g.session_ok = false;
    g.open_route = true;
    TEST_ASSERT_EQUAL(HY_GUARD_OK, hy_auth_guard(&g));
    /* a write without a session on a closed route passes the write check, then is unpaired */
    hy_guard_in_t w = base(HY_M_POST);
    w.session_ok = false;
    TEST_ASSERT_EQUAL(HY_GUARD_UNPAIRED, hy_auth_guard(&w));
}

static void test_guard_open_route(void)
{
    hy_guard_in_t g = base(HY_M_POST);
    g.open_route = true;
    g.session_ok = false;
    TEST_ASSERT_EQUAL(HY_GUARD_OK, hy_auth_guard(&g));
    g.x_hygeia = NULL;
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
}

static void test_guard_odd_origins(void)
{
    hy_guard_in_t g = base(HY_M_POST);
    g.origin = "http://192.168.1.23.evil.com";
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    g.origin = "http://192.168.1.23@evil.com";
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    g.origin = "http://";
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    g.origin = "http://[::1]:80";
    g.host = "[::1]:80";
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    g.origin = "http://192.168.1.23";
    g.host = "";
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(&g));
    TEST_ASSERT_EQUAL(HY_GUARD_FORBIDDEN, hy_auth_guard(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pair_code_format);
    RUN_TEST(test_pair_ok_once);
    RUN_TEST(test_pair_expiry);
    RUN_TEST(test_pair_spaces);
    RUN_TEST(test_pair_lockout);
    RUN_TEST(test_pair_restart_keeps_fails);
    RUN_TEST(test_pair_success_resets_fails);
    RUN_TEST(test_ct_equal);
    RUN_TEST(test_cookie_get);
    RUN_TEST(test_token_form);
    RUN_TEST(test_host_allowed);
    RUN_TEST(test_guard_cross_site_post);
    RUN_TEST(test_guard_origin_mismatch);
    RUN_TEST(test_guard_get_unpaired);
    RUN_TEST(test_guard_open_route);
    RUN_TEST(test_guard_odd_origins);
    RUN_TEST(test_host_first_label);
    RUN_TEST(test_host_bracket_rejected);
    return UNITY_END();
}
