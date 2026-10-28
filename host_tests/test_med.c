#include <string.h>
#include "unity.h"
#include "hy/med.h"

void setUp(void) {}
void tearDown(void) {}

/* A valid fixed-schedule medicine built from the defaults. */
static void make_valid(hy_med_t *m, uint32_t id, const char *code)
{
    hy_med_defaults(m);
    m->id = id;
    strcpy(m->code, code);
    strcpy(m->name, "Antibiotic");
    m->type = HY_SCHED_FIXED;
    m->times[0] = 8 * 60;
    m->n_times = 1;
}

static void test_defaults_match_spec(void)
{
    hy_med_t m;
    hy_med_defaults(&m);
    TEST_ASSERT_EQUAL_HEX8(0x7F, m.weekdays);
    TEST_ASSERT_EQUAL_UINT16(60, m.early_window_min);
    TEST_ASSERT_EQUAL_UINT16(30, m.late_after_min);
    TEST_ASSERT_EQUAL_UINT16(120, m.missed_after_min);
    TEST_ASSERT_EQUAL_INT(HY_SCHED_FIXED, m.type);
    TEST_ASSERT_EQUAL_UINT8(0, m.n_times);
}

static void test_code_rules(void)
{
    hy_med_t m;
    const char *ok[] = { "A", "V2", "7" };
    const char *bad[] = { "", "ABC", "a", "+", "\xC3\x84" /* UTF-8 A-umlaut */ };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        make_valid(&m, 1, "A");
        memset(m.code, 0, sizeof m.code);
        strncpy(m.code, ok[i], sizeof m.code - 1);
        TEST_ASSERT_EQUAL_INT_MESSAGE(HY_OK, hy_med_validate(&m, NULL, 0), ok[i]);
    }
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        make_valid(&m, 1, "A");
        memset(m.code, 0, sizeof m.code);
        memcpy(m.code, bad[i], strlen(bad[i]) < 3 ? strlen(bad[i]) : 3);
        TEST_ASSERT_EQUAL_INT_MESSAGE(HY_E_CODE, hy_med_validate(&m, NULL, 0), bad[i]);
    }
    /* three non-NUL chars, no terminator inside the array's logical string */
    make_valid(&m, 1, "A");
    m.code[0] = 'A'; m.code[1] = 'B'; m.code[2] = 'C';
    TEST_ASSERT_EQUAL_INT(HY_E_CODE, hy_med_validate(&m, NULL, 0));
}

static void test_code_unique_among_active(void)
{
    hy_med_t m, o[2];
    make_valid(&m, 1, "A");

    make_valid(&o[0], 2, "A");
    TEST_ASSERT_EQUAL_INT(HY_E_CODE_DUP, hy_med_validate(&m, o, 1));

    o[0].archived = true;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_med_validate(&m, o, 1));

    /* editing itself: same id in others is skipped */
    make_valid(&o[0], 1, "A");
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_med_validate(&m, o, 1));

    /* self plus a distinct code */
    make_valid(&o[1], 3, "V");
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_med_validate(&m, o, 2));

    /* archived medicine itself needs no uniqueness check */
    make_valid(&o[1], 3, "A");
    m.archived = true;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_med_validate(&m, o, 2));
}

static void test_name_rules(void)
{
    hy_med_t m;
    make_valid(&m, 1, "A");
    m.name[0] = '\0';
    TEST_ASSERT_EQUAL_INT(HY_E_NAME, hy_med_validate(&m, NULL, 0));

    make_valid(&m, 1, "A");
    memset(m.name, 'x', 32);
    m.name[32] = '\0';
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_med_validate(&m, NULL, 0));
}

static void test_times_rules(void)
{
    hy_med_t m;

    make_valid(&m, 1, "A");
    m.n_times = 0;
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_med_validate(&m, NULL, 0));

    make_valid(&m, 1, "A");
    m.times[0] = 1440;
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_med_validate(&m, NULL, 0));

    make_valid(&m, 1, "A");
    m.times[0] = 1439;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_med_validate(&m, NULL, 0));

    make_valid(&m, 1, "A");
    m.times[0] = 480; m.times[1] = 480; m.n_times = 2;
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_med_validate(&m, NULL, 0));

    make_valid(&m, 1, "A");
    m.n_times = HY_MAX_TIMES + 1;
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_med_validate(&m, NULL, 0));

    make_valid(&m, 1, "A");
    m.weekdays = 0;
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_med_validate(&m, NULL, 0));

    make_valid(&m, 1, "A");
    m.weekdays = 0x80; /* only bits outside Monday..Sunday */
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_med_validate(&m, NULL, 0));

    make_valid(&m, 1, "A");
    m.type = HY_SCHED_AS_NEEDED;
    m.n_times = 0;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_med_validate(&m, NULL, 0));
}

static void test_color_range(void)
{
    hy_med_t m;
    make_valid(&m, 1, "A");
    m.color = 9;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_med_validate(&m, NULL, 0));
    m.color = 10;
    TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_med_validate(&m, NULL, 0));
}

static void test_weekdays_and_dates(void)
{
    hy_med_t m;
    make_valid(&m, 1, "A");
    m.weekdays = 0xFF;
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_med_validate(&m, NULL, 0));
    make_valid(&m, 1, "A");
    m.type = HY_SCHED_AS_NEEDED; m.n_times = 0; m.weekdays = 0x80 | 0x7F;
    TEST_ASSERT_EQUAL_INT(HY_E_TIMES, hy_med_validate(&m, NULL, 0));

    make_valid(&m, 1, "A");
    m.active_from = 20261001; m.active_until = 20261031;
    TEST_ASSERT_EQUAL_INT(HY_OK, hy_med_validate(&m, NULL, 0));
    int32_t bad[] = { -1, 5, 19991231, 20260001, 20261301, 20260100, 20260132, 100000101 };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        make_valid(&m, 1, "A");
        m.active_from = bad[i];
        TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_med_validate(&m, NULL, 0));
        make_valid(&m, 1, "A");
        m.active_until = bad[i];
        TEST_ASSERT_EQUAL_INT(HY_E_INVALID, hy_med_validate(&m, NULL, 0));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_match_spec);
    RUN_TEST(test_code_rules);
    RUN_TEST(test_code_unique_among_active);
    RUN_TEST(test_name_rules);
    RUN_TEST(test_times_rules);
    RUN_TEST(test_color_range);
    RUN_TEST(test_weekdays_and_dates);
    return UNITY_END();
}
