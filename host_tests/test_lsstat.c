#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "hy/lsstat.h"

void setUp(void) {}
void tearDown(void) {}

static void test_bucket_edges_include_the_lower_bound(void)
{
    TEST_ASSERT_EQUAL(0, hy_ls_bucket(0));
    TEST_ASSERT_EQUAL(0, hy_ls_bucket(49999));
    TEST_ASSERT_EQUAL(1, hy_ls_bucket(50000));
    TEST_ASSERT_EQUAL(1, hy_ls_bucket(199999));
    TEST_ASSERT_EQUAL(2, hy_ls_bucket(200000));
    TEST_ASSERT_EQUAL(2, hy_ls_bucket(599999));
    TEST_ASSERT_EQUAL(3, hy_ls_bucket(600000));
    TEST_ASSERT_EQUAL(3, hy_ls_bucket(1099999));
    TEST_ASSERT_EQUAL(4, hy_ls_bucket(1100000));
    TEST_ASSERT_EQUAL(4, hy_ls_bucket(2099999));
    TEST_ASSERT_EQUAL(5, hy_ls_bucket(2100000));
    TEST_ASSERT_EQUAL(5, hy_ls_bucket(60LL * 1000000));
}

static void test_bucket_of_a_negative_length_is_the_first(void)
{
    TEST_ASSERT_EQUAL(0, hy_ls_bucket(-5));
}

static void test_bucket_labels(void)
{
    for (int b = 0; b < HY_LS_BUCKETS; b++) {
        TEST_ASSERT_TRUE(strcmp(hy_ls_bucket_label(b), "?") != 0);
    }
    TEST_ASSERT_EQUAL_STRING("?", hy_ls_bucket_label(-1));
    TEST_ASSERT_EQUAL_STRING("?", hy_ls_bucket_label(HY_LS_BUCKETS));
}

static void test_scr_category(void)
{
    TEST_ASSERT_EQUAL(HY_ST_NONE, hy_scrtime_category(true, 0, false, false));
    TEST_ASSERT_EQUAL(HY_ST_NONE, hy_scrtime_category(true, 2, true, false));
    TEST_ASSERT_EQUAL(HY_ST_ON, hy_scrtime_category(false, 0, false, false));
    TEST_ASSERT_EQUAL(HY_ST_DIM, hy_scrtime_category(false, 1, false, false));
    TEST_ASSERT_EQUAL(HY_ST_OFF_ASLEEP, hy_scrtime_category(false, 2, true, false));
    TEST_ASSERT_EQUAL(HY_ST_OFF_HELD, hy_scrtime_category(false, 2, false, true));
    TEST_ASSERT_EQUAL(HY_ST_OFF_AWAKE, hy_scrtime_category(false, 2, false, false));
}

static void test_scr_time_goes_to_the_state_that_ended(void)
{
    hy_scrtime_t t;
    hy_scrtime_reset(&t);
    hy_scrtime_mark(&t, 1000, HY_ST_ON);          /* the first mark starts a stretch, nothing is counted */
    hy_scrtime_mark(&t, 31000, HY_ST_DIM);        /* 30 s on */
    hy_scrtime_mark(&t, 121000, HY_ST_OFF_AWAKE); /* 90 s dim */
    hy_scrtime_mark(&t, 121500, HY_ST_OFF_ASLEEP);
    hy_scrtime_mark(&t, 721500, HY_ST_NONE);      /* 600 s asleep, then USB */
    hy_scrtime_mark(&t, 900000, HY_ST_ON);        /* USB time is not counted */
    TEST_ASSERT_EQUAL_UINT64(30000, t.ms[HY_ST_ON]);
    TEST_ASSERT_EQUAL_UINT64(90000, t.ms[HY_ST_DIM]);
    TEST_ASSERT_EQUAL_UINT64(500, t.ms[HY_ST_OFF_AWAKE]);
    TEST_ASSERT_EQUAL_UINT64(600000, t.ms[HY_ST_OFF_ASLEEP]);
    TEST_ASSERT_EQUAL_UINT64(0, t.ms[HY_ST_OFF_HELD]);
    TEST_ASSERT_EQUAL(HY_ST_ON, t.cur);
}

static void test_scr_time_same_state_closes_the_stretch(void)
{
    hy_scrtime_t t;
    hy_scrtime_reset(&t);
    hy_scrtime_mark(&t, 0, HY_ST_OFF_ASLEEP);
    hy_scrtime_mark(&t, 5000, HY_ST_OFF_ASLEEP);   /* a dump: counted up to now, still asleep */
    hy_scrtime_mark(&t, 8000, HY_ST_OFF_ASLEEP);
    TEST_ASSERT_EQUAL_UINT64(8000, t.ms[HY_ST_OFF_ASLEEP]);
}

static void test_scr_time_clock_going_back_adds_nothing(void)
{
    hy_scrtime_t t;
    hy_scrtime_reset(&t);
    hy_scrtime_mark(&t, 10000, HY_ST_ON);
    hy_scrtime_mark(&t, 4000, HY_ST_DIM);
    TEST_ASSERT_EQUAL_UINT64(0, t.ms[HY_ST_ON]);
    hy_scrtime_mark(&t, 5000, HY_ST_DIM);
    TEST_ASSERT_EQUAL_UINT64(1000, t.ms[HY_ST_DIM]);
}

static void test_scr_time_a_bad_category_stops_timing(void)
{
    hy_scrtime_t t;
    hy_scrtime_reset(&t);
    hy_scrtime_mark(&t, 0, HY_ST_ON);
    hy_scrtime_mark(&t, 100, 99);
    TEST_ASSERT_EQUAL(HY_ST_NONE, t.cur);
    TEST_ASSERT_EQUAL_UINT64(100, t.ms[HY_ST_ON]);
    hy_scrtime_mark(&t, 500, HY_ST_ON);
    TEST_ASSERT_EQUAL_UINT64(100, t.ms[HY_ST_ON]);
}

static void test_ring_empty_and_partial(void)
{
    static hy_mv_ring_t r;
    hy_mv_ring_clear(&r);
    hy_mv_pt_t p;
    TEST_ASSERT_EQUAL_UINT32(0, hy_mv_ring_len(&r));
    TEST_ASSERT_FALSE(hy_mv_ring_get(&r, 0, &p));
    hy_mv_ring_push(&r, 60, 4100, 90, 2);
    hy_mv_ring_push(&r, 120, 4098, 90, 2);
    TEST_ASSERT_EQUAL_UINT32(2, hy_mv_ring_len(&r));
    TEST_ASSERT_TRUE(hy_mv_ring_get(&r, 0, &p));
    TEST_ASSERT_EQUAL_UINT32(60, p.t_s);
    TEST_ASSERT_EQUAL_UINT16(4100, p.mv);
    TEST_ASSERT_EQUAL_INT8(90, p.pct);
    TEST_ASSERT_EQUAL_UINT8(2, p.scr);
    TEST_ASSERT_TRUE(hy_mv_ring_get(&r, 1, &p));
    TEST_ASSERT_EQUAL_UINT32(120, p.t_s);
    TEST_ASSERT_FALSE(hy_mv_ring_get(&r, 2, &p));
}

static void test_ring_full_exactly_keeps_everything(void)
{
    static hy_mv_ring_t r;
    hy_mv_ring_clear(&r);
    for (uint32_t i = 0; i < HY_MV_RING; i++) {
        hy_mv_ring_push(&r, i * 60, (uint16_t)(4200 - i), 80, 2);
    }
    TEST_ASSERT_EQUAL_UINT32(HY_MV_RING, hy_mv_ring_len(&r));
    hy_mv_pt_t p;
    TEST_ASSERT_TRUE(hy_mv_ring_get(&r, 0, &p));
    TEST_ASSERT_EQUAL_UINT32(0, p.t_s);
    TEST_ASSERT_TRUE(hy_mv_ring_get(&r, HY_MV_RING - 1, &p));
    TEST_ASSERT_EQUAL_UINT32((HY_MV_RING - 1) * 60, p.t_s);
    TEST_ASSERT_FALSE(hy_mv_ring_get(&r, HY_MV_RING, &p));
}

static void test_ring_wraps_and_drops_the_oldest(void)
{
    static hy_mv_ring_t r;
    hy_mv_ring_clear(&r);
    const uint32_t extra = 40;
    for (uint32_t i = 0; i < HY_MV_RING + extra; i++) {
        hy_mv_ring_push(&r, i, (uint16_t)(3000 + i), 50, (uint8_t)(i % 3));
    }
    TEST_ASSERT_EQUAL_UINT32(HY_MV_RING, hy_mv_ring_len(&r));
    hy_mv_pt_t p;
    TEST_ASSERT_TRUE(hy_mv_ring_get(&r, 0, &p));
    TEST_ASSERT_EQUAL_UINT32(extra, p.t_s);          /* points 0..extra-1 are gone */
    TEST_ASSERT_TRUE(hy_mv_ring_get(&r, HY_MV_RING - 1, &p));
    TEST_ASSERT_EQUAL_UINT32(HY_MV_RING + extra - 1, p.t_s);
    /* oldest first, one step each */
    uint32_t prev = 0;
    for (uint32_t i = 0; i < HY_MV_RING; i++) {
        TEST_ASSERT_TRUE(hy_mv_ring_get(&r, i, &p));
        if (i) {
            TEST_ASSERT_EQUAL_UINT32(prev + 1, p.t_s);
        }
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(3000 + p.t_s), p.mv);
        prev = p.t_s;
    }
}

static void test_ring_wraps_many_times(void)
{
    static hy_mv_ring_t r;
    hy_mv_ring_clear(&r);
    for (uint32_t i = 0; i < HY_MV_RING * 5 + 7; i++) {
        hy_mv_ring_push(&r, i, 4000, 70, 0);
    }
    hy_mv_pt_t p;
    TEST_ASSERT_TRUE(hy_mv_ring_get(&r, 0, &p));
    TEST_ASSERT_EQUAL_UINT32(HY_MV_RING * 4 + 7, p.t_s);
    TEST_ASSERT_TRUE(hy_mv_ring_get(&r, HY_MV_RING - 1, &p));
    TEST_ASSERT_EQUAL_UINT32(HY_MV_RING * 5 + 6, p.t_s);
}

static void test_parse_list_and_all_off_and_mv(void)
{
    TEST_ASSERT_EQUAL(HY_PXC_LIST, hy_px_parse("").kind);
    TEST_ASSERT_EQUAL(HY_PXC_LIST, hy_px_parse("   ").kind);
    TEST_ASSERT_EQUAL(HY_PXC_LIST, hy_px_parse(NULL).kind);
    TEST_ASSERT_EQUAL(HY_PXC_ALL_OFF, hy_px_parse("off").kind);
    TEST_ASSERT_EQUAL(HY_PXC_MV, hy_px_parse("mv").kind);
    TEST_ASSERT_EQUAL(HY_PXC_MV_CLEAR, hy_px_parse("mv clear").kind);
    TEST_ASSERT_EQUAL(HY_PXC_MV_CLEAR, hy_px_parse("  mv   clear ").kind);
}

static void test_parse_every_flag_on_and_off(void)
{
    char line[32];
    for (int f = 0; f < HY_PX_N; f++) {
        snprintf(line, sizeof line, "%s on", hy_px_name(f));
        hy_pxc_t c = hy_px_parse(line);
        TEST_ASSERT_EQUAL(HY_PXC_SET, c.kind);
        TEST_ASSERT_EQUAL(f, c.flag);
        TEST_ASSERT_TRUE(c.on);
        snprintf(line, sizeof line, "%s off", hy_px_name(f));
        c = hy_px_parse(line);
        TEST_ASSERT_EQUAL(HY_PXC_SET, c.kind);
        TEST_ASSERT_EQUAL(f, c.flag);
        TEST_ASSERT_FALSE(c.on);
    }
}

static void test_parse_rejects_the_rest(void)
{
    TEST_ASSERT_EQUAL(HY_PXC_BAD, hy_px_parse("touch").kind);          /* needs on or off */
    TEST_ASSERT_EQUAL(HY_PXC_BAD, hy_px_parse("touch maybe").kind);
    TEST_ASSERT_EQUAL(HY_PXC_BAD, hy_px_parse("codec on").kind);       /* not built */
    TEST_ASSERT_EQUAL(HY_PXC_BAD, hy_px_parse("Touch on").kind);
    TEST_ASSERT_EQUAL(HY_PXC_BAD, hy_px_parse("touch on extra").kind);
    TEST_ASSERT_EQUAL(HY_PXC_BAD, hy_px_parse("off off").kind);
    TEST_ASSERT_EQUAL(HY_PXC_BAD, hy_px_parse("mv on").kind);
    TEST_ASSERT_EQUAL(HY_PXC_BAD, hy_px_parse("mv clear now").kind);
    TEST_ASSERT_EQUAL(HY_PXC_BAD, hy_px_parse("a b c d").kind);
    TEST_ASSERT_EQUAL(HY_PXC_BAD, hy_px_parse("averyveryverylongword on").kind);
}

static void test_flag_names(void)
{
    char out[64];
    hy_px_names(0, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("none", out);
    hy_px_names(1u << HY_PX_TOUCH, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("touch", out);
    hy_px_names((1u << HY_PX_PANEL) | (1u << HY_PX_LSVBUS), out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("panel lsvbus", out);
    hy_px_names((1u << HY_PX_N) - 1, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("touch nonet panel wakers lsvbus", out);
}

static void test_flag_names_truncate_safely(void)
{
    char out[8];
    hy_px_names((1u << HY_PX_N) - 1, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT(7, strlen(out));
    hy_px_names(0, out, 3);
    TEST_ASSERT_EQUAL_STRING("no", out);
    hy_px_names(1, out, 0);   /* no write, no crash */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bucket_edges_include_the_lower_bound);
    RUN_TEST(test_bucket_of_a_negative_length_is_the_first);
    RUN_TEST(test_bucket_labels);
    RUN_TEST(test_scr_category);
    RUN_TEST(test_scr_time_goes_to_the_state_that_ended);
    RUN_TEST(test_scr_time_same_state_closes_the_stretch);
    RUN_TEST(test_scr_time_clock_going_back_adds_nothing);
    RUN_TEST(test_scr_time_a_bad_category_stops_timing);
    RUN_TEST(test_ring_empty_and_partial);
    RUN_TEST(test_ring_full_exactly_keeps_everything);
    RUN_TEST(test_ring_wraps_and_drops_the_oldest);
    RUN_TEST(test_ring_wraps_many_times);
    RUN_TEST(test_parse_list_and_all_off_and_mv);
    RUN_TEST(test_parse_every_flag_on_and_off);
    RUN_TEST(test_parse_rejects_the_rest);
    RUN_TEST(test_flag_names);
    RUN_TEST(test_flag_names_truncate_safely);
    return UNITY_END();
}
