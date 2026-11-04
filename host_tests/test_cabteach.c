/* The Cabinet open teach and check sequencer (hy/cabteach.h), driven the way the runner drives it: one stream
   sample every 20 ms, the real still tracker, the still tracker reset when STILL begins. */
#include <string.h>
#include "unity.h"
#include "hy/cabteach.h"

typedef struct {
    hy_ct_t c;
    hy_cab_still_t trk;
    uint32_t t;
    int steps;
} sim_t;

void setUp(void) {}
void tearDown(void) {}

static hy_ct_out_t sim_begin(sim_t *s, hy_ct_mode_t mode, uint32_t t0)
{
    memset(s, 0, sizeof *s);
    s->t = t0;
    hy_cab_still_reset(&s->trk);
    return hy_ct_begin(&s->c, mode, t0);
}

/* One 20 ms sample. moving: the accel jumps about (not still); hit: the trigger fires. */
static hy_ct_out_t sim_step(sim_t *s, bool moving, bool hit)
{
    s->t += 20;
    s->steps++;
    hy_accel_t a = {0, 0, 1000};
    if (moving) a.x = (int16_t)(((s->t / 20) % 2) ? 300 : -300);
    hy_accel_t r;
    bool still = hy_cab_still_feed(&s->trk, s->t, a, &r);
    hy_ct_out_t o = hy_ct_step(&s->c, s->t, still, hit);
    if (o.reset_still) hy_cab_still_reset(&s->trk);
    return o;
}

/* Steps until an output carries event ev (returned), or for at most max_ms (has_ev false then). Any other event
   on the way fails the test unless it is one of the allowed STILL / GO kinds the caller passes through. */
static hy_ct_out_t until(sim_t *s, bool moving, bool hit, hy_cabev_kind_t ev, uint32_t max_ms)
{
    uint32_t t_end = s->t + max_ms;
    while ((int32_t)(s->t - t_end) < 0) {
        hy_ct_out_t o = sim_step(s, moving, hit);
        if (o.has_ev && o.ev == ev) return o;
        TEST_ASSERT_FALSE_MESSAGE(o.has_ev, "unexpected event");
        TEST_ASSERT_FALSE_MESSAGE(o.start_capture, "unexpected capture start");
    }
    hy_ct_out_t none;
    memset(&none, 0, sizeof none);
    return none;
}

static void expect_ev(hy_ct_out_t o, hy_cabev_kind_t ev, uint8_t take)
{
    TEST_ASSERT_TRUE(o.has_ev);
    TEST_ASSERT_EQUAL_INT(ev, o.ev);
    TEST_ASSERT_EQUAL_UINT8(take, o.take);
}

/* From the start (or the end of a pause) to GO for the given take, not moving */
static uint32_t to_go(sim_t *s, uint8_t take)
{
    hy_ct_out_t o = until(s, false, false, HY_CABEV_TEACH_STILL, 30000);
    expect_ev(o, HY_CABEV_TEACH_STILL, take);
    TEST_ASSERT_TRUE(o.reset_still);
    uint32_t still_at = s->t;
    o = until(s, false, false, HY_CABEV_TEACH_GO, 5000);
    expect_ev(o, HY_CABEV_TEACH_GO, take);
    TEST_ASSERT_TRUE(o.take_rest);
    return s->t - still_at;
}

/* GO, the trigger, a capture that ends with res */
static hy_ct_out_t one_take(sim_t *s, uint8_t take, hy_cap_res_t res)
{
    to_go(s, take);
    hy_ct_out_t o = sim_step(s, true, true);
    TEST_ASSERT_TRUE(o.start_capture);
    TEST_ASSERT_EQUAL_UINT8(HY_CT_CAP, s->c.phase);
    s->t += 400;   /* the capture lasts 400 ms */
    return hy_ct_capture_done(&s->c, s->t, res);
}

static void test_begin_posts_ready_and_waits_the_lead(void)
{
    sim_t s;
    hy_ct_out_t o = sim_begin(&s, HY_CT_TEACH, 1000);
    expect_ev(o, HY_CABEV_TEACH_READY, 1);
    TEST_ASSERT_EQUAL_UINT8(HY_CT_LEAD, s.c.phase);
    TEST_ASSERT_TRUE(hy_ct_active(&s.c));
}

static void test_no_go_before_lead_even_when_still_from_start(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    /* perfectly still from t = 0, and a trigger hit on every sample: nothing happens for the whole lead */
    while (s.t + 20 < HY_CT_LEAD_MS) {
        hy_ct_out_t o = sim_step(&s, false, true);
        TEST_ASSERT_FALSE(o.has_ev);
        TEST_ASSERT_FALSE(o.start_capture);
        TEST_ASSERT_EQUAL_UINT8(HY_CT_LEAD, s.c.phase);
    }
    hy_ct_out_t o = sim_step(&s, false, false);
    TEST_ASSERT_EQUAL_UINT32(HY_CT_LEAD_MS, s.t);
    expect_ev(o, HY_CABEV_TEACH_STILL, 1);
    TEST_ASSERT_TRUE(o.reset_still);
}

static void test_go_only_after_lead_plus_one_second_still(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    uint32_t waited = to_go(&s, 1);
    TEST_ASSERT_TRUE(waited >= HY_CAB_STILL_MS);
    TEST_ASSERT_TRUE(waited <= HY_CAB_STILL_MS + 60);
    TEST_ASSERT_TRUE(s.t >= HY_CT_LEAD_MS + HY_CAB_STILL_MS);
    TEST_ASSERT_TRUE(s.t <= HY_CT_LEAD_MS + HY_CAB_STILL_MS + 60);
}

static void test_hit_is_ignored_before_go(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    until(&s, false, true, HY_CABEV_TEACH_STILL, 6000);   /* a hit on every sample through the lead */
    /* in STILL a hit is not a capture either */
    for (int i = 0; i < 10; i++) {
        hy_ct_out_t o = sim_step(&s, true, true);
        TEST_ASSERT_FALSE(o.start_capture);
        TEST_ASSERT_EQUAL_UINT8(HY_CT_STILL, s.c.phase);
    }
}

static void test_five_takes_then_build_and_save(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 100);
    for (uint8_t k = 1; k <= HY_CAB_TAKES; k++) {
        hy_ct_out_t o = one_take(&s, k, HY_CAP_DONE);
        expect_ev(o, HY_CABEV_TEACH_TAKE, k);
        TEST_ASSERT_TRUE(o.store);
        TEST_ASSERT_EQUAL_UINT8(k - 1, o.slot);
        if (k < HY_CAB_TAKES) {
            TEST_ASSERT_FALSE(o.build);
            TEST_ASSERT_EQUAL_UINT8(HY_CT_PAUSE, s.c.phase);
        } else {
            TEST_ASSERT_TRUE(o.build);
            TEST_ASSERT_EQUAL_UINT8(HY_CT_BUILD, s.c.phase);
        }
    }
    hy_ct_out_t o = hy_ct_build_result(&s.c, s.t, true, -1, true);
    TEST_ASSERT_TRUE(o.save);
    TEST_ASSERT_FALSE(o.accept_as_is);
    TEST_ASSERT_FALSE(o.has_ev);
    o = hy_ct_saved(&s.c, true);
    expect_ev(o, HY_CABEV_TEACH_DONE, HY_CAB_TAKES);
    TEST_ASSERT_TRUE(o.end);
    TEST_ASSERT_FALSE(hy_ct_active(&s.c));
}

static void test_save_failure_ends_with_save_fail(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    for (uint8_t k = 1; k <= HY_CAB_TAKES; k++) one_take(&s, k, HY_CAP_DONE);
    hy_ct_build_result(&s.c, s.t, true, -1, true);
    hy_ct_out_t o = hy_ct_saved(&s.c, false);
    expect_ev(o, HY_CABEV_TEACH_FAIL, HY_CAB_FAIL_SAVE);
    TEST_ASSERT_TRUE(o.end);
}

static void test_movement_in_pause_is_ignored_for_five_seconds(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    one_take(&s, 1, HY_CAP_DONE);
    uint32_t t_take = s.t;
    /* moving and hitting on every sample through the pause: no event, no capture, until the pause ends */
    uint32_t t_end = t_take + HY_CT_PAUSE_MS;
    hy_ct_out_t o;
    memset(&o, 0, sizeof o);
    while (s.t < t_end) {
        o = sim_step(&s, true, true);
        if (o.has_ev) break;
        TEST_ASSERT_FALSE(o.start_capture);
    }
    expect_ev(o, HY_CABEV_TEACH_STILL, 2);
    TEST_ASSERT_TRUE(s.t >= t_end);
    TEST_ASSERT_TRUE(s.t <= t_end + 20);
}

static void test_too_small_asks_for_the_same_take_again(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    one_take(&s, 1, HY_CAP_DONE);
    hy_ct_out_t o = one_take(&s, 2, HY_CAP_TOO_SHORT);
    expect_ev(o, HY_CABEV_TOO_SMALL, 2);
    TEST_ASSERT_FALSE(o.store);
    TEST_ASSERT_EQUAL_UINT8(1, s.c.takes_n);
    TEST_ASSERT_EQUAL_UINT8(HY_CT_PAUSE, s.c.phase);
    o = one_take(&s, 2, HY_CAP_TOO_LONG);   /* the same take again: STILL and GO name take 2 */
    expect_ev(o, HY_CABEV_TOO_LONG, 2);
    o = one_take(&s, 2, HY_CAP_DONE);
    expect_ev(o, HY_CABEV_TEACH_TAKE, 2);
    TEST_ASSERT_EQUAL_UINT8(2, s.c.takes_n);
    TEST_ASSERT_EQUAL_UINT8(2, s.c.cur);   /* take 3 next */
}

static void test_odd_take_is_redone_at_most_twice_then_accepted(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    for (uint8_t k = 1; k <= HY_CAB_TAKES; k++) one_take(&s, k, HY_CAP_DONE);
    hy_ct_out_t o = hy_ct_build_result(&s.c, s.t, false, 2, true);
    expect_ev(o, HY_CABEV_TEACH_ODD, 3);
    TEST_ASSERT_FALSE(o.save);
    TEST_ASSERT_EQUAL_UINT8(HY_CT_PAUSE, s.c.phase);
    o = one_take(&s, 3, HY_CAP_DONE);   /* the redo replaces take 3 only, and builds again at once */
    expect_ev(o, HY_CABEV_TEACH_TAKE, 3);
    TEST_ASSERT_EQUAL_UINT8(2, o.slot);
    TEST_ASSERT_TRUE(o.build);
    o = hy_ct_build_result(&s.c, s.t, false, 4, true);
    expect_ev(o, HY_CABEV_TEACH_ODD, 5);
    o = one_take(&s, 5, HY_CAP_DONE);
    TEST_ASSERT_TRUE(o.build);
    /* the third time it is accepted as it is */
    o = hy_ct_build_result(&s.c, s.t, false, 4, true);
    TEST_ASSERT_FALSE(o.has_ev);
    TEST_ASSERT_TRUE(o.save);
    TEST_ASSERT_TRUE(o.accept_as_is);
    o = hy_ct_saved(&s.c, true);
    expect_ev(o, HY_CABEV_TEACH_DONE, HY_CAB_TAKES);
}

static void test_odd_take_with_infinite_distance_fails_the_model(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    for (uint8_t k = 1; k <= HY_CAB_TAKES; k++) one_take(&s, k, HY_CAP_DONE);
    /* two redos used up, then no finite d_self */
    hy_ct_build_result(&s.c, s.t, false, 0, true);
    one_take(&s, 1, HY_CAP_DONE);
    hy_ct_build_result(&s.c, s.t, false, 0, true);
    one_take(&s, 1, HY_CAP_DONE);
    hy_ct_out_t o = hy_ct_build_result(&s.c, s.t, false, 0, false);
    expect_ev(o, HY_CABEV_TEACH_FAIL, HY_CAB_FAIL_MODEL);
    TEST_ASSERT_TRUE(o.end);
}

static void test_a_model_that_is_not_ok_and_has_no_odd_take_fails(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    for (uint8_t k = 1; k <= HY_CAB_TAKES; k++) one_take(&s, k, HY_CAP_DONE);
    hy_ct_out_t o = hy_ct_build_result(&s.c, s.t, false, HY_CAB_ODD_GYRO_MIX, true);
    expect_ev(o, HY_CABEV_TEACH_FAIL, HY_CAB_FAIL_MODEL);
}

static void test_still_times_out_after_twenty_seconds(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    until(&s, true, false, HY_CABEV_TEACH_STILL, 6000);   /* moving all the time */
    uint32_t t_still = s.t;
    hy_ct_out_t o = until(&s, true, false, HY_CABEV_TEACH_FAIL, HY_CT_STILL_MS - 40);   /* not yet */
    TEST_ASSERT_FALSE(o.has_ev);
    o = until(&s, true, false, HY_CABEV_TEACH_FAIL, 100);
    expect_ev(o, HY_CABEV_TEACH_FAIL, HY_CAB_FAIL_STILL);
    TEST_ASSERT_TRUE(o.end);
    TEST_ASSERT_EQUAL_UINT32(t_still + HY_CT_STILL_MS, s.t);
    TEST_ASSERT_FALSE(hy_ct_active(&s.c));
}

static void test_go_times_out_after_sixty_seconds(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    to_go(&s, 1);
    uint32_t t_go = s.t;
    hy_ct_out_t o = until(&s, false, false, HY_CABEV_TEACH_FAIL, HY_CT_GO_MS - 40);
    TEST_ASSERT_FALSE(o.has_ev);
    o = until(&s, false, false, HY_CABEV_TEACH_FAIL, 100);
    expect_ev(o, HY_CABEV_TEACH_FAIL, HY_CAB_FAIL_NO_MOVE);
    TEST_ASSERT_TRUE(o.end);
    TEST_ASSERT_EQUAL_UINT32(t_go + HY_CT_GO_MS, s.t);
}

static void test_failed_capture_start_asks_again(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    to_go(&s, 1);
    hy_ct_out_t o = sim_step(&s, true, true);
    TEST_ASSERT_TRUE(o.start_capture);
    o = hy_ct_capture_failed(&s.c, s.t);
    TEST_ASSERT_FALSE(o.has_ev);
    TEST_ASSERT_EQUAL_UINT8(HY_CT_PAUSE, s.c.phase);
    TEST_ASSERT_EQUAL_UINT8(0, s.c.takes_n);
    o = until(&s, false, false, HY_CABEV_TEACH_STILL, 6000);
    expect_ev(o, HY_CABEV_TEACH_STILL, 1);
}

static void test_abort_ends_a_teach(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0);
    one_take(&s, 1, HY_CAP_DONE);
    hy_ct_out_t o = hy_ct_abort(&s.c);
    expect_ev(o, HY_CABEV_TEACH_FAIL, HY_CAB_FAIL_ABORT);
    TEST_ASSERT_TRUE(o.end);
    TEST_ASSERT_FALSE(hy_ct_active(&s.c));
}

static void test_the_clock_may_wrap_during_a_run(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_TEACH, 0xFFFFFFFFu - 2500u);   /* the 32-bit ms clock wraps inside the lead */
    to_go(&s, 1);
    hy_ct_out_t o = sim_step(&s, true, true);
    TEST_ASSERT_TRUE(o.start_capture);
    o = hy_ct_capture_done(&s.c, s.t, HY_CAP_DONE);
    expect_ev(o, HY_CABEV_TEACH_TAKE, 1);
    o = until(&s, false, false, HY_CABEV_TEACH_STILL, 6000);
    expect_ev(o, HY_CABEV_TEACH_STILL, 2);
}

/* ---- check ---- */

static void test_check_one_take_then_a_match(void)
{
    sim_t s;
    hy_ct_out_t o = sim_begin(&s, HY_CT_CHECK, 0);
    expect_ev(o, HY_CABEV_TEACH_READY, 1);
    o = one_take(&s, 1, HY_CAP_DONE);
    TEST_ASSERT_FALSE(o.has_ev);   /* a check counts no take */
    TEST_ASSERT_TRUE(o.store);
    TEST_ASSERT_EQUAL_UINT8(0, o.slot);
    TEST_ASSERT_TRUE(o.match);
    TEST_ASSERT_FALSE(o.build);
    o = hy_ct_check_result(&s.c, true);
    expect_ev(o, HY_CABEV_CHECK_END, HY_CAB_CHECK_OK);
    TEST_ASSERT_TRUE(o.end);
    TEST_ASSERT_FALSE(hy_ct_active(&s.c));
}

static void test_check_miss(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_CHECK, 0);
    one_take(&s, 1, HY_CAP_DONE);
    hy_ct_out_t o = hy_ct_check_result(&s.c, false);
    expect_ev(o, HY_CABEV_CHECK_END, HY_CAB_CHECK_MISS);
    TEST_ASSERT_TRUE(o.end);
}

static void test_check_too_small_is_a_redo_of_the_single_take(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_CHECK, 0);
    hy_ct_out_t o = one_take(&s, 1, HY_CAP_TOO_SHORT);
    expect_ev(o, HY_CABEV_TOO_SMALL, 1);
    o = one_take(&s, 1, HY_CAP_TOO_LONG);
    expect_ev(o, HY_CABEV_TOO_LONG, 1);
    o = one_take(&s, 1, HY_CAP_DONE);
    TEST_ASSERT_TRUE(o.match);
}

static void test_check_no_move_outcome(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_CHECK, 0);
    to_go(&s, 1);
    hy_ct_out_t o = until(&s, false, false, HY_CABEV_CHECK_END, HY_CT_GO_MS + 100);
    expect_ev(o, HY_CABEV_CHECK_END, HY_CAB_CHECK_NO_MOVE);
    TEST_ASSERT_TRUE(o.end);
}

static void test_check_not_still_is_an_abort(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_CHECK, 0);
    until(&s, true, false, HY_CABEV_TEACH_STILL, 6000);
    hy_ct_out_t o = until(&s, true, false, HY_CABEV_CHECK_END, HY_CT_STILL_MS + 100);
    expect_ev(o, HY_CABEV_CHECK_END, HY_CAB_CHECK_ABORT);
    TEST_ASSERT_TRUE(o.end);
}

static void test_check_abort(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_CHECK, 0);
    to_go(&s, 1);
    hy_ct_out_t o = hy_ct_abort(&s.c);
    expect_ev(o, HY_CABEV_CHECK_END, HY_CAB_CHECK_ABORT);
    TEST_ASSERT_TRUE(o.end);
}

static void test_check_lead_and_pause_ignore_movement(void)
{
    sim_t s;
    sim_begin(&s, HY_CT_CHECK, 0);
    for (int i = 0; i < 200; i++) {   /* 4 s of the lead */
        hy_ct_out_t o = sim_step(&s, true, true);
        TEST_ASSERT_FALSE(o.has_ev);
        TEST_ASSERT_FALSE(o.start_capture);
    }
    one_take(&s, 1, HY_CAP_TOO_SHORT);
    uint32_t t_end = s.t + HY_CT_PAUSE_MS;
    while (s.t + 20 < t_end) {
        hy_ct_out_t o = sim_step(&s, true, true);
        TEST_ASSERT_FALSE(o.has_ev);
        TEST_ASSERT_FALSE(o.start_capture);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_begin_posts_ready_and_waits_the_lead);
    RUN_TEST(test_no_go_before_lead_even_when_still_from_start);
    RUN_TEST(test_go_only_after_lead_plus_one_second_still);
    RUN_TEST(test_hit_is_ignored_before_go);
    RUN_TEST(test_five_takes_then_build_and_save);
    RUN_TEST(test_save_failure_ends_with_save_fail);
    RUN_TEST(test_movement_in_pause_is_ignored_for_five_seconds);
    RUN_TEST(test_too_small_asks_for_the_same_take_again);
    RUN_TEST(test_odd_take_is_redone_at_most_twice_then_accepted);
    RUN_TEST(test_odd_take_with_infinite_distance_fails_the_model);
    RUN_TEST(test_a_model_that_is_not_ok_and_has_no_odd_take_fails);
    RUN_TEST(test_still_times_out_after_twenty_seconds);
    RUN_TEST(test_go_times_out_after_sixty_seconds);
    RUN_TEST(test_failed_capture_start_asks_again);
    RUN_TEST(test_abort_ends_a_teach);
    RUN_TEST(test_the_clock_may_wrap_during_a_run);
    RUN_TEST(test_check_one_take_then_a_match);
    RUN_TEST(test_check_miss);
    RUN_TEST(test_check_too_small_is_a_redo_of_the_single_take);
    RUN_TEST(test_check_no_move_outcome);
    RUN_TEST(test_check_not_still_is_an_abort);
    RUN_TEST(test_check_abort);
    RUN_TEST(test_check_lead_and_pause_ignore_movement);
    return UNITY_END();
}
