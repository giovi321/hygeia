#include <string.h>
#include "unity.h"
#include "hy/medform.h"
#include "hy/occ.h"

#define TZ_ZURICH "CET-1CEST,M3.5.0,M10.5.0/3"
/* 2026-11-02 08:10 CET Zurich */
#define T_NOV02_0810 1793603400LL

void setUp(void) {}
void tearDown(void) {}

static hy_med_t s_meds[12];
static size_t s_n;

static hy_med_t *add_med(uint32_t id, const char *code, const char *name, uint8_t color, bool archived)
{
    hy_med_t *m = &s_meds[s_n++];
    hy_med_defaults(m);
    m->id = id;
    strcpy(m->code, code);
    strcpy(m->name, name);
    m->color = color;
    m->archived = archived;
    m->type = HY_SCHED_FIXED;
    m->times[0] = 480;
    m->n_times = 1;
    m->order = (uint16_t)id;
    return m;
}

/* A Antibiotic 1 c0, V Vitamin D 2 c1, M Magnesium 3 c2, Z Zinc 4 archived c3 */
static void fixture(void)
{
    memset(s_meds, 0, sizeof s_meds);
    s_n = 0;
    add_med(1, "A", "Antibiotic", 0, false);
    add_med(2, "V", "Vitamin D", 1, false);
    add_med(3, "M", "Magnesium", 2, false);
    add_med(4, "Z", "Zinc", 3, true);
}

static void test_new_defaults(void)
{
    fixture();
    hy_medform_t f;
    hy_mf_new(&f, s_meds, s_n);
    TEST_ASSERT_EQUAL_INT(HY_SCHED_FIXED, f.draft.type);
    TEST_ASSERT_EQUAL_UINT8(1, f.draft.n_times);
    TEST_ASSERT_EQUAL_UINT16(480, f.draft.times[0]);
    TEST_ASSERT_EQUAL_HEX8(0x7F, f.draft.weekdays);
    TEST_ASSERT_EQUAL_UINT16(60, f.draft.early_window_min);
    TEST_ASSERT_EQUAL_UINT16(30, f.draft.late_after_min);
    TEST_ASSERT_EQUAL_UINT16(120, f.draft.missed_after_min);
    TEST_ASSERT_EQUAL_UINT8(3, f.draft.color);
    TEST_ASSERT_EQUAL_STRING("", f.draft.code);
    TEST_ASSERT_EQUAL_STRING("", f.draft.name);
    TEST_ASSERT_EQUAL_UINT16(0, f.draft.min_interval_min);
    TEST_ASSERT_EQUAL_INT(HY_MF_NAME, f.step);
    TEST_ASSERT_EQUAL_UINT32(0, f.edit_id);
    TEST_ASSERT_FALSE(f.dirty);
    TEST_ASSERT_FALSE(f.from_review);
}

static void test_default_color_all_used(void)
{
    hy_med_t big[20];
    memset(big, 0, sizeof big);
    for (int i = 0; i < 10; i++) { hy_med_defaults(&big[i]); big[i].color = (uint8_t)i; }
    hy_med_defaults(&big[10]); big[10].color = 4;
    TEST_ASSERT_EQUAL_UINT8(0, hy_mf_default_color(big, 11));
    /* each colour twice, colour 7 once */
    for (int i = 0; i < 20; i++) { hy_med_defaults(&big[i]); big[i].color = (uint8_t)(i % 10); }
    big[17].color = 6; /* index 17 was colour 7: colour 7 now once, colour 6 three times */
    TEST_ASSERT_EQUAL_UINT8(7, hy_mf_default_color(big, 20));
}

static void test_set_name(void)
{
    fixture();
    hy_medform_t f;
    hy_mf_new(&f, s_meds, s_n);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_set_name(&f, "  vitamin   c  "));
    TEST_ASSERT_EQUAL_STRING("Vitamin c", f.draft.name);
    TEST_ASSERT_TRUE(f.dirty);
    TEST_ASSERT_EQUAL_INT(HY_E_NAME, hy_mf_set_name(&f, ""));
    TEST_ASSERT_EQUAL_STRING("Vitamin c", f.draft.name);
    TEST_ASSERT_EQUAL_INT(HY_E_NAME, hy_mf_set_name(&f, "   "));
    TEST_ASSERT_EQUAL_STRING("Vitamin c", f.draft.name);
    char b[40];
    memset(b, 'x', 32); b[32] = '\0';
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_set_name(&f, b));
    TEST_ASSERT_EQUAL_UINT(32, strlen(f.draft.name));
    memset(b, 'x', 33); b[33] = '\0';
    TEST_ASSERT_EQUAL_INT(HY_E_NAME, hy_mf_set_name(&f, b));
    TEST_ASSERT_EQUAL_UINT(32, strlen(f.draft.name));
}

static void test_set_dose(void)
{
    fixture();
    hy_medform_t f;
    hy_mf_new(&f, s_meds, s_n);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_set_dose(&f, ""));
    TEST_ASSERT_EQUAL_STRING("", f.draft.dose);
    TEST_ASSERT_FALSE(f.dirty);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_set_dose(&f, "  1   tab "));
    TEST_ASSERT_EQUAL_STRING("1 tab", f.draft.dose);
    TEST_ASSERT_TRUE(f.dirty);
    f.dirty = false;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_set_dose(&f, "1 tab"));
    TEST_ASSERT_FALSE(f.dirty);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_set_dose(&f, "xxxxxxxxxxxxxxxx"));
    TEST_ASSERT_EQUAL_UINT(16, strlen(f.draft.dose));
    TEST_ASSERT_EQUAL_INT(HY_E_DOSE, hy_mf_set_dose(&f, "xxxxxxxxxxxxxxxxx"));
    TEST_ASSERT_EQUAL_STRING("xxxxxxxxxxxxxxxx", f.draft.dose);
}

static void test_code_check(void)
{
    fixture();
    uint32_t dup = 0;
    TEST_ASSERT_EQUAL_INT(HY_E_CODE_DUP, hy_mf_check_code("v", s_meds, s_n, 0, &dup));
    TEST_ASSERT_EQUAL_UINT32(2, dup);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_check_code("Z", s_meds, s_n, 0, &dup));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_check_code("A", s_meds, s_n, 1, NULL));
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_check_code("AB", s_meds, s_n, 0, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_CODE, hy_mf_check_code("A1B", s_meds, s_n, 0, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_CODE, hy_mf_check_code("", s_meds, s_n, 0, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_CODE, hy_mf_check_code("A-", s_meds, s_n, 0, NULL));

    hy_medform_t f;
    hy_mf_new(&f, s_meds, s_n);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_set_code(&f, "q", s_meds, s_n, NULL));
    TEST_ASSERT_EQUAL_STRING("Q", f.draft.code);
    TEST_ASSERT_TRUE(f.dirty);
    TEST_ASSERT_EQUAL_INT(HY_E_CODE_DUP, hy_mf_set_code(&f, "m", s_meds, s_n, &dup));
    TEST_ASSERT_EQUAL_UINT32(3, dup);
    TEST_ASSERT_EQUAL_STRING("Q", f.draft.code);
}

static void test_suggest_code(void)
{
    fixture();
    char out[3];
    hy_mf_suggest_code("Calcium", s_meds, s_n, 0, out);
    TEST_ASSERT_EQUAL_STRING("C", out);
    hy_mf_suggest_code("Vitamin D", s_meds, s_n, 0, out);
    TEST_ASSERT_EQUAL_STRING("VD", out);
    add_med(5, "VD", "Test", 4, false);
    hy_mf_suggest_code("Vitamin D", s_meds, s_n, 0, out);
    TEST_ASSERT_EQUAL_STRING("VI", out);
    hy_mf_suggest_code("Magnesium", s_meds, s_n, 0, out);
    TEST_ASSERT_EQUAL_STRING("MA", out);
    hy_mf_suggest_code("12 hour", s_meds, s_n, 0, out);
    TEST_ASSERT_EQUAL_STRING("1", out);
    hy_mf_suggest_code("", s_meds, s_n, 0, out);
    TEST_ASSERT_EQUAL_STRING("B", out);
    hy_mf_suggest_code("Magnesium", s_meds, s_n, 3, out);
    TEST_ASSERT_EQUAL_STRING("M", out);
}

static void test_times_rules(void)
{
    fixture();
    hy_medform_t f;
    hy_mf_new(&f, s_meds, s_n);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_add_time(&f, 1200));
    TEST_ASSERT_EQUAL_UINT8(2, f.draft.n_times);
    TEST_ASSERT_EQUAL_UINT16(480, f.draft.times[0]);
    TEST_ASSERT_EQUAL_UINT16(1200, f.draft.times[1]);
    TEST_ASSERT_TRUE(f.dirty);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_add_time(&f, 720));
    TEST_ASSERT_EQUAL_UINT16(720, f.draft.times[1]);
    TEST_ASSERT_EQUAL_UINT16(1200, f.draft.times[2]);
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_mf_add_time(&f, 720));
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_mf_add_time(&f, 1440));
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_mf_remove_time(&f, 3));
    TEST_ASSERT_EQUAL_UINT8(3, f.draft.n_times);

    hy_mf_new(&f, s_meds, s_n);
    for (uint16_t i = 1; i <= 7; i++) TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_add_time(&f, (uint16_t)(480 + i * 10)));
    TEST_ASSERT_EQUAL_UINT8(8, f.draft.n_times);
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_mf_add_time(&f, 1000));
    TEST_ASSERT_EQUAL_UINT8(8, f.draft.n_times);

    hy_mf_new(&f, s_meds, s_n);
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_mf_remove_time(&f, 0));
    TEST_ASSERT_EQUAL_UINT8(1, f.draft.n_times);
    hy_mf_add_time(&f, 600);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_remove_time(&f, 0));
    TEST_ASSERT_EQUAL_UINT8(1, f.draft.n_times);
    TEST_ASSERT_EQUAL_UINT16(600, f.draft.times[0]);
}

static void test_next_time_default(void)
{
    fixture();
    hy_medform_t f;
    hy_mf_new(&f, s_meds, s_n);
    TEST_ASSERT_EQUAL_UINT16(720, hy_mf_next_time_default(&f));
    hy_mf_add_time(&f, 720); hy_mf_add_time(&f, 1080); hy_mf_add_time(&f, 1320);
    TEST_ASSERT_EQUAL_UINT16(480, hy_mf_next_time_default(&f));
}

static void test_interval_options(void)
{
    uint16_t o[12];
    static const uint16_t base[10] = { 0, 30, 60, 120, 180, 240, 360, 480, 720, 1440 };
    TEST_ASSERT_EQUAL_UINT(10, hy_mf_interval_options(0, o, 12));
    TEST_ASSERT_EQUAL_UINT16_ARRAY(base, o, 10);
    TEST_ASSERT_EQUAL_UINT(11, hy_mf_interval_options(45, o, 12));
    TEST_ASSERT_EQUAL_UINT16(30, o[1]);
    TEST_ASSERT_EQUAL_UINT16(45, o[2]);
    TEST_ASSERT_EQUAL_UINT16(60, o[3]);
    TEST_ASSERT_EQUAL_UINT(10, hy_mf_interval_options(240, o, 12));
    TEST_ASSERT_EQUAL_UINT16_ARRAY(base, o, 10);
    TEST_ASSERT_EQUAL_UINT(10, hy_mf_interval_options(2000, o, 12));
    TEST_ASSERT_EQUAL_UINT16_ARRAY(base, o, 10);

    fixture();
    hy_medform_t f;
    hy_mf_new(&f, s_meds, s_n);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_set_interval(&f, 1440));
    TEST_ASSERT_TRUE(f.dirty);
    TEST_ASSERT_EQUAL_INT(HY_E_INTERVAL, hy_mf_set_interval(&f, 1441));
    TEST_ASSERT_EQUAL_UINT16(1440, f.draft.min_interval_min);
}

static void test_step_flow(void)
{
    fixture();
    hy_medform_t f;
    hy_mf_new(&f, s_meds, s_n);
    static const hy_mf_step_t fixed[] = { HY_MF_NAME, HY_MF_DOSE, HY_MF_CODE, HY_MF_COLOR, HY_MF_TYPE,
                                          HY_MF_TIMES, HY_MF_REVIEW };
    static const int nums[] = { 1, 2, 3, 4, 5, 6, 0 };
    for (size_t i = 0; i < 7; i++) {
        f.step = fixed[i];
        TEST_ASSERT_EQUAL_INT(nums[i], hy_mf_step_number(&f));
        if (i + 1 < 7) TEST_ASSERT_EQUAL_INT(fixed[i + 1], hy_mf_next(&f));
    }
    f.step = HY_MF_REVIEW;
    TEST_ASSERT_EQUAL_INT(HY_MF_TIMES, hy_mf_prev(&f));
    f.step = HY_MF_NAME;
    TEST_ASSERT_EQUAL_INT(HY_MF_NAME, hy_mf_prev(&f));
    f.step = HY_MF_CODE;
    TEST_ASSERT_EQUAL_INT(HY_MF_DOSE, hy_mf_prev(&f));

    hy_mf_set_type(&f, HY_SCHED_AS_NEEDED);
    f.step = HY_MF_TYPE;
    TEST_ASSERT_EQUAL_INT(HY_MF_INTERVAL, hy_mf_next(&f));
    f.step = HY_MF_INTERVAL;
    TEST_ASSERT_EQUAL_INT(HY_MF_REVIEW, hy_mf_next(&f));
    TEST_ASSERT_EQUAL_INT(6, hy_mf_step_number(&f));
    TEST_ASSERT_EQUAL_INT(HY_MF_TYPE, hy_mf_prev(&f));
    f.step = HY_MF_REVIEW;
    TEST_ASSERT_EQUAL_INT(HY_MF_INTERVAL, hy_mf_prev(&f));

    f.from_review = true;
    f.step = HY_MF_CODE;
    TEST_ASSERT_EQUAL_INT(HY_MF_REVIEW, hy_mf_next(&f));

    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_edit(&f, s_meds, s_n, 2));
    TEST_ASSERT_EQUAL_INT(HY_MF_REVIEW, f.step);
    TEST_ASSERT_EQUAL_INT(HY_MF_NAME, hy_mf_prev(&f));
}

static void test_build_new(void)
{
    fixture();
    s_meds[2].order = 9;
    hy_medform_t f;
    hy_mf_new(&f, s_meds, s_n);
    hy_mf_set_name(&f, "calcium");
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_set_code(&f, "V2", s_meds, s_n, NULL));
    hy_mf_set_color(&f, 5);
    hy_mf_add_time(&f, 1200);
    hy_med_t out;
    hy_mf_step_t bad = HY_MF_NAME;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_build(&f, s_meds, s_n, &out, &bad));
    TEST_ASSERT_EQUAL_UINT32(0, out.id);
    TEST_ASSERT_EQUAL_UINT16(10, out.order);
    TEST_ASSERT_EQUAL_STRING("", out.dose);
    TEST_ASSERT_EQUAL_HEX8(0x7F, out.weekdays);
    TEST_ASSERT_FALSE(out.archived);
    TEST_ASSERT_EQUAL_STRING("Calcium", out.name);
    TEST_ASSERT_EQUAL_STRING("V2", out.code);
    TEST_ASSERT_EQUAL_UINT8(5, out.color);
    TEST_ASSERT_EQUAL_UINT8(2, out.n_times);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_med_validate(&out, s_meds, s_n));
}

static void test_build_edit_keeps_web_fields(void)
{
    fixture();
    hy_med_t *v = &s_meds[1];
    strcpy(v->dose, "1 tab");
    v->weekdays = 0x15;
    v->early_window_min = 45; v->late_after_min = 20; v->missed_after_min = 90;
    v->pills_enabled = true; v->pills_left = 30; v->pills_alert = 5;
    v->active_from = 20261101; v->active_until = 20261231;
    v->order = 7;
    hy_medform_t f;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_edit(&f, s_meds, s_n, 2));
    hy_mf_set_name(&f, "Test");
    hy_mf_set_dose(&f, "2 tab");
    hy_mf_set_code(&f, "K", s_meds, s_n, NULL);
    hy_mf_set_color(&f, 8);
    hy_mf_add_time(&f, 1200);
    hy_med_t out;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_build(&f, s_meds, s_n, &out, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, out.id);
    TEST_ASSERT_EQUAL_STRING("Test", out.name);
    TEST_ASSERT_EQUAL_STRING("K", out.code);
    TEST_ASSERT_EQUAL_UINT8(8, out.color);
    TEST_ASSERT_EQUAL_UINT8(2, out.n_times);
    TEST_ASSERT_EQUAL_STRING("2 tab", out.dose);
    TEST_ASSERT_EQUAL_HEX8(0x15, out.weekdays);
    TEST_ASSERT_EQUAL_UINT16(45, out.early_window_min);
    TEST_ASSERT_EQUAL_UINT16(20, out.late_after_min);
    TEST_ASSERT_EQUAL_UINT16(90, out.missed_after_min);
    TEST_ASSERT_TRUE(out.pills_enabled);
    TEST_ASSERT_EQUAL_INT16(30, out.pills_left);
    TEST_ASSERT_EQUAL_INT16(5, out.pills_alert);
    TEST_ASSERT_EQUAL_INT32(20261101, out.active_from);
    TEST_ASSERT_EQUAL_INT32(20261231, out.active_until);
    TEST_ASSERT_EQUAL_UINT16(7, out.order);
    TEST_ASSERT_FALSE(out.archived);
}

static void test_build_edit_uses_live_record(void)
{
    fixture();
    hy_medform_t f;
    hy_mf_edit(&f, s_meds, s_n, 2);
    hy_mf_set_name(&f, "Calcium");
    s_meds[1].late_after_min = 40;
    s_meds[1].pills_left = 12;
    hy_med_t out;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_build(&f, s_meds, s_n, &out, NULL));
    TEST_ASSERT_EQUAL_UINT16(40, out.late_after_min);
    TEST_ASSERT_EQUAL_INT16(12, out.pills_left);
    TEST_ASSERT_EQUAL_STRING("Calcium", out.name);
}

static void test_build_type_switch(void)
{
    fixture();
    hy_medform_t f;

    /* new form: fixed -> as needed -> fixed keeps the times {480} */
    hy_mf_new(&f, s_meds, s_n);
    hy_mf_set_type(&f, HY_SCHED_AS_NEEDED);
    hy_mf_set_type(&f, HY_SCHED_FIXED);
    TEST_ASSERT_EQUAL_UINT8(1, f.draft.n_times);
    TEST_ASSERT_EQUAL_UINT16(480, f.draft.times[0]);

    hy_mf_edit(&f, s_meds, s_n, 2);
    hy_mf_set_type(&f, HY_SCHED_AS_NEEDED);
    hy_mf_set_interval(&f, 240);
    hy_med_t out;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_build(&f, s_meds, s_n, &out, NULL));
    TEST_ASSERT_EQUAL_INT(HY_SCHED_AS_NEEDED, out.type);
    TEST_ASSERT_EQUAL_UINT8(0, out.n_times);
    for (int i = 0; i < HY_MAX_TIMES; i++) TEST_ASSERT_EQUAL_UINT16(0, out.times[i]);
    TEST_ASSERT_EQUAL_UINT16(240, out.min_interval_min);

    /* as needed live record -> fixed: 480 added when the draft has no times */
    s_meds[1].type = HY_SCHED_AS_NEEDED;
    s_meds[1].n_times = 0;
    memset(s_meds[1].times, 0, sizeof s_meds[1].times);
    s_meds[1].min_interval_min = 90;
    hy_mf_edit(&f, s_meds, s_n, 2);
    hy_mf_set_type(&f, HY_SCHED_FIXED);
    TEST_ASSERT_EQUAL_UINT8(1, f.draft.n_times);
    TEST_ASSERT_EQUAL_UINT16(480, f.draft.times[0]);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_build(&f, s_meds, s_n, &out, NULL));
    TEST_ASSERT_EQUAL_INT(HY_SCHED_FIXED, out.type);
    TEST_ASSERT_EQUAL_UINT8(1, out.n_times);
    TEST_ASSERT_EQUAL_UINT16(90, out.min_interval_min); /* fixed keeps the live value */
}

static void test_build_edit_gone(void)
{
    fixture();
    hy_medform_t f;
    hy_mf_edit(&f, s_meds, s_n, 2);
    hy_med_t out;
    s_meds[1].archived = true;
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_mf_build(&f, s_meds, s_n, &out, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_mf_build(&f, s_meds, 1, &out, NULL));
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_mf_edit(&f, s_meds, s_n, 4));
    TEST_ASSERT_EQUAL_INT(HY_E_NOT_FOUND, hy_mf_edit(&f, s_meds, s_n, 99));
}

static void test_build_errors_map(void)
{
    fixture();
    hy_medform_t f;
    hy_med_t out;
    hy_mf_step_t bad = HY_MF_REVIEW;
    hy_mf_new(&f, s_meds, s_n);
    strcpy(f.draft.name, "Calcium");
    strcpy(f.draft.code, "V");
    TEST_ASSERT_EQUAL_INT(HY_E_CODE_DUP, hy_mf_build(&f, s_meds, s_n, &out, &bad));
    TEST_ASSERT_EQUAL_INT(HY_MF_CODE, bad);
    strcpy(f.draft.code, "C");
    f.draft.name[0] = '\0';
    TEST_ASSERT_EQUAL_INT(HY_E_NAME, hy_mf_build(&f, s_meds, s_n, &out, &bad));
    TEST_ASSERT_EQUAL_INT(HY_MF_NAME, bad);
    strcpy(f.draft.name, "Calcium");
    f.draft.type = HY_SCHED_AS_NEEDED;
    f.draft.min_interval_min = 1441;
    TEST_ASSERT_EQUAL_INT(HY_E_INTERVAL, hy_mf_build(&f, s_meds, s_n, &out, &bad));
    TEST_ASSERT_EQUAL_INT(HY_MF_INTERVAL, bad);
    f.draft.min_interval_min = 0;
    memset(f.draft.dose, 'x', 17);
    TEST_ASSERT_EQUAL_INT(HY_E_DOSE, hy_mf_build(&f, s_meds, s_n, &out, &bad));
    TEST_ASSERT_EQUAL_INT(HY_MF_DOSE, bad);
    TEST_ASSERT_EQUAL_INT(HY_MF_TIMES, hy_mf_step_for_err(HY_E_TIMES));
    TEST_ASSERT_EQUAL_INT(HY_MF_REVIEW, hy_mf_step_for_err(HY_E_FULL));
}

/* Errors on web-only fields of the live record send the user to Review, not to a step that cannot fix them. */
static void test_build_errors_on_web_fields(void)
{
    fixture();
    hy_medform_t f;
    hy_med_t out;
    hy_mf_step_t bad = HY_MF_NAME;

    hy_mf_edit(&f, s_meds, s_n, 2);
    s_meds[1].weekdays = 0;
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_mf_build(&f, s_meds, s_n, &out, &bad));
    TEST_ASSERT_EQUAL_INT(HY_MF_REVIEW, bad);
    s_meds[1].weekdays = 0x7F;

    s_meds[1].active_from = 20261345;   /* month 13 */
    s_meds[1].active_until = 20261201;
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_mf_build(&f, s_meds, s_n, &out, &bad));
    TEST_ASSERT_EQUAL_INT(HY_MF_REVIEW, bad);
    s_meds[1].active_from = 0;
    s_meds[1].active_until = 0;

    s_meds[1].min_interval_min = 2000;   /* fixed keeps the live value */
    TEST_ASSERT_EQUAL_INT(HY_E_INTERVAL, hy_mf_build(&f, s_meds, s_n, &out, &bad));
    TEST_ASSERT_EQUAL_INT(HY_MF_REVIEW, bad);
    s_meds[1].min_interval_min = 0;

    /* errors from the draft keep their step */
    f.draft.color = 12;
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_mf_build(&f, s_meds, s_n, &out, &bad));
    TEST_ASSERT_EQUAL_INT(HY_MF_COLOR, bad);
    f.draft.color = 1;
    f.draft.times[1] = f.draft.times[0];   /* duplicate time */
    f.draft.n_times = 2;
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_mf_build(&f, s_meds, s_n, &out, &bad));
    TEST_ASSERT_EQUAL_INT(HY_MF_TIMES, bad);
    s_meds[1].weekdays = 0;   /* bad live weekdays too: the draft times take precedence */
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_mf_build(&f, s_meds, s_n, &out, &bad));
    TEST_ASSERT_EQUAL_INT(HY_MF_TIMES, bad);
    s_meds[1].weekdays = 0x7F;

    /* as needed: the interval error stays on the interval step */
    f.draft.type = HY_SCHED_AS_NEEDED;
    f.draft.min_interval_min = 1441;
    TEST_ASSERT_EQUAL_INT(HY_E_INTERVAL, hy_mf_build(&f, s_meds, s_n, &out, &bad));
    TEST_ASSERT_EQUAL_INT(HY_MF_INTERVAL, bad);
}

static void test_due_on_save(void)
{
    fixture();
    hy_medform_t f;
    hy_med_t built;
    int64_t out[8];
    hy_mf_new(&f, s_meds, s_n);
    hy_mf_set_name(&f, "Calcium");
    hy_mf_set_code(&f, "C", s_meds, s_n, NULL);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_build(&f, s_meds, s_n, &built, NULL));
    TEST_ASSERT_EQUAL_UINT(1, hy_mf_due_on_save(&built, NULL, T_NOV02_0810, TZ_ZURICH, out, 8));
    TEST_ASSERT_EQUAL_INT64(T_NOV02_0810 - 10 * 60, out[0]);
    TEST_ASSERT_EQUAL_UINT(0, hy_mf_due_on_save(&built, NULL, T_NOV02_0810 + 115 * 60, TZ_ZURICH, out, 8));

    /* edit adding 10:00 to {08:00} at 10:05 */
    int64_t t1005 = T_NOV02_0810 + 115 * 60;
    hy_mf_edit(&f, s_meds, s_n, 2);
    hy_mf_add_time(&f, 600);
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_build(&f, s_meds, s_n, &built, NULL));
    TEST_ASSERT_EQUAL_UINT(1, hy_mf_due_on_save(&built, &s_meds[1], t1005, TZ_ZURICH, out, 8));
    TEST_ASSERT_EQUAL_INT64(t1005 - 5 * 60, out[0]);

    /* edit keeping {08:00} at 08:10 */
    hy_mf_edit(&f, s_meds, s_n, 2);
    hy_mf_set_name(&f, "Calcium");
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_mf_build(&f, s_meds, s_n, &built, NULL));
    TEST_ASSERT_EQUAL_UINT(0, hy_mf_due_on_save(&built, &s_meds[1], T_NOV02_0810, TZ_ZURICH, out, 8));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_new_defaults);
    RUN_TEST(test_default_color_all_used);
    RUN_TEST(test_set_name);
    RUN_TEST(test_set_dose);
    RUN_TEST(test_code_check);
    RUN_TEST(test_suggest_code);
    RUN_TEST(test_times_rules);
    RUN_TEST(test_next_time_default);
    RUN_TEST(test_interval_options);
    RUN_TEST(test_step_flow);
    RUN_TEST(test_build_new);
    RUN_TEST(test_build_edit_keeps_web_fields);
    RUN_TEST(test_build_edit_uses_live_record);
    RUN_TEST(test_build_type_switch);
    RUN_TEST(test_build_edit_gone);
    RUN_TEST(test_build_errors_map);
    RUN_TEST(test_build_errors_on_web_fields);
    RUN_TEST(test_due_on_save);
    return UNITY_END();
}
