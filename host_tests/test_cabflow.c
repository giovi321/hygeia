/* The Cabinet open teach view's phase, status and cue mapping (hy/cabflow.h) */
#include <string.h>
#include "unity.h"
#include "hy/cabflow.h"

void setUp(void) {}
void tearDown(void) {}

static const hy_cabev_kind_t ALL_KINDS[] = {
    HY_CABEV_REACT, HY_CABEV_MATCH, HY_CABEV_OTHER, HY_CABEV_TOO_SMALL, HY_CABEV_TOO_LONG, HY_CABEV_TRIGGER,
    HY_CABEV_TEACH_STILL, HY_CABEV_TEACH_GO, HY_CABEV_TEACH_TAKE, HY_CABEV_TEACH_ODD, HY_CABEV_TEACH_DONE,
    HY_CABEV_TEACH_FAIL, HY_CABEV_FAULT, HY_CABEV_TEST_END, HY_CABEV_TEACH_READY, HY_CABEV_CHECK_END,
    HY_CABEV_STATE,
};
#define N_KINDS (sizeof ALL_KINDS / sizeof ALL_KINDS[0])

static hy_cf_t at(uint8_t phase)
{
    hy_cf_t f;
    hy_cab_flow_init(&f);
    f.phase = phase;
    return f;
}

static void test_event_names_and_values_are_appended(void)
{
    /* the order is public (the console log, the docs): the new events come after TEST_END, none was moved */
    TEST_ASSERT_EQUAL_INT(0, HY_CABEV_REACT);
    TEST_ASSERT_EQUAL_INT(12, HY_CABEV_FAULT);
    TEST_ASSERT_EQUAL_INT(13, HY_CABEV_TEST_END);
    TEST_ASSERT_EQUAL_INT(14, HY_CABEV_TEACH_READY);
    TEST_ASSERT_EQUAL_INT(15, HY_CABEV_CHECK_END);
    TEST_ASSERT_EQUAL_INT(16, HY_CABEV_STATE);
}

static void test_idle_ignores_every_event(void)
{
    for (unsigned k = 0; k < N_KINDS; k++) {
        for (uint8_t take = 0; take <= 5; take++) {
            hy_cf_t f = at(HY_CF_IDLE);
            hy_cf_out_t o = hy_cab_flow_event(&f, ALL_KINDS[k], take);
            TEST_ASSERT_EQUAL_UINT8(HY_CF_IDLE, o.phase);
            TEST_ASSERT_EQUAL_INT(HY_CUE_NONE, o.cue);
        }
    }
}

static void test_phases_without_a_run_ignore_every_event(void)
{
    static const uint8_t quiet[] = {HY_CF_SAVED, HY_CF_CHECK_OK, HY_CF_CHECK_MISS, HY_CF_CHECK_NOMOVE, HY_CF_FAILED};
    for (unsigned i = 0; i < sizeof quiet; i++) {
        for (unsigned k = 0; k < N_KINDS; k++) {
            hy_cf_t f = at(quiet[i]);
            hy_cf_out_t o = hy_cab_flow_event(&f, ALL_KINDS[k], 1);
            TEST_ASSERT_EQUAL_UINT8(quiet[i], o.phase);
            TEST_ASSERT_EQUAL_UINT8(quiet[i], f.phase);
            TEST_ASSERT_EQUAL_INT(HY_CUE_NONE, o.cue);
        }
    }
}

static void test_teaching_status_and_cues(void)
{
    static const struct { hy_cabev_kind_t k; uint8_t take; uint8_t status; hy_cab_cue_t cue; } c[] = {
        {HY_CABEV_TEACH_READY, 1, HY_CF_S_READY, HY_CUE_NONE},
        {HY_CABEV_TEACH_STILL, 1, HY_CF_S_STILL, HY_CUE_NONE},
        {HY_CABEV_TEACH_GO, 1, HY_CF_S_NOW, HY_CUE_GO},
        {HY_CABEV_TEACH_TAKE, 1, HY_CF_S_GOT, HY_CUE_TAKE},
        {HY_CABEV_TEACH_TAKE, 4, HY_CF_S_GOT, HY_CUE_TAKE},
        {HY_CABEV_TOO_SMALL, 2, HY_CF_S_SMALL, HY_CUE_REDO},
        {HY_CABEV_TOO_LONG, 2, HY_CF_S_LONG, HY_CUE_REDO},
        {HY_CABEV_TEACH_ODD, 3, HY_CF_S_ODD, HY_CUE_REDO},
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        hy_cf_t f = at(HY_CF_RUNNING);
        hy_cf_out_t o = hy_cab_flow_event(&f, c[i].k, c[i].take);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(HY_CF_RUNNING, o.phase, "phase stays");
        TEST_ASSERT_EQUAL_UINT8(c[i].status, o.status);
        TEST_ASSERT_EQUAL_INT(c[i].cue, o.cue);
        TEST_ASSERT_EQUAL_UINT8(c[i].status, f.status);
    }
}

/* The last take plays no TAKE cue: DONE, REDO or FAIL follows within the build time */
static void test_last_take_plays_no_take_cue(void)
{
    hy_cf_t f = at(HY_CF_RUNNING);
    hy_cf_out_t o = hy_cab_flow_event(&f, HY_CABEV_TEACH_TAKE, HY_CAB_TAKES - 1);
    TEST_ASSERT_EQUAL_INT(HY_CUE_TAKE, o.cue);
    o = hy_cab_flow_event(&f, HY_CABEV_TEACH_TAKE, HY_CAB_TAKES);
    TEST_ASSERT_EQUAL_INT(HY_CUE_NONE, o.cue);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_S_GOT, o.status);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_RUNNING, o.phase);
}

/* A check that stopped returns to SAVED with a status line the view shows, and the next Test clears it */
static void test_check_stops_give_a_status_and_the_fail_cue(void)
{
    hy_cf_t f = at(HY_CF_CHECKING);
    hy_cf_out_t o = hy_cab_flow_event(&f, HY_CABEV_CHECK_END, HY_CAB_CHECK_NO_STILL);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_S_C_NOSTILL, o.status);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_S_C_NOSTILL, f.status);
    TEST_ASSERT_EQUAL_INT(HY_CUE_FAIL, o.cue);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_CHECKING, hy_cab_flow_user(&f, HY_CF_U_TEST));
    TEST_ASSERT_EQUAL_UINT8(HY_CF_S_NONE, f.status);
    o = hy_cab_flow_event(&f, HY_CABEV_CHECK_END, HY_CAB_CHECK_ABORT);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_S_C_STOPPED, o.status);
    TEST_ASSERT_EQUAL_INT(HY_CUE_FAIL, o.cue);
}

/* The take asked for again after an odd one is the last capture before the build: no TAKE cue, then TAKE again */
static void test_redo_of_an_odd_take_plays_no_take_cue(void)
{
    hy_cf_t f = at(HY_CF_RUNNING);
    hy_cf_out_t o = hy_cab_flow_event(&f, HY_CABEV_TEACH_ODD, 3);
    TEST_ASSERT_EQUAL_INT(HY_CUE_REDO, o.cue);
    o = hy_cab_flow_event(&f, HY_CABEV_TEACH_TAKE, 3);
    TEST_ASSERT_EQUAL_INT(HY_CUE_NONE, o.cue);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_S_GOT, o.status);
    o = hy_cab_flow_event(&f, HY_CABEV_TEACH_TAKE, 2);   /* the bit is spent */
    TEST_ASSERT_EQUAL_INT(HY_CUE_TAKE, o.cue);
    /* a redo that was too small keeps the bit until the take is counted */
    hy_cab_flow_event(&f, HY_CABEV_TEACH_ODD, 1);
    o = hy_cab_flow_event(&f, HY_CABEV_TOO_SMALL, 1);
    TEST_ASSERT_EQUAL_INT(HY_CUE_REDO, o.cue);
    o = hy_cab_flow_event(&f, HY_CABEV_TEACH_TAKE, 1);
    TEST_ASSERT_EQUAL_INT(HY_CUE_NONE, o.cue);
}

static void test_teach_done_goes_to_saved_with_the_done_cue(void)
{
    hy_cf_t f = at(HY_CF_RUNNING);
    hy_cf_out_t o = hy_cab_flow_event(&f, HY_CABEV_TEACH_DONE, HY_CAB_TAKES);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_SAVED, o.phase);
    TEST_ASSERT_EQUAL_INT(HY_CUE_DONE, o.cue);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_SAVED, f.phase);
}

static void test_teach_fail_goes_to_failed_with_the_fail_cue(void)
{
    static const struct { uint8_t why; uint8_t status; } c[] = {
        {HY_CAB_FAIL_STILL, HY_CF_S_STOPPED}, {HY_CAB_FAIL_NO_MOVE, HY_CF_S_NOMOVE}, {HY_CAB_FAIL_MODEL, HY_CF_S_STOPPED},
        {HY_CAB_FAIL_SAVE, HY_CF_S_IO},       {HY_CAB_FAIL_ABORT, HY_CF_S_STOPPED},
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        hy_cf_t f = at(HY_CF_RUNNING);
        hy_cf_out_t o = hy_cab_flow_event(&f, HY_CABEV_TEACH_FAIL, c[i].why);
        TEST_ASSERT_EQUAL_UINT8(HY_CF_FAILED, o.phase);
        TEST_ASSERT_EQUAL_UINT8(c[i].status, o.status);
        TEST_ASSERT_EQUAL_INT(HY_CUE_FAIL, o.cue);
    }
}

static void test_running_ignores_events_of_other_features(void)
{
    static const hy_cabev_kind_t other[] = {HY_CABEV_REACT, HY_CABEV_MATCH, HY_CABEV_OTHER, HY_CABEV_TRIGGER,
                                            HY_CABEV_FAULT, HY_CABEV_TEST_END, HY_CABEV_CHECK_END};
    for (unsigned i = 0; i < sizeof other / sizeof other[0]; i++) {
        hy_cf_t f = at(HY_CF_RUNNING);
        f.status = HY_CF_S_NOW;
        hy_cf_out_t o = hy_cab_flow_event(&f, other[i], HY_CAB_CHECK_OK);
        TEST_ASSERT_EQUAL_UINT8(HY_CF_RUNNING, o.phase);
        TEST_ASSERT_EQUAL_UINT8(HY_CF_S_NOW, o.status);
        TEST_ASSERT_EQUAL_INT(HY_CUE_NONE, o.cue);
    }
}

static void test_checking_status_and_cues(void)
{
    static const struct { hy_cabev_kind_t k; uint8_t status; hy_cab_cue_t cue; } c[] = {
        {HY_CABEV_TEACH_READY, HY_CF_S_READY, HY_CUE_NONE}, {HY_CABEV_TEACH_STILL, HY_CF_S_STILL, HY_CUE_NONE},
        {HY_CABEV_TEACH_GO, HY_CF_S_NOW, HY_CUE_GO},        {HY_CABEV_TOO_SMALL, HY_CF_S_SMALL, HY_CUE_REDO},
        {HY_CABEV_TOO_LONG, HY_CF_S_LONG, HY_CUE_REDO},
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        hy_cf_t f = at(HY_CF_CHECKING);
        hy_cf_out_t o = hy_cab_flow_event(&f, c[i].k, 1);
        TEST_ASSERT_EQUAL_UINT8(HY_CF_CHECKING, o.phase);
        TEST_ASSERT_EQUAL_UINT8(c[i].status, o.status);
        TEST_ASSERT_EQUAL_INT(c[i].cue, o.cue);
    }
}

static void test_check_outcomes(void)
{
    static const struct { uint8_t result; uint8_t phase; hy_cab_cue_t cue; } c[] = {
        {HY_CAB_CHECK_OK, HY_CF_CHECK_OK, HY_CUE_DONE},
        {HY_CAB_CHECK_MISS, HY_CF_CHECK_MISS, HY_CUE_FAIL},
        {HY_CAB_CHECK_NO_MOVE, HY_CF_CHECK_NOMOVE, HY_CUE_FAIL},
        {HY_CAB_CHECK_NO_STILL, HY_CF_SAVED, HY_CUE_FAIL},
        {HY_CAB_CHECK_ABORT, HY_CF_SAVED, HY_CUE_FAIL},
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        hy_cf_t f = at(HY_CF_CHECKING);
        hy_cf_out_t o = hy_cab_flow_event(&f, HY_CABEV_CHECK_END, c[i].result);
        TEST_ASSERT_EQUAL_UINT8(c[i].phase, o.phase);
        TEST_ASSERT_EQUAL_INT(c[i].cue, o.cue);
        TEST_ASSERT_EQUAL_UINT8(c[i].phase, f.phase);
    }
}

static void test_checking_ignores_teach_only_events(void)
{
    static const hy_cabev_kind_t teach_only[] = {HY_CABEV_TEACH_TAKE, HY_CABEV_TEACH_ODD, HY_CABEV_TEACH_DONE,
                                                 HY_CABEV_TEACH_FAIL, HY_CABEV_REACT, HY_CABEV_MATCH};
    for (unsigned i = 0; i < sizeof teach_only / sizeof teach_only[0]; i++) {
        hy_cf_t f = at(HY_CF_CHECKING);
        hy_cf_out_t o = hy_cab_flow_event(&f, teach_only[i], 1);
        TEST_ASSERT_EQUAL_UINT8(HY_CF_CHECKING, o.phase);
        TEST_ASSERT_EQUAL_INT(HY_CUE_NONE, o.cue);
    }
}

static void test_buttons_move_between_phases(void)
{
    hy_cf_t f;
    hy_cab_flow_init(&f);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_IDLE, f.phase);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_IDLE, hy_cab_flow_user(&f, HY_CF_U_TEST));    /* no Test in IDLE */
    TEST_ASSERT_EQUAL_UINT8(HY_CF_IDLE, hy_cab_flow_user(&f, HY_CF_U_AGAIN));   /* nor Teach again */
    TEST_ASSERT_EQUAL_UINT8(HY_CF_RUNNING, hy_cab_flow_user(&f, HY_CF_U_START));
    hy_cab_flow_event(&f, HY_CABEV_TEACH_DONE, HY_CAB_TAKES);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_SAVED, f.phase);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_SAVED, hy_cab_flow_user(&f, HY_CF_U_START));   /* no Start in SAVED */
    TEST_ASSERT_EQUAL_UINT8(HY_CF_CHECKING, hy_cab_flow_user(&f, HY_CF_U_TEST));
    hy_cab_flow_event(&f, HY_CABEV_CHECK_END, HY_CAB_CHECK_MISS);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_CHECK_MISS, f.phase);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_RUNNING, hy_cab_flow_user(&f, HY_CF_U_AGAIN));   /* Teach again restarts at once */
    TEST_ASSERT_EQUAL_UINT8(HY_CF_S_NONE, f.status);
    hy_cab_flow_event(&f, HY_CABEV_TEACH_FAIL, HY_CAB_FAIL_NO_MOVE);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_FAILED, f.phase);
    TEST_ASSERT_EQUAL_UINT8(HY_CF_RUNNING, hy_cab_flow_user(&f, HY_CF_U_START));   /* Start again after a failure */
}

static void test_cue_names(void)
{
    TEST_ASSERT_EQUAL_STRING("none", hy_cab_cue_name(HY_CUE_NONE));
    TEST_ASSERT_EQUAL_STRING("go", hy_cab_cue_name(HY_CUE_GO));
    TEST_ASSERT_EQUAL_STRING("take", hy_cab_cue_name(HY_CUE_TAKE));
    TEST_ASSERT_EQUAL_STRING("redo", hy_cab_cue_name(HY_CUE_REDO));
    TEST_ASSERT_EQUAL_STRING("done", hy_cab_cue_name(HY_CUE_DONE));
    TEST_ASSERT_EQUAL_STRING("fail", hy_cab_cue_name(HY_CUE_FAIL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_event_names_and_values_are_appended);
    RUN_TEST(test_idle_ignores_every_event);
    RUN_TEST(test_phases_without_a_run_ignore_every_event);
    RUN_TEST(test_teaching_status_and_cues);
    RUN_TEST(test_last_take_plays_no_take_cue);
    RUN_TEST(test_check_stops_give_a_status_and_the_fail_cue);
    RUN_TEST(test_redo_of_an_odd_take_plays_no_take_cue);
    RUN_TEST(test_teach_done_goes_to_saved_with_the_done_cue);
    RUN_TEST(test_teach_fail_goes_to_failed_with_the_fail_cue);
    RUN_TEST(test_running_ignores_events_of_other_features);
    RUN_TEST(test_checking_status_and_cues);
    RUN_TEST(test_check_outcomes);
    RUN_TEST(test_checking_ignores_teach_only_events);
    RUN_TEST(test_buttons_move_between_phases);
    RUN_TEST(test_cue_names);
    return UNITY_END();
}
