#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "hy/codec.h"
#include "hy/med.h"

void setUp(void) {}
void tearDown(void) {}

static hy_log_t sample_log(void)
{
    hy_log_t e;
    memset(&e, 0, sizeof e);
    e.id = 42; e.ts = 1793181600LL;
    strcpy(e.tz, "CET-1CEST,M3.5.0,M10.5.0/3");
    e.med_id = 7; e.status = HY_SKIPPED; e.slot = 1793181000LL; e.delta_min = -10;
    strcpy(e.note, "\xC3\x9C" "belkeit \"leicht\"");
    e.source = HY_SRC_MQTT; e.edited = true;
    return e;
}

static void assert_log_eq(const hy_log_t *a, const hy_log_t *b)
{
    TEST_ASSERT_EQUAL_UINT32(a->id, b->id);
    TEST_ASSERT_EQUAL_INT64(a->ts, b->ts);
    TEST_ASSERT_EQUAL_STRING(a->tz, b->tz);
    TEST_ASSERT_EQUAL_UINT32(a->med_id, b->med_id);
    TEST_ASSERT_EQUAL_INT(a->status, b->status);
    TEST_ASSERT_EQUAL_INT64(a->slot, b->slot);
    TEST_ASSERT_EQUAL_INT32(a->delta_min, b->delta_min);
    TEST_ASSERT_EQUAL_STRING(a->note, b->note);
    TEST_ASSERT_EQUAL_INT(a->source, b->source);
    TEST_ASSERT_EQUAL(a->edited, b->edited);
}

void test_log_roundtrip(void)
{
    hy_log_t e = sample_log(), d;
    char buf[1024];
    int n = hy_log_to_jsonl(&e, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL((int)strlen(buf), n);
    TEST_ASSERT_NULL(strchr(buf, '\n'));
    TEST_ASSERT_EQUAL(HY_OK, hy_log_from_jsonl(buf, &d));
    assert_log_eq(&e, &d);

    for (int st = HY_TAKEN; st <= HY_MISSED; st++)
        for (int src = HY_SRC_SCREEN; src <= HY_SRC_AUTO; src++) {
            e.status = (hy_status_t)st; e.source = (hy_source_t)src;
            n = hy_log_to_jsonl(&e, buf, sizeof buf);
            TEST_ASSERT_GREATER_THAN(0, n);
            TEST_ASSERT_EQUAL(HY_OK, hy_log_from_jsonl(buf, &d));
            assert_log_eq(&e, &d);
        }
}

void test_log_optional_fields_omitted(void)
{
    hy_log_t e = sample_log(), d;
    char buf[1024];
    e.slot = 0; e.note[0] = 0;
    TEST_ASSERT_GREATER_THAN(0, hy_log_to_jsonl(&e, buf, sizeof buf));
    TEST_ASSERT_NULL(strstr(buf, "\"slot\""));
    TEST_ASSERT_NULL(strstr(buf, "\"note\""));
    TEST_ASSERT_EQUAL(HY_OK, hy_log_from_jsonl(buf, &d));
    assert_log_eq(&e, &d);
    /* explicit null slot and missing optional fields */
    TEST_ASSERT_EQUAL(HY_OK, hy_log_from_jsonl(
        "{\"id\":1,\"ts\":100,\"med_id\":2,\"status\":\"taken\",\"slot\":null}", &d));
    TEST_ASSERT_EQUAL_INT64(0, d.slot);
    TEST_ASSERT_EQUAL_STRING("", d.tz);
    TEST_ASSERT_EQUAL_INT(HY_SRC_SCREEN, d.source);
    TEST_ASSERT_FALSE(d.edited);
}

void test_log_newline_in_note_escaped(void)
{
    hy_log_t e = sample_log(), d;
    char buf[1024];
    strcpy(e.note, "line1\nline2\r\n\ttab");
    TEST_ASSERT_GREATER_THAN(0, hy_log_to_jsonl(&e, buf, sizeof buf));
    TEST_ASSERT_NULL(strchr(buf, '\n'));
    TEST_ASSERT_NULL(strchr(buf, '\r'));
    TEST_ASSERT_EQUAL(HY_OK, hy_log_from_jsonl(buf, &d));
    TEST_ASSERT_EQUAL_STRING(e.note, d.note);
}

void test_log_buffer_too_small(void)
{
    hy_log_t e = sample_log();
    char buf[1024];
    int n = hy_log_to_jsonl(&e, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN(0, n);
    char small[1024];
    TEST_ASSERT_EQUAL(-1, hy_log_to_jsonl(&e, small, (size_t)n));      /* no room for NUL */
    TEST_ASSERT_EQUAL(-1, hy_log_to_jsonl(&e, small, 5));
    TEST_ASSERT_EQUAL(n, hy_log_to_jsonl(&e, small, (size_t)n + 1));
}

void test_unknown_fields_ignored(void)
{
    hy_log_t d;
    TEST_ASSERT_EQUAL(HY_OK, hy_log_from_jsonl(
        "{\"id\":3,\"ts\":500,\"future\":{\"a\":[1,2]},\"med_id\":9,\"status\":\"missed\","
        "\"source\":\"auto\",\"extra\":true}", &d));
    TEST_ASSERT_EQUAL_UINT32(3, d.id);
    TEST_ASSERT_EQUAL_INT(HY_MISSED, d.status);
    TEST_ASSERT_EQUAL_INT(HY_SRC_AUTO, d.source);
}

void test_log_invalid(void)
{
    hy_log_t d;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_log_from_jsonl("", &d));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_log_from_jsonl("{\"id\":1,\"ts\":5", &d));  /* truncated */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_log_from_jsonl("[1,2]", &d));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_log_from_jsonl("{\"ts\":5,\"med_id\":1,\"status\":\"taken\"}", &d));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_log_from_jsonl(
        "{\"id\":\"1\",\"ts\":5,\"med_id\":1,\"status\":\"taken\"}", &d));         /* wrong type */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_log_from_jsonl(
        "{\"id\":1,\"ts\":5,\"med_id\":1,\"status\":\"eaten\"}", &d));             /* enum */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_log_from_jsonl(
        "{\"id\":1,\"ts\":5,\"med_id\":1,\"status\":\"taken\",\"source\":\"x\"}", &d));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_log_from_jsonl(
        "{\"id\":-1,\"ts\":5,\"med_id\":1,\"status\":\"taken\"}", &d));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_log_from_jsonl(
        "{\"id\":1,\"ts\":5,\"med_id\":1,\"status\":\"taken\",\"edited\":1}", &d));
    /* note of 64 bytes does not fit char[64] */
    char line[300];
    char note[65];
    memset(note, 'n', 64); note[64] = 0;
    snprintf(line, sizeof line,
             "{\"id\":1,\"ts\":5,\"med_id\":1,\"status\":\"taken\",\"note\":\"%s\"}", note);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_log_from_jsonl(line, &d));
    note[63] = 0;
    snprintf(line, sizeof line,
             "{\"id\":1,\"ts\":5,\"med_id\":1,\"status\":\"taken\",\"note\":\"%s\"}", note);
    TEST_ASSERT_EQUAL(HY_OK, hy_log_from_jsonl(line, &d));
    /* tz of 48 bytes does not fit */
    char tz[49];
    memset(tz, 'z', 48); tz[48] = 0;
    snprintf(line, sizeof line,
             "{\"id\":1,\"ts\":5,\"med_id\":1,\"status\":\"taken\",\"tz\":\"%s\"}", tz);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_log_from_jsonl(line, &d));
}

static void make_meds(hy_med_t m[3])
{
    for (int i = 0; i < 3; i++) hy_med_defaults(&m[i]);
    m[0].id = 1; strcpy(m[0].code, "A"); strcpy(m[0].name, "Antibiotic"); strcpy(m[0].dose, "1 tab");
    m[0].color = 3; m[0].n_times = 3; m[0].times[0] = 8 * 60; m[0].times[1] = 14 * 60 + 5;
    m[0].times[2] = 23 * 60 + 59; m[0].weekdays = 0x55; m[0].order = 2;
    m[0].active_from = 20261001; m[0].active_until = 20261031;
    m[0].pills_enabled = true; m[0].pills_left = 12; m[0].pills_alert = 3;
    m[1].id = 2; strcpy(m[1].code, "V"); strcpy(m[1].name, "Vitamin D"); strcpy(m[1].dose, "5 ml");
    m[1].color = 9; m[1].type = HY_SCHED_AS_NEEDED; m[1].min_interval_min = 240;
    m[1].early_window_min = 10; m[1].late_after_min = 20; m[1].missed_after_min = 300;
    m[1].archived = true; m[1].order = 1;
    m[2].id = 3; strcpy(m[2].code, "M"); strcpy(m[2].name, "Magnesium"); m[2].n_times = 1;
    m[2].times[0] = 0;
}

static void assert_med_eq(const hy_med_t *a, const hy_med_t *b)
{
    TEST_ASSERT_EQUAL_UINT32(a->id, b->id);
    TEST_ASSERT_EQUAL_STRING(a->code, b->code);
    TEST_ASSERT_EQUAL_STRING(a->name, b->name);
    TEST_ASSERT_EQUAL_STRING(a->dose, b->dose);
    TEST_ASSERT_EQUAL_UINT8(a->color, b->color);
    TEST_ASSERT_EQUAL_INT(a->type, b->type);
    TEST_ASSERT_EQUAL_UINT8(a->n_times, b->n_times);
    for (int i = 0; i < a->n_times; i++) TEST_ASSERT_EQUAL_UINT16(a->times[i], b->times[i]);
    TEST_ASSERT_EQUAL_UINT8(a->weekdays, b->weekdays);
    TEST_ASSERT_EQUAL_UINT16(a->min_interval_min, b->min_interval_min);
    TEST_ASSERT_EQUAL_UINT16(a->early_window_min, b->early_window_min);
    TEST_ASSERT_EQUAL_UINT16(a->late_after_min, b->late_after_min);
    TEST_ASSERT_EQUAL_UINT16(a->missed_after_min, b->missed_after_min);
    TEST_ASSERT_EQUAL(a->pills_enabled, b->pills_enabled);
    TEST_ASSERT_EQUAL_INT16(a->pills_left, b->pills_left);
    TEST_ASSERT_EQUAL_INT16(a->pills_alert, b->pills_alert);
    TEST_ASSERT_EQUAL_INT32(a->active_from, b->active_from);
    TEST_ASSERT_EQUAL_INT32(a->active_until, b->active_until);
    TEST_ASSERT_EQUAL(a->archived, b->archived);
    TEST_ASSERT_EQUAL_UINT16(a->order, b->order);
}

void test_meds_roundtrip(void)
{
    hy_med_t m[3], d[8];
    size_t n = 99;
    make_meds(m);
    char *js = hy_meds_to_json(m, 3);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"v\":1"));
    TEST_ASSERT_NULL(strchr(js, '\n'));
    TEST_ASSERT_EQUAL(HY_OK, hy_meds_from_json(js, d, 8, &n));
    TEST_ASSERT_EQUAL(3, n);
    for (int i = 0; i < 3; i++) assert_med_eq(&m[i], &d[i]);
    free(js);

    js = hy_meds_to_json(NULL, 0);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_EQUAL(HY_OK, hy_meds_from_json(js, d, 8, &n));
    TEST_ASSERT_EQUAL(0, n);
    free(js);
}

void test_meds_defaults_and_unknown_fields(void)
{
    hy_med_t d[2];
    size_t n;
    TEST_ASSERT_EQUAL(HY_OK, hy_meds_from_json(
        "{\"v\":1,\"x\":1,\"meds\":[{\"id\":5,\"code\":\"Q\",\"name\":\"Magnesium\",\"zzz\":[1]}]}",
        d, 2, &n));
    TEST_ASSERT_EQUAL(1, n);
    hy_med_t ref;
    hy_med_defaults(&ref);
    TEST_ASSERT_EQUAL_UINT8(0x7F, d[0].weekdays);
    TEST_ASSERT_EQUAL_UINT16(ref.early_window_min, d[0].early_window_min);
    TEST_ASSERT_EQUAL_UINT16(ref.late_after_min, d[0].late_after_min);
    TEST_ASSERT_EQUAL_UINT16(ref.missed_after_min, d[0].missed_after_min);
    TEST_ASSERT_EQUAL_INT(HY_SCHED_FIXED, d[0].type);
    TEST_ASSERT_EQUAL_UINT8(0, d[0].n_times);
    TEST_ASSERT_EQUAL_STRING("", d[0].dose);
}

void test_meds_invalid(void)
{
    hy_med_t d[2];
    size_t n = 77;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json("not json", d, 2, &n));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json("{\"meds\":[]}", d, 2, &n));          /* no v */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json("{\"v\":2,\"meds\":[]}", d, 2, &n));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json("{\"v\":1}", d, 2, &n));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json("{\"v\":1,\"meds\":{}}", d, 2, &n));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json("{\"v\":1,\"meds\":[5]}", d, 2, &n));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json(
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"ABC\",\"name\":\"x\"}]}", d, 2, &n));      /* code */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json(
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"123456789012345678901234567890123\"}]}",
        d, 2, &n));                                                                          /* 33 chars */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json(
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"x\",\"dose\":\"12345678901234567\"}]}",
        d, 2, &n));                                                                          /* 17 chars */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json(
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"x\",\"schedule\":{\"type\":\"daily\"}}]}",
        d, 2, &n));                                                                          /* enum */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json(
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"x\",\"schedule\":{\"times\":[\"24:00\"]}}]}",
        d, 2, &n));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json(
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"x\",\"schedule\":{\"times\":[\"12:60\"]}}]}",
        d, 2, &n));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json(
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"x\",\"schedule\":{\"times\":[\"8:00\"]}}]}",
        d, 2, &n));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json(
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"x\",\"schedule\":{\"times\":[800]}}]}",
        d, 2, &n));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json(
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"x\",\"schedule\":{\"times\":"
        "[\"01:00\",\"02:00\",\"03:00\",\"04:00\",\"05:00\",\"06:00\",\"07:00\",\"08:00\",\"09:00\"]}}]}",
        d, 2, &n));                                                                          /* 9 times */
    TEST_ASSERT_EQUAL(HY_OK, hy_meds_from_json(
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"x\",\"schedule\":{\"times\":"
        "[\"01:00\",\"02:00\",\"03:00\",\"04:00\",\"05:00\",\"06:00\",\"07:00\",\"23:59\"]}}]}",
        d, 2, &n));                                                                          /* 8 times */
    TEST_ASSERT_EQUAL(8, d[0].n_times);
    TEST_ASSERT_EQUAL_UINT16(23 * 60 + 59, d[0].times[7]);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json(
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"x\",\"archived\":\"no\"}]}", d, 2, &n));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_meds_from_json(
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"x\",\"color\":10}]}", d, 2, &n));
}

void test_meds_full(void)
{
    hy_med_t d[2];
    size_t n = 0;
    const char *three =
        "{\"v\":1,\"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"a\"},"
        "{\"id\":2,\"code\":\"B\",\"name\":\"b\"},{\"id\":3,\"code\":\"C\",\"name\":\"c\"}]}";
    TEST_ASSERT_EQUAL(HY_E_FULL, hy_meds_from_json(three, d, 2, &n));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_log_roundtrip);
    RUN_TEST(test_log_optional_fields_omitted);
    RUN_TEST(test_log_newline_in_note_escaped);
    RUN_TEST(test_log_buffer_too_small);
    RUN_TEST(test_unknown_fields_ignored);
    RUN_TEST(test_log_invalid);
    RUN_TEST(test_meds_roundtrip);
    RUN_TEST(test_meds_defaults_and_unknown_fields);
    RUN_TEST(test_meds_invalid);
    RUN_TEST(test_meds_full);
    return UNITY_END();
}
