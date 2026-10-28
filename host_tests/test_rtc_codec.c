#include <string.h>

#include "unity.h"
#include "hy/rtc_codec.h"

void setUp(void) {}
void tearDown(void) {}

static void roundtrip(int64_t epoch, const uint8_t *expect_raw)
{
    uint8_t raw[7];
    int64_t back = -1;
    TEST_ASSERT_TRUE(hy_rtc_encode(epoch, raw));
    if (expect_raw) {
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expect_raw, raw, 7);
    }
    TEST_ASSERT_TRUE(hy_rtc_decode(raw, &back));
    TEST_ASSERT_EQUAL_INT64(epoch, back);
}

static void test_known_vector_from_board(void)
{
    /* log: raw=06 06 19 28 03 10 26 -> 2026-10-28 19:06:06 UTC, a Wednesday */
    const uint8_t raw[7] = { 0x06, 0x06, 0x19, 0x28, 0x03, 0x10, 0x26 };
    int64_t e = 0;
    TEST_ASSERT_TRUE(hy_rtc_decode(raw, &e));
    TEST_ASSERT_EQUAL_INT64(1793214366LL, e);
    roundtrip(1793214366LL, raw);
}

static void test_leap_day(void)
{
    const uint8_t raw[7] = { 0x00, 0x00, 0x12, 0x29, 0x04, 0x02, 0x24 }; /* 2024-02-29 12:00:00 Thursday */
    roundtrip(1709208000LL, raw);
}

static void test_2000_03_01_after_leap_2000(void)
{
    const uint8_t raw[7] = { 0x00, 0x00, 0x00, 0x01, 0x03, 0x03, 0x00 }; /* Wednesday */
    roundtrip(951868800LL, raw);
    const uint8_t feb29[7] = { 0x00, 0x00, 0x00, 0x29, 0x02, 0x02, 0x00 }; /* 2000 is a leap year */
    roundtrip(951782400LL, feb29);
}

static void test_range_ends(void)
{
    const uint8_t first[7] = { 0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x00 }; /* 2000-01-01 Saturday */
    roundtrip(946684800LL, first);
    const uint8_t last[7] = { 0x59, 0x59, 0x23, 0x31, 0x04, 0x12, 0x99 }; /* 2099-12-31 Thursday */
    roundtrip(4102444799LL, last);
}

static void test_month_boundaries(void)
{
    int64_t e = 1709164800LL; /* 2024-02-29 00:00:00 */
    uint8_t raw[7];
    TEST_ASSERT_TRUE(hy_rtc_encode(e + 86400, raw)); /* 2024-03-01 */
    TEST_ASSERT_EQUAL_HEX8(0x01, raw[3]);
    TEST_ASSERT_EQUAL_HEX8(0x03, raw[5]);
    TEST_ASSERT_TRUE(hy_rtc_encode(e - 86400, raw)); /* 2024-02-28 */
    TEST_ASSERT_EQUAL_HEX8(0x28, raw[3]);
    TEST_ASSERT_EQUAL_HEX8(0x02, raw[5]);
    /* every day of 2024 and 2100-adjacent years round-trips */
    for (int64_t t = 1704067200LL; t < 1735689600LL; t += 86400 + 3601) {
        roundtrip(t, NULL);
    }
}

static void test_encode_out_of_range(void)
{
    uint8_t raw[7];
    TEST_ASSERT_FALSE(hy_rtc_encode(946684799LL, raw));
    TEST_ASSERT_FALSE(hy_rtc_encode(4102444800LL, raw));
    TEST_ASSERT_FALSE(hy_rtc_encode(-1, raw));
}

static void test_rejects_feb_30_and_feb_29_non_leap(void)
{
    int64_t e;
    const uint8_t feb30[7] = { 0, 0, 0, 0x30, 0, 0x02, 0x24 };
    const uint8_t feb29_2025[7] = { 0, 0, 0, 0x29, 0, 0x02, 0x25 };
    const uint8_t feb29_2100ish[7] = { 0, 0, 0, 0x29, 0, 0x02, 0x99 };
    const uint8_t apr31[7] = { 0, 0, 0, 0x31, 0, 0x04, 0x24 };
    TEST_ASSERT_FALSE(hy_rtc_decode(feb30, &e));
    TEST_ASSERT_FALSE(hy_rtc_decode(feb29_2025, &e));
    TEST_ASSERT_FALSE(hy_rtc_decode(feb29_2100ish, &e));
    TEST_ASSERT_FALSE(hy_rtc_decode(apr31, &e));
}

static void test_rejects_invalid_bcd_and_ranges(void)
{
    int64_t e;
    const uint8_t bad_sec[7] = { 0x6A, 0, 0, 0x01, 0, 0x01, 0x24 };
    const uint8_t sec60[7] = { 0x60, 0, 0, 0x01, 0, 0x01, 0x24 };
    const uint8_t min_a[7] = { 0, 0x0A, 0, 0x01, 0, 0x01, 0x24 };
    const uint8_t hour24[7] = { 0, 0, 0x24, 0x01, 0, 0x01, 0x24 };
    const uint8_t day0[7] = { 0, 0, 0, 0x00, 0, 0x01, 0x24 };
    const uint8_t mon13[7] = { 0, 0, 0, 0x01, 0, 0x13, 0x24 };
    const uint8_t mon0[7] = { 0, 0, 0, 0x01, 0, 0x00, 0x24 };
    const uint8_t year_f[7] = { 0, 0, 0, 0x01, 0, 0x01, 0xA0 };
    TEST_ASSERT_FALSE(hy_rtc_decode(bad_sec, &e));
    TEST_ASSERT_FALSE(hy_rtc_decode(sec60, &e));
    TEST_ASSERT_FALSE(hy_rtc_decode(min_a, &e));
    TEST_ASSERT_FALSE(hy_rtc_decode(hour24, &e));
    TEST_ASSERT_FALSE(hy_rtc_decode(day0, &e));
    TEST_ASSERT_FALSE(hy_rtc_decode(mon13, &e));
    TEST_ASSERT_FALSE(hy_rtc_decode(mon0, &e));
    TEST_ASSERT_FALSE(hy_rtc_decode(year_f, &e));
}

static void test_os_flag_ignored_on_decode_and_clear_on_encode(void)
{
    const uint8_t raw[7] = { 0x86, 0x06, 0x19, 0x28, 0x03, 0x10, 0x26 };
    int64_t e = 0;
    TEST_ASSERT_TRUE(hy_rtc_decode(raw, &e));
    TEST_ASSERT_EQUAL_INT64(1793214366LL, e);
    uint8_t out[7];
    TEST_ASSERT_TRUE(hy_rtc_encode(e, out));
    TEST_ASSERT_EQUAL_HEX8(0x06, out[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_known_vector_from_board);
    RUN_TEST(test_leap_day);
    RUN_TEST(test_2000_03_01_after_leap_2000);
    RUN_TEST(test_range_ends);
    RUN_TEST(test_month_boundaries);
    RUN_TEST(test_encode_out_of_range);
    RUN_TEST(test_rejects_feb_30_and_feb_29_non_leap);
    RUN_TEST(test_rejects_invalid_bcd_and_ranges);
    RUN_TEST(test_os_flag_ignored_on_decode_and_clear_on_encode);
    return UNITY_END();
}
