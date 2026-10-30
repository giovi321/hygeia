#include <string.h>
#include "unity.h"
#include "hy/sched.h"
#include "hy/occ.h"
#include "hy/types.h"

#define TZ_ZURICH "CET-1CEST,M3.5.0,M10.5.0/3"
#define TZ_NY     "EST5EDT,M3.2.0,M11.1.0"

/* Literal UTC epochs (same values as test_occ.c). */
#define T_2026_11_02_LOCAL_MIDNIGHT 1793574000LL /* 2026-11-01T23:00:00Z, Monday, CET */
#define T_2026_03_29_LOCAL_MIDNIGHT 1774738800LL /* 2026-03-28T23:00:00Z */
#define T_2026_03_29_0100Z          1774746000LL /* 03:00 CEST, first minute after the gap */
#define T_2026_07_15_0600Z          1784095200LL /* 08:00 CEST Zurich */
#define T_2026_07_15_1000Z          1784109600LL /* 06:00 EDT New York, 12:00 CEST Zurich */
#define DAY 86400LL
#define MIN 60LL
#define HOUR 3600LL

/* Local Zurich time on 2026-11-02. */
#define L(h, m) (T_2026_11_02_LOCAL_MIDNIGHT + (h) * HOUR + (m) * MIN)

void setUp(void) {}
void tearDown(void) {}

static hy_sched_t s;
static hy_sched_out_t out;
static hy_med_t meds[4];
static size_t n_meds;
static hy_log_t logs[8];
static size_t n_logs;
static hy_snooze_t snoozes[4];
static size_t n_snoozes;
static const char *tz = TZ_ZURICH;
static const hy_tzchange_t *change;

static void reset(void)
{
    hy_sched_init(&s);
    memset(&out, 0, sizeof out);
    memset(meds, 0, sizeof meds);
    memset(logs, 0, sizeof logs);
    memset(snoozes, 0, sizeof snoozes);
    n_meds = n_logs = n_snoozes = 0;
    tz = TZ_ZURICH;
    change = NULL;
}

/* A: Antibiotic, fixed 08:00 and 20:00, order 0. */
static hy_med_t *add_a(void)
{
    hy_med_t *m = &meds[n_meds++];
    hy_med_defaults(m);
    m->id = 1;
    strcpy(m->code, "A");
    strcpy(m->name, "Antibiotic");
    m->type = HY_SCHED_FIXED;
    m->times[0] = 8 * 60;
    m->times[1] = 20 * 60;
    m->n_times = 2;
    m->order = 0;
    return m;
}

/* V: Vitamin D, fixed 08:00, order 1. */
static hy_med_t *add_v(void)
{
    hy_med_t *m = &meds[n_meds++];
    hy_med_defaults(m);
    m->id = 2;
    strcpy(m->code, "V");
    strcpy(m->name, "Vitamin D");
    m->type = HY_SCHED_FIXED;
    m->times[0] = 8 * 60;
    m->n_times = 1;
    m->order = 1;
    return m;
}

static void add_log(uint32_t med_id, int64_t ts, hy_status_t st, int64_t slot)
{
    hy_log_t *l = &logs[n_logs];
    memset(l, 0, sizeof *l);
    l->id = (uint32_t)(n_logs + 1);
    l->ts = ts;
    strcpy(l->tz, TZ_ZURICH);
    l->med_id = med_id;
    l->status = st;
    l->slot = slot;
    l->delta_min = (int32_t)((ts - slot) / 60);
    l->source = st == HY_MISSED ? HY_SRC_AUTO : HY_SRC_SCREEN;
    n_logs++;
}

static void eval(int64_t now)
{
    hy_sched_in_t in;
    memset(&in, 0, sizeof in);
    in.meds = meds;
    in.n_meds = n_meds;
    in.logs = logs;
    in.n_logs = n_logs;
    in.snoozes = snoozes;
    in.n_snoozes = n_snoozes;
    in.tz = tz;
    in.last_change = change;
    in.now = now;
    hy_sched_eval(&s, &in, &out);
}

static bool in_list(const hy_slot_ref_t *l, size_t n, uint32_t med_id, int64_t slot)
{
    for (size_t i = 0; i < n; i++)
        if (l[i].med_id == med_id && l[i].slot == slot) return true;
    return false;
}

/* Review Focus 2: tracking began after the 08:00 window closed. */
static void test_no_missed_before_tracking(void)
{
    reset();
    hy_med_t *a = add_a();
    hy_sched_start_tracking(&s, L(12, 0));
    eval(L(12, 0));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);
    TEST_ASSERT_FALSE(out.chime);
    TEST_ASSERT_EQUAL_INT64(L(12, 0), hy_sched_tracked_since(&s, a->id));
    hy_gap_t g = { 0, hy_sched_tracked_since(&s, a->id) };
    TEST_ASSERT_EQUAL_INT(HY_OCC_UNTRACKED,
                          hy_occ_state(a, L(8, 0), L(20, 0), L(12, 0), TZ_ZURICH, NULL, 0, &g, 1, NULL));
}

/* Review Focus 2: a clock jump restarts tracking; the skipped window is not missed. */
static void test_clock_jump_untracked(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 30));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
    hy_sched_start_tracking(&s, L(13, 0));
    eval(L(13, 0));
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);
    TEST_ASSERT_FALSE(in_list(out.to_miss, out.n_to_miss, 1, L(8, 0)));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
}

static void test_due_once_and_chime_cap(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 59));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
    TEST_ASSERT_EQUAL_UINT(0, out.n_newly_due);
    TEST_ASSERT_FALSE(out.chime);

    eval(L(8, 0));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_EQUAL_UINT32(1, out.open[0].med_id);
    TEST_ASSERT_EQUAL_INT64(L(8, 0), out.open[0].slot);
    TEST_ASSERT_EQUAL_INT64(L(8, 0), out.open[0].base);
    TEST_ASSERT_EQUAL_INT(HY_OCC_DUE, out.open[0].state);
    TEST_ASSERT_EQUAL_INT64(0, out.open[0].snoozed_until);
    TEST_ASSERT_EQUAL_UINT(1, out.n_showing);
    TEST_ASSERT_EQUAL_UINT(1, out.n_newly_due);
    TEST_ASSERT_EQUAL_UINT32(1, out.newly_due[0].med_id);
    TEST_ASSERT_EQUAL_INT64(L(8, 0), out.newly_due[0].slot);
    TEST_ASSERT_TRUE(out.chime);
    TEST_ASSERT_EQUAL_UINT8(1, s.chime_plays);

    eval(L(8, 0) + 30);
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_EQUAL_UINT(0, out.n_newly_due);
    TEST_ASSERT_FALSE(out.chime);

    for (int m = 2; m <= 8; m += 2) {
        eval(L(8, m));
        TEST_ASSERT_TRUE_MESSAGE(out.chime, "repeat chime");
        TEST_ASSERT_EQUAL_UINT(0, out.n_newly_due);
    }
    TEST_ASSERT_EQUAL_UINT8(5, s.chime_plays);
    eval(L(8, 10));
    TEST_ASSERT_FALSE(out.chime);
    TEST_ASSERT_EQUAL_UINT8(5, s.chime_plays);
}

static void test_late_then_missed(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(8, 30));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_EQUAL_INT(HY_OCC_LATE, out.open[0].state);
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);

    eval(L(10, 0));
    TEST_ASSERT_EQUAL_UINT(1, out.n_to_miss);
    TEST_ASSERT_EQUAL_UINT32(1, out.to_miss[0].med_id);
    TEST_ASSERT_EQUAL_INT64(L(8, 0), out.to_miss[0].slot);
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);

    add_log(1, L(10, 0), HY_MISSED, L(8, 0));
    eval(L(10, 1));
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
}

static void test_taken_closes_and_stops_chime(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(8, 0));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_TRUE(out.chime);
    add_log(1, L(8, 1), HY_TAKEN, L(8, 0));
    eval(L(8, 2));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
    TEST_ASSERT_EQUAL_UINT(0, out.n_showing);
    TEST_ASSERT_FALSE(out.chime);
    TEST_ASSERT_EQUAL_UINT8(0, s.chime_plays);
}

/* Review Focus 4: two doses at once, one snoozed. */
static void test_two_due_and_snooze(void)
{
    reset();
    add_v(); /* array order differs from medicine order on purpose */
    add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(8, 0));
    TEST_ASSERT_EQUAL_UINT(2, out.n_open);
    TEST_ASSERT_EQUAL_UINT32(1, out.open[0].med_id);
    TEST_ASSERT_EQUAL_UINT32(2, out.open[1].med_id);
    TEST_ASSERT_EQUAL_UINT(2, out.n_showing);
    TEST_ASSERT_EQUAL_UINT(2, out.n_newly_due);
    TEST_ASSERT_TRUE(out.chime);

    snoozes[0] = (hy_snooze_t){ 2, L(8, 0), L(8, 15) };
    n_snoozes = 1;
    eval(L(8, 1));
    TEST_ASSERT_EQUAL_UINT(2, out.n_open);
    TEST_ASSERT_EQUAL_UINT(1, out.n_showing);
    TEST_ASSERT_EQUAL_INT64(L(8, 15), out.open[1].snoozed_until);
    TEST_ASSERT_EQUAL_INT64(0, out.open[0].snoozed_until);
    TEST_ASSERT_FALSE(out.chime);

    eval(L(8, 2));
    TEST_ASSERT_TRUE(out.chime); /* A repeat */
    TEST_ASSERT_EQUAL_UINT8(2, s.chime_plays);

    eval(L(8, 15));
    TEST_ASSERT_EQUAL_UINT(2, out.n_showing);
    TEST_ASSERT_EQUAL_INT64(0, out.open[1].snoozed_until);
    TEST_ASSERT_TRUE(out.chime);
    TEST_ASSERT_EQUAL_UINT8(1, s.chime_plays);
    TEST_ASSERT_EQUAL_UINT(0, out.n_newly_due);

    /* The restarted sequence also stops after 5 plays. */
    for (int m = 17; m <= 23; m += 2) {
        eval(L(8, m));
        TEST_ASSERT_TRUE_MESSAGE(out.chime, "restarted sequence repeat");
    }
    TEST_ASSERT_EQUAL_UINT8(5, s.chime_plays);
    eval(L(8, 25));
    TEST_ASSERT_FALSE(out.chime);
}

static void test_new_medicine_mid_day(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 0));
    add_v(); /* V first seen at 12:00 */
    eval(L(12, 0));
    TEST_ASSERT_EQUAL_INT64(L(12, 0), hy_sched_tracked_since(&s, 2));
    for (size_t i = 0; i < out.n_open; i++) TEST_ASSERT_NOT_EQUAL(2, out.open[i].med_id);
    TEST_ASSERT_FALSE(in_list(out.to_miss, out.n_to_miss, 2, L(8, 0)));
    /* A was tracked from 07:00, so its 08:00 slot is missed. */
    TEST_ASSERT_TRUE(in_list(out.to_miss, out.n_to_miss, 1, L(8, 0)));
}

/* Review Focus 5 (R6 setup): Zurich 08:00 slot in the skipped interval, DUE on arrival. */
static void test_east_arrival_due(void)
{
    reset();
    add_a();
    static hy_tzchange_t ch;
    memset(&ch, 0, sizeof ch);
    ch.at = T_2026_07_15_1000Z;
    strcpy(ch.old_tz, TZ_NY);
    strcpy(ch.new_tz, TZ_ZURICH);
    change = &ch;
    hy_sched_start_tracking(&s, T_2026_07_15_1000Z - HOUR);
    eval(T_2026_07_15_1000Z);
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_EQUAL_INT64(T_2026_07_15_0600Z, out.open[0].slot);
    TEST_ASSERT_EQUAL_INT64(T_2026_07_15_1000Z, out.open[0].base);
    TEST_ASSERT_EQUAL_INT(HY_OCC_DUE, out.open[0].state);
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);

    eval(T_2026_07_15_1000Z + 2 * HOUR);
    TEST_ASSERT_TRUE(in_list(out.to_miss, out.n_to_miss, 1, T_2026_07_15_0600Z));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
}

static void test_next_wake(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 0));
    TEST_ASSERT_EQUAL_INT64(L(8, 0), out.next_wake);
    eval(L(8, 0));
    TEST_ASSERT_TRUE(out.chime);
    TEST_ASSERT_EQUAL_INT64(L(8, 2), out.next_wake);
    for (int m = 2; m <= 8; m += 2) {
        eval(L(8, m));
        TEST_ASSERT_TRUE(out.chime);
    }
    TEST_ASSERT_EQUAL_UINT8(5, s.chime_plays);
    TEST_ASSERT_EQUAL_INT64(L(8, 30), out.next_wake); /* late */
    snoozes[0] = (hy_snooze_t){ 1, L(8, 0), L(8, 15) };
    n_snoozes = 1;
    eval(L(8, 9));
    TEST_ASSERT_EQUAL_INT64(L(8, 15), out.next_wake);
}

static void test_dst_gap_slot(void)
{
    reset();
    hy_med_t *m = add_a();
    m->times[0] = 2 * 60 + 30; /* 02:30 does not exist on 2026-03-29 in Zurich */
    m->n_times = 1;
    hy_sched_start_tracking(&s, T_2026_03_29_LOCAL_MIDNIGHT);
    eval(T_2026_03_29_0100Z - MIN);
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
    TEST_ASSERT_EQUAL_INT64(T_2026_03_29_0100Z, out.next_wake);
    eval(T_2026_03_29_0100Z);
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_EQUAL_INT64(T_2026_03_29_0100Z, out.open[0].slot);
    TEST_ASSERT_EQUAL_INT(HY_OCC_DUE, out.open[0].state);
    TEST_ASSERT_EQUAL_UINT(1, out.n_newly_due);
}

static void test_clock_back_no_reannounce(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(8, 1));
    TEST_ASSERT_EQUAL_UINT(1, out.n_newly_due);
    eval(L(7, 59));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
    TEST_ASSERT_EQUAL_UINT(0, out.n_newly_due);
    eval(L(8, 0));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_EQUAL_UINT(0, out.n_newly_due);
}

static void test_not_tracking_outputs_nothing(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 30));
    hy_sched_stop_tracking(&s);
    TEST_ASSERT_EQUAL_INT64(0, hy_sched_tracked_since(&s, 1));
    eval(L(8, 0));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
    TEST_ASSERT_EQUAL_UINT(0, out.n_showing);
    TEST_ASSERT_EQUAL_UINT(0, out.n_newly_due);
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);
    TEST_ASSERT_FALSE(out.chime);
    TEST_ASSERT_EQUAL_INT64(0, out.next_wake);
}

/* Waking exactly at base + late and base + missed sees the new state. */
static void test_wake_at_late_and_missed(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(8, 0));
    for (int m = 2; m <= 8; m += 2) eval(L(8, m));
    TEST_ASSERT_EQUAL_INT64(L(8, 30), out.next_wake);
    eval(out.next_wake);
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_EQUAL_INT(HY_OCC_LATE, out.open[0].state);
    TEST_ASSERT_FALSE(out.chime);
    TEST_ASSERT_EQUAL_INT64(L(10, 0), out.next_wake);
    eval(out.next_wake);
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
    TEST_ASSERT_TRUE(in_list(out.to_miss, out.n_to_miss, 1, L(8, 0)));
    TEST_ASSERT_EQUAL_INT64(L(20, 0), out.next_wake);
}

/* P18: the clock was ahead (14:00), then corrected back to 07:00. */
static void test_backward_jump_keeps_today(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(14, 0));
    eval(L(14, 0));
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 0));
    TEST_ASSERT_EQUAL_INT64(L(7, 0), hy_sched_tracked_since(&s, 1));
    TEST_ASSERT_EQUAL_INT64(L(8, 0), out.next_wake);
    eval(L(8, 0));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_EQUAL_INT(HY_OCC_DUE, out.open[0].state);
    TEST_ASSERT_TRUE(out.chime);
    eval(L(10, 0));
    TEST_ASSERT_TRUE(in_list(out.to_miss, out.n_to_miss, 1, L(8, 0)));
}

/* P19: a late eval plays one repeat and the next one comes 120 s after it. */
static void test_late_eval_cadence(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(8, 0));
    eval(L(8, 5));
    TEST_ASSERT_TRUE(out.chime);
    TEST_ASSERT_EQUAL_UINT8(2, s.chime_plays);
    TEST_ASSERT_EQUAL_INT64(L(8, 7), s.chime_next);
    TEST_ASSERT_EQUAL_INT64(L(8, 7), out.next_wake);
    eval(L(8, 5) + 1);
    TEST_ASSERT_FALSE(out.chime);
    eval(L(8, 7));
    TEST_ASSERT_TRUE(out.chime);
    TEST_ASSERT_EQUAL_UINT8(3, s.chime_plays);

    /* Clock invalid with a dose showing, valid again 20 min later: no burst. */
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(8, 0));
    hy_sched_stop_tracking(&s);
    eval(L(8, 1));
    hy_sched_start_tracking(&s, L(8, 20));
    eval(L(8, 20));
    int plays = out.chime ? 1 : 0;
    for (int i = 1; i < 4; i++) {
        eval(L(8, 20) + i);
        plays += out.chime ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_INT(1, plays);
}

/* P20: archived for 3 days, unarchived at 12:00: nothing missed for that time. */
static void test_unarchive_no_missed(void)
{
    reset();
    hy_med_t *a = add_a();
    hy_sched_start_tracking(&s, L(7, 0) - 3 * DAY);
    eval(L(7, 0) - 3 * DAY);
    a->archived = true;
    eval(L(7, 0) - 2 * DAY);
    a->archived = false;
    eval(L(12, 0));
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);
    TEST_ASSERT_EQUAL_INT64(L(12, 0), hy_sched_tracked_since(&s, 1));

    /* Same when it was as-needed in between. */
    eval(L(13, 0));
    a->type = HY_SCHED_AS_NEEDED;
    eval(L(14, 0));
    a->type = HY_SCHED_FIXED;
    eval(L(23, 0));
    TEST_ASSERT_FALSE(in_list(out.to_miss, out.n_to_miss, 1, L(20, 0)));
}

/* P20: a 09:00 time added at 12:00 does not make today's 09:00 missed. */
static void test_added_time_no_missed(void)
{
    reset();
    hy_med_t *a = add_a();
    a->times[0] = 20 * 60;
    a->n_times = 1;
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 30));
    a->times[0] = 9 * 60;
    a->times[1] = 20 * 60;
    a->n_times = 2;
    eval(L(12, 0));
    TEST_ASSERT_FALSE(in_list(out.to_miss, out.n_to_miss, 1, L(9, 0)));
    TEST_ASSERT_EQUAL_INT64(L(12, 0), hy_sched_tracked_since(&s, 1));

    /* An unchanged schedule keeps its since. */
    eval(L(13, 0));
    TEST_ASSERT_EQUAL_INT64(L(12, 0), hy_sched_tracked_since(&s, 1));
    /* Weekdays and active dates count as schedule changes too. */
    a->weekdays = 0x1F;
    eval(L(14, 0));
    TEST_ASSERT_EQUAL_INT64(L(14, 0), hy_sched_tracked_since(&s, 1));
    a->active_until = 20261231;
    eval(L(15, 0));
    TEST_ASSERT_EQUAL_INT64(L(15, 0), hy_sched_tracked_since(&s, 1));
}

/* Review Focus 1: boot after an off period, the 08:00 slot is still inside its window: due (LATE) with the chime. */
static void test_boot_late_slot_still_due(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(8, 40));
    eval(L(8, 40));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_EQUAL_UINT32(1, out.open[0].med_id);
    TEST_ASSERT_EQUAL_INT64(L(8, 0), out.open[0].slot);
    TEST_ASSERT_EQUAL_INT(HY_OCC_LATE, out.open[0].state);
    TEST_ASSERT_TRUE(in_list(out.newly_due, out.n_newly_due, 1, L(8, 0)));
    TEST_ASSERT_TRUE(out.chime);
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);
}

/* Boot after the 08:00 window closed: not open and never missed (the gap is untracked). */
static void test_boot_after_window_untracked(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(10, 5));
    eval(L(10, 5));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
    TEST_ASSERT_FALSE(in_list(out.to_miss, out.n_to_miss, 1, L(8, 0)));
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);
}

/* Boot inside the window, then the window closes untouched: missed as usual. The runner evaluates at the
 * tracking start (it is the tick that starts tracking), which also records when A was first seen. */
static void test_boot_inside_window_then_missed(void)
{
    reset();
    add_a();
    hy_sched_start_tracking(&s, L(8, 40));
    eval(L(8, 40));
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);
    eval(L(10, 0));
    TEST_ASSERT_EQUAL_UINT(1, out.n_to_miss);
    TEST_ASSERT_EQUAL_UINT32(1, out.to_miss[0].med_id);
    TEST_ASSERT_EQUAL_INT64(L(8, 0), out.to_miss[0].slot);
}

/* 100 stored: 50 archived listed first, then 50 active, all fixed 08:00. The archived medicines
 * take no seen, candidate or to_miss entry, so every active medicine is tracked, due and missed. */
static void test_archived_take_no_active_slot(void)
{
    static hy_med_t big[HY_MAX_STORED];
    reset();
    for (int i = 0; i < HY_MAX_STORED; i++) {
        hy_med_t *m = &big[i];
        memset(m, 0, sizeof *m);
        hy_med_defaults(m);
        m->id = (uint32_t)(i + 1);
        m->code[0] = (char)('A' + i % 26);
        m->code[1] = (char)('0' + i / 26);
        strcpy(m->name, "Filler");
        m->type = HY_SCHED_FIXED;
        m->times[0] = 8 * 60;
        m->n_times = 1;
        m->archived = i < HY_MAX_STORED - HY_MAX_MEDS;
        m->order = (uint16_t)(i == HY_MAX_STORED - 1 ? 0 : i + 1);   /* the last one sorts first */
    }
    hy_sched_in_t in;
    memset(&in, 0, sizeof in);
    in.meds = big;
    in.n_meds = HY_MAX_STORED;
    in.tz = tz;

    hy_sched_start_tracking(&s, L(7, 0));
    in.now = L(8, 5);
    hy_sched_eval(&s, &in, &out);
    TEST_ASSERT_EQUAL_size_t(HY_MAX_MEDS, s.n_seen);
    TEST_ASSERT_EQUAL_UINT(HY_SCHED_MAX_OPEN, out.n_open);
    TEST_ASSERT_EQUAL_UINT32(HY_MAX_STORED, out.open[0].med_id);   /* last stored, first by order */
    for (size_t i = 0; i < out.n_open; i++) TEST_ASSERT_TRUE(out.open[i].med_id > HY_MAX_STORED - HY_MAX_MEDS);

    in.now = L(10, 5);
    hy_sched_eval(&s, &in, &out);
    TEST_ASSERT_EQUAL_UINT(HY_MAX_MEDS, out.n_to_miss);
    TEST_ASSERT_TRUE(in_list(out.to_miss, out.n_to_miss, HY_MAX_STORED, L(8, 0)));
}

/* Review Focus 2: archiving A while its 08:00 dose is open removes the card and the chime, logs nothing. */
static void test_archive_open_slot_disappears(void)
{
    reset();
    hy_med_t *a = add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 0));
    eval(L(8, 5));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_TRUE(out.chime);

    a->archived = true;
    eval(L(8, 6));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
    TEST_ASSERT_EQUAL_UINT(0, out.n_showing);
    TEST_ASSERT_FALSE(out.chime);
    TEST_ASSERT_EQUAL_UINT8(0, s.chime_plays);
    TEST_ASSERT_EQUAL_UINT(0, n_logs);

    eval(L(10, 5));
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);
    TEST_ASSERT_EQUAL_INT64(0, out.next_wake);   /* no A deadline left */
}

/* Archive removes the snooze entries too (hy_act_snoozes gives an empty list afterwards). */
static void test_archive_snoozed(void)
{
    reset();
    hy_med_t *a = add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 0));
    eval(L(8, 5));
    snoozes[0] = (hy_snooze_t){ 1, L(8, 0), L(8, 20) };
    n_snoozes = 1;
    eval(L(8, 6));
    TEST_ASSERT_EQUAL_UINT(0, out.n_showing);

    a->archived = true;
    n_snoozes = 0;
    eval(L(8, 21));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
    TEST_ASSERT_EQUAL_UINT(0, out.n_showing);
    TEST_ASSERT_FALSE(out.chime);
    eval(L(10, 5));
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);
}

/* Review Focus 2: archiving one of two open doses leaves the other with its chime sequence. */
static void test_archive_one_of_two(void)
{
    reset();
    hy_med_t *a = add_a();
    add_v();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 0));
    eval(L(8, 5));
    TEST_ASSERT_EQUAL_UINT(2, out.n_open);
    TEST_ASSERT_TRUE(out.chime);
    TEST_ASSERT_EQUAL_UINT8(1, s.chime_plays);

    a->archived = true;
    eval(L(8, 6));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_EQUAL_UINT32(2, out.open[0].med_id);
    TEST_ASSERT_EQUAL_UINT(1, out.n_showing);
    TEST_ASSERT_EQUAL_UINT(0, out.n_newly_due);
    TEST_ASSERT_FALSE(out.chime);
    TEST_ASSERT_EQUAL_UINT8(1, s.chime_plays);

    eval(L(8, 7));
    TEST_ASSERT_TRUE(out.chime);
    TEST_ASSERT_EQUAL_UINT8(2, s.chime_plays);
}

/* An edit of name, code and colour leaves the schedule signature alone: the open dose stays. */
static void test_edit_name_keeps_open(void)
{
    reset();
    hy_med_t *a = add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 0));
    eval(L(8, 5));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    int64_t slot = out.open[0].slot;

    strcpy(a->name, "Calcium");
    strcpy(a->code, "C");
    a->color = 3;
    eval(L(8, 6));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_EQUAL_INT64(slot, out.open[0].slot);
    TEST_ASSERT_EQUAL_UINT(0, out.n_newly_due);
    TEST_ASSERT_FALSE(out.chime);
}

/* Changed times restart tracking: the open 08:00 dose goes, is never missed, the new 09:00 time is due. */
static void test_edit_times_drops_open(void)
{
    reset();
    hy_med_t *a = add_a();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 0));
    eval(L(8, 5));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);

    a->times[0] = 9 * 60;
    a->n_times = 1;
    eval(L(8, 6));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);

    eval(L(9, 0));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_TRUE(in_list(out.newly_due, out.n_newly_due, 1, L(9, 0)));
    TEST_ASSERT_TRUE(out.chime);

    eval(L(10, 5));
    TEST_ASSERT_FALSE(in_list(out.to_miss, out.n_to_miss, 1, L(8, 0)));
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);
}

/* Q62: a medicine added inside an open dose window is due at once with the chime; after the window nothing. */
static void test_added_time_inside_window_due(void)
{
    reset();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 0));
    hy_med_t *m = &meds[n_meds++];
    hy_med_defaults(m);
    m->id = 3;
    strcpy(m->code, "M");
    strcpy(m->name, "Magnesium");
    m->type = HY_SCHED_FIXED;
    m->times[0] = 8 * 60;
    m->n_times = 1;
    eval(L(8, 10));
    TEST_ASSERT_EQUAL_UINT(1, out.n_open);
    TEST_ASSERT_EQUAL_UINT32(3, out.open[0].med_id);
    TEST_ASSERT_EQUAL_INT64(L(8, 0), out.open[0].slot);
    TEST_ASSERT_EQUAL_INT(HY_OCC_DUE, out.open[0].state);
    TEST_ASSERT_TRUE(in_list(out.newly_due, out.n_newly_due, 3, L(8, 0)));
    TEST_ASSERT_TRUE(out.chime);

    reset();
    hy_sched_start_tracking(&s, L(7, 0));
    eval(L(7, 0));
    m = &meds[n_meds++];
    hy_med_defaults(m);
    m->id = 3;
    strcpy(m->code, "M");
    strcpy(m->name, "Magnesium");
    m->type = HY_SCHED_FIXED;
    m->times[0] = 8 * 60;
    m->n_times = 1;
    eval(L(10, 5));
    TEST_ASSERT_EQUAL_UINT(0, out.n_open);
    TEST_ASSERT_EQUAL_UINT(0, out.n_to_miss);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_missed_before_tracking);
    RUN_TEST(test_clock_jump_untracked);
    RUN_TEST(test_due_once_and_chime_cap);
    RUN_TEST(test_late_then_missed);
    RUN_TEST(test_taken_closes_and_stops_chime);
    RUN_TEST(test_two_due_and_snooze);
    RUN_TEST(test_new_medicine_mid_day);
    RUN_TEST(test_east_arrival_due);
    RUN_TEST(test_next_wake);
    RUN_TEST(test_dst_gap_slot);
    RUN_TEST(test_clock_back_no_reannounce);
    RUN_TEST(test_not_tracking_outputs_nothing);
    RUN_TEST(test_wake_at_late_and_missed);
    RUN_TEST(test_backward_jump_keeps_today);
    RUN_TEST(test_late_eval_cadence);
    RUN_TEST(test_unarchive_no_missed);
    RUN_TEST(test_added_time_no_missed);
    RUN_TEST(test_boot_late_slot_still_due);
    RUN_TEST(test_boot_after_window_untracked);
    RUN_TEST(test_boot_inside_window_then_missed);
    RUN_TEST(test_archived_take_no_active_slot);
    RUN_TEST(test_archive_open_slot_disappears);
    RUN_TEST(test_archive_snoozed);
    RUN_TEST(test_archive_one_of_two);
    RUN_TEST(test_edit_name_keeps_open);
    RUN_TEST(test_edit_times_drops_open);
    RUN_TEST(test_added_time_inside_window_due);
    return UNITY_END();
}
