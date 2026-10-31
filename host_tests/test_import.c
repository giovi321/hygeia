#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "hy/api.h"
#include "hy/codec.h"
#include "hy/import.h"
#include "hy/settings.h"

#define D0 1793145600LL /* 2026-10-28 00:00 UTC */
#define DAY 86400LL

static void med(hy_med_t *m, uint32_t id, const char *code, const char *name, uint16_t order)
{
    memset(m, 0, sizeof *m);
    hy_med_defaults(m);
    m->id = id; m->order = order;
    strcpy(m->code, code); strcpy(m->name, name); strcpy(m->dose, "1 tablet");
    m->type = HY_SCHED_FIXED; m->times[0] = 8 * 60; m->n_times = 1;
}

static hy_log_t entry(uint32_t id, uint32_t med_id, int64_t ts, hy_status_t st)
{
    hy_log_t e;
    memset(&e, 0, sizeof e);
    e.id = id; e.med_id = med_id; e.ts = ts; e.status = st; strcpy(e.tz, "UTC0");
    e.source = HY_SRC_SCREEN;
    return e;
}

/* ---- a growing text buffer for building files ---- */
static char *buf; static size_t blen, bcap;
static void put(const char *s)
{
    size_t n = strlen(s);
    if (blen + n + 1 > bcap) {
        bcap = (blen + n + 1) * 2;
        buf = realloc(buf, bcap);
        TEST_ASSERT_NOT_NULL(buf);
    }
    memcpy(buf + blen, s, n + 1);
    blen += n;
}

/* An export as the device writes it: head, entries, tail. */
static const char *build(const hy_med_t *m, size_t n, const hy_log_t *logs, size_t nl, const hy_gap_t *gaps,
                         const uint8_t *reasons, size_t ng, const hy_settings_t *s, const hy_backup_wifi_t *w)
{
    blen = 0;
    hy_export_head_t h = { D0, m, n, 0, gaps, reasons, ng, s, w };
    uint32_t mx = 0;
    for (size_t i = 0; i < n; i++) if (m[i].id > mx) mx = m[i].id;
    h.next_id = mx + 1;
    char *head = hy_export_head_json(&h);
    TEST_ASSERT_NOT_NULL(head);
    put(head);
    free(head);
    char line[HY_IMPORT_ENTRY_MAX];
    for (size_t i = 0; i < nl; i++) {
        TEST_ASSERT_TRUE(hy_log_to_jsonl(&logs[i], line, sizeof line) > 0);
        if (i) put(",");
        put(line);
    }
    put(HY_EXPORT_TAIL);
    return buf;
}

/* ---- device log index for log_exists ---- */
typedef struct { uint32_t med; int64_t ts; hy_status_t st; } dkey_t;
static dkey_t dev[16]; static size_t n_dev;
static uint32_t asked_meds[64]; static size_t n_asked;
static bool exists_cb(void *ctx, uint32_t m, int64_t ts, hy_status_t st)
{
    (void)ctx;
    if (n_asked < 64) asked_meds[n_asked++] = m;
    for (size_t i = 0; i < n_dev; i++)
        if (dev[i].med == m && dev[i].ts == ts && dev[i].st == st) return true;
    return false;
}

static hy_med_t cur[4];
static hy_import_t imp;

void setUp(void)
{
    med(&cur[0], 1, "A", "Antibiotic", 0);
    med(&cur[1], 2, "V", "Vitamin D", 1);
    med(&cur[2], 3, "M", "Magnesium", 2);
    n_dev = 0; n_asked = 0;
    memset(&imp, 0, sizeof imp);
}
void tearDown(void) { hy_import_free(&imp); }

static void test_preview_merge_counts(void)
{
    hy_med_t f[3];
    f[0] = cur[0]; strcpy(f[0].name, "Antibiotic forte");   /* A changed */
    f[1] = cur[1];                                           /* V the same */
    med(&f[2], 7, "Z", "Zinc", 3);                           /* new id */
    hy_log_t l[5] = {
        entry(10, 1, D0 + 100, HY_TAKEN), entry(11, 2, D0 + 200, HY_TAKEN), entry(12, 1, D0 + 300, HY_SKIPPED),
        entry(13, 1, D0 + 400, HY_TAKEN), entry(14, 7, D0 + 500, HY_TAKEN),
    };
    dev[0] = (dkey_t){ 1, D0 + 100, HY_TAKEN };
    dev[1] = (dkey_t){ 2, D0 + 200, HY_TAKEN };
    dev[2] = (dkey_t){ 1, D0 + 300, HY_SKIPPED };
    n_dev = 3;
    const char *js = build(f, 3, l, 5, NULL, NULL, 0, NULL, NULL);
    hy_import_diff_t d; hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_OK, hy_import_plan(js, HY_IMPORT_MERGE, cur, 2, exists_cb, NULL, &d, &e));
    TEST_ASSERT_EQUAL_size_t(1, d.meds_change);
    TEST_ASSERT_EQUAL_size_t(1, d.meds_add);
    TEST_ASSERT_EQUAL_size_t(1, d.meds_same);
    TEST_ASSERT_EQUAL_size_t(2, d.log_add);
    TEST_ASSERT_EQUAL_size_t(3, d.log_dup);
    TEST_ASSERT_EQUAL_size_t(0, d.log_skip);
    TEST_ASSERT_FALSE(d.settings);
    TEST_ASSERT_FALSE(d.secrets);
    for (size_t i = 0; i < n_asked; i++) TEST_ASSERT_NOT_EQUAL(7, asked_meds[i]);   /* a new medicine has no log yet */
}

static void test_merge_id_collision_remap(void)
{
    hy_med_t f[2];
    f[0] = cur[0];
    med(&f[1], 2, "C", "Calcium", 1);   /* id 2 is V on the device */
    hy_log_t l[3] = { entry(20, 2, D0 + 100, HY_TAKEN), entry(21, 2, D0 + 200, HY_TAKEN),
                      entry(22, 1, D0 + 300, HY_TAKEN) };
    dev[0] = (dkey_t){ 2, D0 + 100, HY_TAKEN };   /* V's dose at the same second: not Calcium's */
    dev[1] = (dkey_t){ 1, D0 + 300, HY_TAKEN };
    n_dev = 2;
    const char *js = build(f, 2, l, 3, NULL, NULL, 0, NULL, NULL);
    hy_import_diff_t d; hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_OK, hy_import_parse(js, &imp, &e));
    TEST_ASSERT_EQUAL(HY_OK, hy_import_plan_parsed(&imp, HY_IMPORT_MERGE, cur, 3, exists_cb, NULL, &d, &e));
    TEST_ASSERT_EQUAL_size_t(1, d.meds_add);
    TEST_ASSERT_EQUAL_size_t(1, d.meds_same);
    TEST_ASSERT_EQUAL_size_t(0, d.meds_change);
    TEST_ASSERT_EQUAL_size_t(2, d.log_add);
    TEST_ASSERT_EQUAL_size_t(1, d.log_dup);
    for (size_t i = 0; i < n_asked; i++) TEST_ASSERT_NOT_EQUAL(2, asked_meds[i]);

    uint32_t to = 0; uint8_t kind = 0xFF;
    TEST_ASSERT_FALSE(hy_import_target(&imp, 2, &to, &kind));   /* fresh id not known yet */
    TEST_ASSERT_EQUAL_UINT8(HY_IMP_ADD, kind);
    hy_import_set_dev_id(&imp, 2, 9);
    TEST_ASSERT_TRUE(hy_import_target(&imp, 2, &to, &kind));
    TEST_ASSERT_EQUAL_UINT32(9, to);
    TEST_ASSERT_TRUE(hy_import_target(&imp, 1, &to, &kind));
    TEST_ASSERT_EQUAL_UINT32(1, to);
    TEST_ASSERT_EQUAL_UINT8(HY_IMP_SAME, kind);
    TEST_ASSERT_FALSE(hy_import_target(&imp, 5, &to, NULL));   /* not in the file */

    /* the log walk gives the entries with their file ids; the caller maps them */
    hy_import_cursor_t c; hy_log_t got; size_t k = 0;
    hy_import_log_begin(&imp, &c);
    while (hy_import_log_next(&c, &got) == 1) {
        TEST_ASSERT_EQUAL_UINT32(l[k].id, got.id);
        TEST_ASSERT_EQUAL_UINT32(l[k].med_id, got.med_id);
        k++;
    }
    TEST_ASSERT_EQUAL_size_t(3, k);
}

static void test_merge_unknown_id_takes_fresh_id(void)
{
    hy_med_t f[1];
    med(&f[0], 4, "C", "Calcium", 0);
    const char *js = build(f, 1, NULL, 0, NULL, NULL, 0, NULL, NULL);
    hy_import_diff_t d; hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_OK, hy_import_parse(js, &imp, &e));
    TEST_ASSERT_EQUAL(HY_OK, hy_import_plan_parsed(&imp, HY_IMPORT_MERGE, cur, 3, exists_cb, NULL, &d, &e));
    TEST_ASSERT_EQUAL_size_t(1, d.meds_add);
    TEST_ASSERT_EQUAL_UINT8(HY_IMP_ADD, imp.map[0].kind);
    TEST_ASSERT_EQUAL_UINT32(0, imp.map[0].dev_id);
}

static void test_merge_code_clash_refused(void)
{
    hy_med_t f[1];
    med(&f[0], 9, "A", "Another A", 0);   /* new id, but active A already exists */
    const char *js = build(f, 1, NULL, 0, NULL, NULL, 0, NULL, NULL);
    hy_import_diff_t d; hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_E_CODE_DUP, hy_import_plan(js, HY_IMPORT_MERGE, cur, 3, exists_cb, NULL, &d, &e));
    TEST_ASSERT_EQUAL_INT(409, e.http);
    /* replace keeps nothing, so the same file is fine there */
    TEST_ASSERT_EQUAL(HY_OK, hy_import_plan(js, HY_IMPORT_REPLACE, cur, 3, exists_cb, NULL, &d, &e));
}

static void test_replace_counts(void)
{
    hy_med_t f[2] = { cur[0], cur[1] };
    hy_log_t l[4] = { entry(1, 1, D0 - 40 * DAY, HY_TAKEN), entry(2, 2, D0, HY_TAKEN),
                      entry(3, 1, D0 + 10, HY_MISSED), entry(4, 9, D0 + 20, HY_TAKEN) };   /* 9: not in the file */
    hy_gap_t g[2] = { { D0 - 5000, D0 - 4000 }, { D0 - 3000, D0 - 1000 } };
    uint8_t r[2] = { HY_GAP_OFF, HY_GAP_LOST };
    hy_settings_t s; hy_settings_defaults(&s); s.volume = 33;
    n_dev = 1; dev[0] = (dkey_t){ 1, D0 - 40 * DAY, HY_TAKEN };
    const char *js = build(f, 2, l, 4, g, r, 2, &s, NULL);
    hy_import_diff_t d; hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_OK, hy_import_parse(js, &imp, &e));
    TEST_ASSERT_EQUAL(HY_OK, hy_import_plan_parsed(&imp, HY_IMPORT_REPLACE, cur, 3, exists_cb, NULL, &d, &e));
    TEST_ASSERT_EQUAL_size_t(2, d.meds_add);
    TEST_ASSERT_EQUAL_size_t(0, d.meds_change);
    TEST_ASSERT_EQUAL_size_t(0, d.meds_same);
    TEST_ASSERT_EQUAL_size_t(3, d.log_add);
    TEST_ASSERT_EQUAL_size_t(0, d.log_dup);
    TEST_ASSERT_EQUAL_size_t(1, d.log_skip);
    TEST_ASSERT_EQUAL_size_t(2, d.gaps_add);
    TEST_ASSERT_TRUE(d.settings);
    TEST_ASSERT_EQUAL_size_t(0, n_asked);   /* replace keeps nothing: no lookups */
    TEST_ASSERT_EQUAL_UINT32(2, imp.map[1].dev_id);
    TEST_ASSERT_EQUAL_UINT8(HY_IMP_ADD, imp.map[1].kind);
    TEST_ASSERT_EQUAL_INT64(D0 - 3000, imp.gaps[1].start);
    TEST_ASSERT_EQUAL_UINT8(HY_GAP_LOST, imp.gap_reasons[1]);
    TEST_ASSERT_NOT_NULL(imp.settings);
    hy_settings_t base, out; hy_settings_defaults(&base);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json(imp.settings, &base, &out, &e));
    TEST_ASSERT_EQUAL_UINT8(33, out.volume);
}

static void test_replace_duplicate_log_id_refused(void)
{
    hy_med_t f[1] = { cur[0] };
    hy_log_t l[2] = { entry(5, 1, D0, HY_TAKEN), entry(5, 1, D0 + 60, HY_TAKEN) };
    const char *js = build(f, 1, l, 2, NULL, NULL, 0, NULL, NULL);
    hy_import_diff_t d; hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_import_plan(js, HY_IMPORT_REPLACE, cur, 3, exists_cb, NULL, &d, &e));
    TEST_ASSERT_EQUAL_STRING("log", e.field);
}

static void test_bad_version(void)
{
    hy_import_diff_t d; hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_E_INVALID,
                      hy_import_plan("{\"v\":2,\"meds\":[],\"log\":[]}", HY_IMPORT_MERGE, cur, 3, exists_cb, NULL, &d, &e));
    TEST_ASSERT_EQUAL_STRING("v", e.field);
    TEST_ASSERT_EQUAL_INT(400, e.http);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_import_plan("{\"meds\":[]}", HY_IMPORT_MERGE, cur, 3, exists_cb, NULL, &d, &e));
    TEST_ASSERT_EQUAL_STRING("v", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_import_plan("[1,2]", HY_IMPORT_MERGE, cur, 3, exists_cb, NULL, &d, &e));
    TEST_ASSERT_EQUAL_STRING("json", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID,
                      hy_import_plan("{\"v\":1,\"meds\":[],\"log\":[{\"id\":1,", HY_IMPORT_MERGE, cur, 3, exists_cb,
                                     NULL, &d, &e));
    TEST_ASSERT_EQUAL_STRING("log", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID,
                      hy_import_plan("{\"v\":1,\"meds\":[],\"log\":[{\"id\":1}]}", HY_IMPORT_MERGE, cur, 3, exists_cb,
                                     NULL, &d, &e));
    TEST_ASSERT_EQUAL_STRING("log", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID,
                      hy_import_plan("{\"v\":1,\"meds\":[],\"log\":[]} x", HY_IMPORT_MERGE, cur, 3, exists_cb, NULL,
                                     &d, &e));
    TEST_ASSERT_EQUAL_STRING("json", e.field);
}

static void test_log_member_anywhere_and_tricky_strings(void)
{
    /* the log first, brackets and escaped quotes inside a note, whitespace between elements */
    const char *js =
        "{ \"log\" : [ {\"id\":3,\"ts\":1793145600,\"tz\":\"UTC0\",\"med_id\":1,\"status\":\"taken\","
        "\"note\":\"a \\\"]}{[ b\\\\\",\"source\":\"web\",\"edited\":false} ,\n"
        " {\"id\":4,\"ts\":1793145700,\"tz\":\"UTC0\",\"med_id\":1,\"status\":\"skipped\"} ] ,"
        " \"v\":1, \"meds\":[{\"id\":1,\"code\":\"A\",\"name\":\"Antibiotic\",\"schedule\":{\"type\":\"as_needed\"}}], \"note\":\"\\\"log\\\":[\" }";
    hy_import_diff_t d; hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_OK, hy_import_parse(js, &imp, &e));
    TEST_ASSERT_EQUAL_size_t(2, imp.n_log);
    hy_import_cursor_t c; hy_log_t got;
    hy_import_log_begin(&imp, &c);
    TEST_ASSERT_EQUAL_INT(1, hy_import_log_next(&c, &got));
    TEST_ASSERT_EQUAL_STRING("a \"]}{[ b\\", got.note);
    TEST_ASSERT_EQUAL_INT(HY_SRC_WEB, got.source);
    TEST_ASSERT_EQUAL_INT(1, hy_import_log_next(&c, &got));
    TEST_ASSERT_EQUAL_INT(HY_SKIPPED, got.status);
    TEST_ASSERT_EQUAL_INT(0, hy_import_log_next(&c, &got));
    TEST_ASSERT_EQUAL(HY_OK, hy_import_plan_parsed(&imp, HY_IMPORT_MERGE, cur, 3, exists_cb, NULL, &d, &e));
    TEST_ASSERT_EQUAL_size_t(2, d.log_add);
}

static void test_secrets(void)
{
    hy_backup_wifi_t w; memset(&w, 0, sizeof w);
    w.n = 1; strcpy(w.ssid[0], "HomeNet"); strcpy(w.pass[0], "example-pass"); strcpy(w.ap_pass, "abcdefgh23");
    hy_med_t f[1] = { cur[0] };
    const char *js = build(f, 1, NULL, 0, NULL, NULL, 0, NULL, &w);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"wifi\""));
    hy_import_diff_t d; hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_OK, hy_import_parse(js, &imp, &e));
    TEST_ASSERT_EQUAL(HY_OK, hy_import_plan_parsed(&imp, HY_IMPORT_MERGE, cur, 3, exists_cb, NULL, &d, &e));
    TEST_ASSERT_TRUE(d.secrets);
    TEST_ASSERT_TRUE(imp.has_wifi);
    TEST_ASSERT_EQUAL_size_t(1, imp.wifi.n);
    TEST_ASSERT_EQUAL_STRING("HomeNet", imp.wifi.ssid[0]);
    TEST_ASSERT_EQUAL_STRING("example-pass", imp.wifi.pass[0]);
    TEST_ASSERT_EQUAL_STRING("abcdefgh23", imp.wifi.ap_pass);
    hy_import_free(&imp);
    TEST_ASSERT_EQUAL_STRING("", imp.wifi.pass[0]);   /* wiped */

    /* without secrets: no wifi member at all */
    js = build(f, 1, NULL, 0, NULL, NULL, 0, NULL, NULL);
    TEST_ASSERT_NULL(strstr(js, "wifi"));
    /* a bad password is refused */
    const char *bad = "{\"v\":1,\"meds\":[],\"wifi\":{\"networks\":[{\"ssid\":\"HomeNet\",\"pass\":\"short\"}]}}";
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_import_plan(bad, HY_IMPORT_MERGE, cur, 3, exists_cb, NULL, &d, &e));
    TEST_ASSERT_EQUAL_STRING("wifi", e.field);
}

static void test_import_roundtrip(void)
{
    static hy_log_t l[300];
    hy_med_t f[3] = { cur[0], cur[1], cur[2] };
    f[2].archived = true;
    f[1].pills_enabled = true; f[1].pills_left = 17;
    for (size_t i = 0; i < 300; i++) {
        l[i] = entry((uint32_t)(i + 1), (uint32_t)(i % 3 + 1), D0 - 90 * DAY + (int64_t)i * 7 * 3600,
                     (hy_status_t)(i % 3));
        l[i].slot = l[i].ts - 600; l[i].delta_min = 10; l[i].source = (hy_source_t)(i % 4);
        l[i].edited = (i % 5) == 0;
        snprintf(l[i].note, sizeof l[i].note, "n%zu \"q\" [x] {y} \\", i);
    }
    hy_gap_t g[1] = { { D0 - 9000, D0 - 8000 } };
    uint8_t r[1] = { HY_GAP_CLOCK };
    const char *js = build(f, 3, l, 300, g, r, 1, NULL, NULL);
    hy_import_diff_t d; hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_OK, hy_import_parse(js, &imp, &e));
    TEST_ASSERT_EQUAL(HY_OK, hy_import_plan_parsed(&imp, HY_IMPORT_REPLACE, NULL, 0, exists_cb, NULL, &d, &e));
    TEST_ASSERT_EQUAL_size_t(3, d.meds_add);
    TEST_ASSERT_EQUAL_size_t(300, d.log_add);
    TEST_ASSERT_EQUAL_size_t(0, d.log_dup);
    TEST_ASSERT_EQUAL_size_t(0, d.log_skip);
    TEST_ASSERT_EQUAL_size_t(1, d.gaps_add);
    TEST_ASSERT_EQUAL_size_t(300, imp.n_log);
    TEST_ASSERT_EQUAL_UINT32(4, imp.next_id);
    TEST_ASSERT_EQUAL_INT64(D0, imp.exported);
    TEST_ASSERT_TRUE(imp.meds[2].archived);
    TEST_ASSERT_EQUAL_INT16(17, imp.meds[1].pills_left);
    hy_import_cursor_t c; hy_log_t got; size_t k = 0;
    hy_import_log_begin(&imp, &c);
    while (hy_import_log_next(&c, &got) == 1) {
        TEST_ASSERT_EQUAL_UINT32(l[k].id, got.id);
        TEST_ASSERT_EQUAL_INT64(l[k].ts, got.ts);
        TEST_ASSERT_EQUAL_INT64(l[k].slot, got.slot);
        TEST_ASSERT_EQUAL_INT32(l[k].delta_min, got.delta_min);
        TEST_ASSERT_EQUAL_INT(l[k].status, got.status);
        TEST_ASSERT_EQUAL_INT(l[k].source, got.source);
        TEST_ASSERT_EQUAL(l[k].edited, got.edited);
        TEST_ASSERT_EQUAL_STRING(l[k].note, got.note);
        k++;
    }
    TEST_ASSERT_EQUAL_size_t(300, k);
    /* merging the same file onto the same device finds every entry */
    hy_med_t same[3] = { f[0], f[1], f[2] };
    hy_import_diff_t d2;
    hy_import_free(&imp);
    TEST_ASSERT_EQUAL(HY_OK, hy_import_plan(js, HY_IMPORT_MERGE, same, 3, exists_cb, NULL, &d2, &e));
    TEST_ASSERT_EQUAL_size_t(3, d2.meds_same);
    TEST_ASSERT_EQUAL_size_t(300, d2.log_add + d2.log_dup);
}

static void test_too_many_meds(void)
{
    static hy_med_t f[HY_MAX_STORED + 1];
    char code[3];
    for (size_t i = 0; i <= HY_MAX_STORED; i++) {
        snprintf(code, sizeof code, "%c%c", 'A' + (int)(i / 26) % 26, 'A' + (int)(i % 26));
        med(&f[i], (uint32_t)(i + 1), code, "Test", (uint16_t)i);
        f[i].archived = true;
    }
    const char *js = build(f, HY_MAX_STORED + 1, NULL, 0, NULL, NULL, 0, NULL, NULL);
    hy_import_diff_t d; hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_E_FULL, hy_import_plan(js, HY_IMPORT_REPLACE, cur, 3, exists_cb, NULL, &d, &e));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_preview_merge_counts);
    RUN_TEST(test_merge_id_collision_remap);
    RUN_TEST(test_merge_unknown_id_takes_fresh_id);
    RUN_TEST(test_merge_code_clash_refused);
    RUN_TEST(test_replace_counts);
    RUN_TEST(test_replace_duplicate_log_id_refused);
    RUN_TEST(test_bad_version);
    RUN_TEST(test_log_member_anywhere_and_tricky_strings);
    RUN_TEST(test_secrets);
    RUN_TEST(test_import_roundtrip);
    RUN_TEST(test_too_many_meds);
    free(buf);
    return UNITY_END();
}
