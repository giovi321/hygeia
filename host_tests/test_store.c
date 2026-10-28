#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "unity.h"
#include "hy/codec.h"
#include "hy/med.h"
#include "hy/store.h"

/* 2026-10-28 00:00 UTC; months: Sep 2026 = D0 - 40d, Oct 2026 = D0, Nov 2026 = D0 + 10d */
#define D0 1793145600LL
#define DAY 86400LL
#define SEP (D0 - 40 * DAY)
#define OCT D0
#define NOV (D0 + 10 * DAY)

static char base[64];
static const hy_store_port_t *P;

static void rmtree(void)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    (void)system(cmd);
}

void setUp(void)
{
    strcpy(base, "/tmp/hy_store_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(base));
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    P = hy_store_port();
}
void tearDown(void) { rmtree(); }

static hy_log_t mk(int64_t ts, uint32_t med)
{
    hy_log_t e;
    memset(&e, 0, sizeof e);
    e.ts = ts; e.med_id = med; e.status = HY_TAKEN; strcpy(e.tz, "UTC0");
    return e;
}

static hy_err_t add(int64_t ts, uint32_t med, uint32_t *id)
{
    hy_log_t e = mk(ts, med);
    hy_err_t r = P->append_log(NULL, &e);
    if (id) *id = e.id;
    return r;
}

static size_t q(int64_t from, int64_t to, uint32_t med, hy_log_t *out, size_t max)
{
    return P->query_log(NULL, from, to, med, out, max);
}

static void path(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", base, rel); }

static bool exists(const char *rel)
{
    char p[160];
    struct stat st;
    path(p, sizeof p, rel);
    return stat(p, &st) == 0;
}

static void write_raw(const char *rel, const char *data)
{
    char p[160];
    path(p, sizeof p, rel);
    FILE *f = fopen(p, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs(data, f);
    fclose(f);
}

void test_append_query_by_range_and_med(void)
{
    uint32_t id1, id2, id3, id4, id5;
    TEST_ASSERT_EQUAL(HY_OK, add(OCT + 3600, 1, &id1));
    TEST_ASSERT_EQUAL(HY_OK, add(SEP + 3600, 2, &id2));
    TEST_ASSERT_EQUAL(HY_OK, add(NOV, 1, &id3));
    TEST_ASSERT_EQUAL(HY_OK, add(SEP, 1, &id4));
    TEST_ASSERT_EQUAL(HY_OK, add(OCT + 3600, 2, &id5));   /* same ts as id1: stable by id */
    TEST_ASSERT_EQUAL_UINT32(1, id1);
    TEST_ASSERT_EQUAL_UINT32(5, id5);
    TEST_ASSERT_TRUE(exists("log/2026-09.jsonl"));
    TEST_ASSERT_TRUE(exists("log/2026-10.jsonl"));
    TEST_ASSERT_TRUE(exists("log/2026-11.jsonl"));

    hy_log_t out[16];
    size_t n = q(SEP, NOV + 1, 0, out, 16);
    TEST_ASSERT_EQUAL(5, n);
    TEST_ASSERT_EQUAL_UINT32(id4, out[0].id);
    TEST_ASSERT_EQUAL_UINT32(id2, out[1].id);
    TEST_ASSERT_EQUAL_UINT32(id1, out[2].id);
    TEST_ASSERT_EQUAL_UINT32(id5, out[3].id);
    TEST_ASSERT_EQUAL_UINT32(id3, out[4].id);

    /* [from, to): to is exclusive, from inclusive */
    n = q(SEP + 3600, NOV, 0, out, 16);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_UINT32(id2, out[0].id);

    n = q(SEP, NOV + 1, 1, out, 16);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_UINT32(id4, out[0].id);
    TEST_ASSERT_EQUAL_UINT32(id1, out[1].id);
    TEST_ASSERT_EQUAL_UINT32(id3, out[2].id);

    /* max keeps the earliest */
    n = q(SEP, NOV + 1, 0, out, 2);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_UINT32(id4, out[0].id);
    TEST_ASSERT_EQUAL_UINT32(id2, out[1].id);

    TEST_ASSERT_EQUAL(0, q(NOV + 100 * DAY, NOV + 200 * DAY, 0, out, 16));
    TEST_ASSERT_EQUAL(0, q(OCT, OCT, 0, out, 16));
    TEST_ASSERT_EQUAL(0, q(0, INT64_MAX, 0, out, 0));
    TEST_ASSERT_EQUAL(5, q(0, INT64_MAX, 0, out, 16));
}

void test_append_stores_all_fields(void)
{
    hy_log_t e = mk(OCT + 100, 4), out[2];
    e.slot = OCT; e.delta_min = 1; e.source = HY_SRC_WEB; e.edited = true;
    strcpy(e.note, "n\"1\"\nx");
    TEST_ASSERT_EQUAL(HY_OK, P->append_log(NULL, &e));
    TEST_ASSERT_EQUAL(1, q(OCT, OCT + DAY, 4, out, 2));
    TEST_ASSERT_EQUAL_UINT32(e.id, out[0].id);
    TEST_ASSERT_EQUAL_INT64(OCT, out[0].slot);
    TEST_ASSERT_EQUAL_STRING("n\"1\"\nx", out[0].note);
    TEST_ASSERT_EQUAL(HY_SRC_WEB, out[0].source);
    TEST_ASSERT_TRUE(out[0].edited);
}

void test_update_and_delete_rewrite_month(void)
{
    uint32_t a, b, c;
    add(OCT + 10, 1, &a); add(OCT + 20, 1, &b); add(OCT + 30, 2, &c);
    hy_log_t out[8];

    /* update in place */
    hy_log_t e = mk(OCT + 20, 1);
    e.id = b; e.status = HY_SKIPPED; e.edited = true; strcpy(e.note, "changed");
    TEST_ASSERT_EQUAL(HY_OK, P->update_log(NULL, &e));
    TEST_ASSERT_EQUAL(3, q(OCT, OCT + DAY, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(b, out[1].id);
    TEST_ASSERT_EQUAL(HY_SKIPPED, out[1].status);
    TEST_ASSERT_EQUAL_STRING("changed", out[1].note);
    TEST_ASSERT_TRUE(out[1].edited);
    TEST_ASSERT_EQUAL_UINT32(a, out[0].id);
    TEST_ASSERT_FALSE(out[0].edited);

    /* update unknown id */
    e.id = 999;
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, P->update_log(NULL, &e));

    /* ts changed within same month still found */
    e.id = a; e.ts = OCT + 5 * DAY;
    TEST_ASSERT_EQUAL(HY_OK, P->update_log(NULL, &e));
    TEST_ASSERT_EQUAL(1, q(OCT + 5 * DAY, OCT + 6 * DAY, 0, out, 8));

    /* update with ts moved into another month (ruling R7) */
    e.id = c; e.ts = NOV + 7; e.med_id = 2; e.status = HY_TAKEN;
    TEST_ASSERT_EQUAL(HY_OK, P->update_log(NULL, &e));
    TEST_ASSERT_EQUAL(1, q(NOV, NOV + DAY, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(c, out[0].id);
    TEST_ASSERT_EQUAL(0, q(OCT + 29, OCT + 31, 0, out, 8));
    TEST_ASSERT_EQUAL(3, q(0, INT64_MAX, 0, out, 8));                 /* no duplicate */

    /* delete: wrong month is not found, right month works */
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, P->delete_log(NULL, b, NOV));
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, P->delete_log(NULL, b, SEP));   /* no such file */
    TEST_ASSERT_EQUAL(HY_OK, P->delete_log(NULL, b, OCT + 20));
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, P->delete_log(NULL, b, OCT + 20));
    TEST_ASSERT_EQUAL(2, q(0, INT64_MAX, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(a, out[0].id);
    TEST_ASSERT_EQUAL_UINT32(c, out[1].id);
    TEST_ASSERT_FALSE(exists("log/2026-10.jsonl.tmp"));
}

void test_atomic_write_ignores_tmp(void)
{
    hy_med_t m[2], d[8];
    size_t n = 0;
    hy_med_defaults(&m[0]); m[0].id = 1; strcpy(m[0].code, "A"); strcpy(m[0].name, "Antibiotic");
    hy_med_defaults(&m[1]); m[1].id = 2; strcpy(m[1].code, "V"); strcpy(m[1].name, "Vitamin D");
    TEST_ASSERT_EQUAL(HY_OK, P->save_meds(NULL, m, 2));
    TEST_ASSERT_FALSE(exists("meds.json.tmp"));
    write_raw("meds.json.tmp", "{garbage that is not json");
    write_raw("log/2026-10.jsonl.tmp", "garbage");
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    TEST_ASSERT_FALSE(exists("meds.json.tmp"));
    TEST_ASSERT_FALSE(exists("log/2026-10.jsonl.tmp"));
    TEST_ASSERT_EQUAL(HY_OK, P->load_meds(NULL, d, 8, &n));
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("Antibiotic", d[0].name);
    TEST_ASSERT_EQUAL_STRING("Vitamin D", d[1].name);
}

void test_load_meds_missing_and_corrupt(void)
{
    hy_med_t d[4];
    size_t n = 55;
    TEST_ASSERT_EQUAL(HY_OK, P->load_meds(NULL, d, 4, &n));
    TEST_ASSERT_EQUAL(0, n);
    write_raw("meds.json", "{\"v\":1,\"meds\":[{\"id\":1,");
    TEST_ASSERT_EQUAL(HY_E_IO, P->load_meds(NULL, d, 4, &n));
    /* corrupt file is left alone */
    char p[160];
    struct stat st;
    path(p, sizeof p, "meds.json");
    TEST_ASSERT_EQUAL(0, stat(p, &st));
    TEST_ASSERT_EQUAL(23, st.st_size);
}

void test_save_meds_replaces_previous(void)
{
    hy_med_t m[1], d[4];
    size_t n;
    hy_med_defaults(&m[0]); m[0].id = 1; strcpy(m[0].code, "M"); strcpy(m[0].name, "Magnesium");
    TEST_ASSERT_EQUAL(HY_OK, P->save_meds(NULL, m, 1));
    strcpy(m[0].name, "Magnesium 2");
    TEST_ASSERT_EQUAL(HY_OK, P->save_meds(NULL, m, 1));
    TEST_ASSERT_EQUAL(HY_OK, P->load_meds(NULL, d, 4, &n));
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("Magnesium 2", d[0].name);
    TEST_ASSERT_EQUAL(HY_OK, P->save_meds(NULL, m, 0));
    TEST_ASSERT_EQUAL(HY_OK, P->load_meds(NULL, d, 4, &n));
    TEST_ASSERT_EQUAL(0, n);
}

void test_corrupt_line_skipped(void)
{
    uint32_t a, b;
    add(OCT + 10, 1, &a); add(OCT + 20, 1, &b);
    /* truncated last line, no newline */
    char p[160];
    path(p, sizeof p, "log/2026-10.jsonl");
    FILE *f = fopen(p, "ab");
    fputs("{\"id\":3,\"ts\":17931456", f);
    fclose(f);

    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    TEST_ASSERT_EQUAL(0, hy_store_skipped_lines());
    hy_log_t out[8];
    TEST_ASSERT_EQUAL(2, q(OCT, OCT + DAY, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(a, out[0].id);
    TEST_ASSERT_EQUAL_UINT32(b, out[1].id);
    TEST_ASSERT_EQUAL(1, hy_store_skipped_lines());

    /* the next append starts on a fresh line and is readable */
    uint32_t c;
    TEST_ASSERT_EQUAL(HY_OK, add(OCT + 30, 1, &c));
    TEST_ASSERT_EQUAL_UINT32(b + 1, c);
    TEST_ASSERT_EQUAL(3, q(OCT, OCT + DAY, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(c, out[2].id);
}

void test_overlong_line_skipped(void)
{
    uint32_t a;
    add(OCT + 10, 1, &a);
    char p[160];
    path(p, sizeof p, "log/2026-10.jsonl");
    FILE *f = fopen(p, "ab");
    fputs("{\"id\":9,\"note\":\"", f);
    for (int i = 0; i < 5000; i++) fputc('x', f);
    fputs("\"}\n", f);
    fclose(f);
    uint32_t b;
    TEST_ASSERT_EQUAL(HY_OK, add(OCT + 20, 1, &b));
    hy_log_t out[8];
    TEST_ASSERT_EQUAL(2, q(OCT, OCT + DAY, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(a, out[0].id);
    TEST_ASSERT_EQUAL_UINT32(b, out[1].id);
    TEST_ASSERT_EQUAL(1, hy_store_skipped_lines());
}

void test_ids_continue_after_reopen(void)
{
    uint32_t a, b, c, d;
    add(SEP + 5, 1, &a);
    add(OCT + 5, 1, &b);
    add(OCT + 6, 2, &c);
    TEST_ASSERT_EQUAL_UINT32(1, a);
    TEST_ASSERT_EQUAL_UINT32(3, c);
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    TEST_ASSERT_EQUAL(HY_OK, add(OCT + 7, 1, &d));
    TEST_ASSERT_EQUAL_UINT32(4, d);

    /* ids are global: a new month continues the sequence */
    uint32_t e;
    TEST_ASSERT_EQUAL(HY_OK, add(NOV + 1, 1, &e));
    TEST_ASSERT_EQUAL_UINT32(5, e);
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    uint32_t f;
    TEST_ASSERT_EQUAL(HY_OK, add(NOV + 2, 1, &f));
    TEST_ASSERT_EQUAL_UINT32(6, f);
}

void test_ids_start_at_one_on_empty_store(void)
{
    uint32_t a;
    TEST_ASSERT_EQUAL(HY_OK, add(OCT, 1, &a));
    TEST_ASSERT_EQUAL_UINT32(1, a);
}

void test_open_missing_base_fails(void)
{
    TEST_ASSERT_EQUAL(HY_E_IO, hy_store_open("/tmp/hy_store_does_not_exist_dir/x"));
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_append_query_by_range_and_med);
    RUN_TEST(test_append_stores_all_fields);
    RUN_TEST(test_update_and_delete_rewrite_month);
    RUN_TEST(test_atomic_write_ignores_tmp);
    RUN_TEST(test_load_meds_missing_and_corrupt);
    RUN_TEST(test_save_meds_replaces_previous);
    RUN_TEST(test_corrupt_line_skipped);
    RUN_TEST(test_overlong_line_skipped);
    RUN_TEST(test_ids_continue_after_reopen);
    RUN_TEST(test_ids_start_at_one_on_empty_store);
    RUN_TEST(test_open_missing_base_fails);
    return UNITY_END();
}
