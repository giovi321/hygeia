#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "unity.h"
#include "hy/codec.h"
#include "hy/gap.h"
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

void test_ids_unique_across_months_after_reopen(void)
{
    uint32_t a, b, c, d;
    TEST_ASSERT_EQUAL(HY_OK, add(NOV, 1, &a));          /* newest month */
    TEST_ASSERT_EQUAL(HY_OK, add(OCT + 5, 1, &b));      /* backdated: higher id in older month */
    TEST_ASSERT_EQUAL_UINT32(1, a);
    TEST_ASSERT_EQUAL_UINT32(2, b);
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    TEST_ASSERT_EQUAL(HY_OK, add(NOV + 1, 1, &c));
    TEST_ASSERT_EQUAL(HY_OK, add(SEP, 1, &d));
    TEST_ASSERT_TRUE(c != a && c != b && d != a && d != b && c != d);
    TEST_ASSERT_EQUAL_UINT32(3, c);
    TEST_ASSERT_EQUAL_UINT32(4, d);
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

static void put_file(const char *rel, const char *content)
{
    char p[160];
    path(p, sizeof p, rel);
    FILE *f = fopen(p, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs(content, f);
    fclose(f);
}

#define TS_2026_01 1767225600LL
#define TS_2026_02 1769904000LL
#define TS_2026_03 1772323200LL
#define TS_2026_04 1775001600LL
#define TS_2026_05 1777593600LL

void test_month_index_ignores_strays(void)
{
    uint32_t a;
    TEST_ASSERT_EQUAL(HY_OK, add(TS_2026_01 + 10, 1, &a));
    put_file("log/2026-13.jsonl", "{\"id\":50,\"ts\":1767225700}\n");
    put_file("log/2026-1.jsonl", "{\"id\":51,\"ts\":1767225700}\n");
    put_file("log/notes.txt", "hello\n");
    put_file("log/2026-01.jsonl.bak", "{\"id\":52,\"ts\":1767225700}\n");
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    hy_log_t out[8];
    TEST_ASSERT_EQUAL(1, q(TS_2026_01, TS_2026_02, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(a, out[0].id);
    TEST_ASSERT_EQUAL(0, q(TS_2026_02, TS_2026_05, 0, out, 8));
    /* stray ids do not feed the id counter */
    uint32_t b;
    TEST_ASSERT_EQUAL(HY_OK, add(TS_2026_01 + 20, 1, &b));
    TEST_ASSERT_EQUAL_UINT32(a + 1, b);
}

void test_query_across_missing_months(void)
{
    uint32_t a, b;
    TEST_ASSERT_EQUAL(HY_OK, add(TS_2026_03 + 5, 1, &b));
    TEST_ASSERT_EQUAL(HY_OK, add(TS_2026_01 + 5, 1, &a));
    hy_log_t out[8];
    TEST_ASSERT_EQUAL(2, q(TS_2026_01, TS_2026_04, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(a, out[0].id);
    TEST_ASSERT_EQUAL_UINT32(b, out[1].id);
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    TEST_ASSERT_EQUAL(2, q(TS_2026_01, TS_2026_04, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(a, out[0].id);
    TEST_ASSERT_EQUAL_UINT32(b, out[1].id);
}

void test_new_month_after_open_is_indexed(void)
{
    uint32_t a, b;
    TEST_ASSERT_EQUAL(HY_OK, add(TS_2026_01 + 5, 1, &a));
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    TEST_ASSERT_EQUAL(HY_OK, add(TS_2026_05 + 5, 1, &b));
    hy_log_t out[8];
    TEST_ASSERT_EQUAL(1, q(TS_2026_05, TS_2026_05 + DAY, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(b, out[0].id);
    hy_log_t e = out[0];
    strcpy(e.note, "edited");
    TEST_ASSERT_EQUAL(HY_OK, P->update_log(NULL, &e));
    TEST_ASSERT_EQUAL(1, q(TS_2026_05, TS_2026_05 + DAY, 0, out, 8));
    TEST_ASSERT_EQUAL_STRING("edited", out[0].note);
    TEST_ASSERT_EQUAL(HY_OK, P->delete_log(NULL, b, e.ts));
    TEST_ASSERT_EQUAL(0, q(TS_2026_05, TS_2026_05 + DAY, 0, out, 8));
}

void test_update_moves_into_month_without_file(void)
{
    uint32_t a;
    TEST_ASSERT_EQUAL(HY_OK, add(TS_2026_01 + 5, 1, &a));
    hy_log_t out[8];
    TEST_ASSERT_EQUAL(1, q(TS_2026_01, TS_2026_02, 0, out, 8));
    hy_log_t e = out[0];
    e.ts = TS_2026_02 + 5;
    TEST_ASSERT_EQUAL(HY_OK, P->update_log(NULL, &e));
    TEST_ASSERT_EQUAL(0, q(TS_2026_01, TS_2026_02, 0, out, 8));
    TEST_ASSERT_EQUAL(1, q(TS_2026_02, TS_2026_03, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(a, out[0].id);
    /* the new month is indexed: a second update and a delete there work */
    e.ts = TS_2026_02 + 9;
    TEST_ASSERT_EQUAL(HY_OK, P->update_log(NULL, &e));
    TEST_ASSERT_EQUAL(HY_OK, P->delete_log(NULL, a, e.ts));
    TEST_ASSERT_EQUAL(0, q(TS_2026_01, TS_2026_03, 0, out, 8));
}

void test_id_exhaustion(void)
{
    hy_log_t e = mk(TS_2026_01 + 5, 1);
    e.id = 4294967294u;
    char line[1024];
    TEST_ASSERT_TRUE(hy_log_to_jsonl(&e, line, sizeof line) > 0);
    strcat(line, "\n");
    put_file("log/2026-01.jsonl", line);
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    uint32_t id = 7;
    TEST_ASSERT_EQUAL(HY_E_FULL, add(TS_2026_01 + 9, 1, &id));
    TEST_ASSERT_EQUAL(HY_E_FULL, add(TS_2026_03 + 9, 1, &id));
    hy_log_t out[4];
    TEST_ASSERT_EQUAL(1, q(TS_2026_01, TS_2026_04, 0, out, 4));
    TEST_ASSERT_EQUAL_UINT32(4294967294u, out[0].id);
    char p[160];
    path(p, sizeof p, "log/2026-01.jsonl");
    struct stat sb;
    TEST_ASSERT_EQUAL(0, stat(p, &sb));
    TEST_ASSERT_EQUAL((long)strlen(line), (long)sb.st_size);
    path(p, sizeof p, "log/2026-03.jsonl");
    TEST_ASSERT_NOT_EQUAL(0, stat(p, &sb));
}

void test_id_exhaustion_at_uint32_max(void)
{
    hy_log_t e = mk(TS_2026_01 + 5, 1);
    e.id = 4294967295u;
    char line[1024];
    TEST_ASSERT_TRUE(hy_log_to_jsonl(&e, line, sizeof line) > 0);
    strcat(line, "\n");
    put_file("log/2026-01.jsonl", line);
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    uint32_t id = 7;
    TEST_ASSERT_EQUAL(HY_E_FULL, add(TS_2026_01 + 9, 1, &id));
    char p[160];
    path(p, sizeof p, "log/2026-01.jsonl");
    struct stat sb;
    TEST_ASSERT_EQUAL(0, stat(p, &sb));
    TEST_ASSERT_EQUAL((long)strlen(line), (long)sb.st_size);
}

static void make_month_files(int n)
{
    for (int i = 0; i < n; i++) {
        char rel[40];
        snprintf(rel, sizeof rel, "log/%04d-%02d.jsonl", 1 + i / 12, 1 + i % 12);
        put_file(rel, "");
    }
}

void test_open_with_too_many_months_is_full(void)
{
    make_month_files(1200);
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    make_month_files(1201);
    TEST_ASSERT_EQUAL(HY_E_FULL, hy_store_open(base));
}

void test_append_to_new_month_when_index_full(void)
{
    make_month_files(1200);
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    uint32_t id = 7;
    TEST_ASSERT_EQUAL(HY_E_FULL, add(TS_2026_01 + 9, 1, &id));
    char p[160];
    struct stat sb;
    path(p, sizeof p, "log/2026-01.jsonl");
    TEST_ASSERT_NOT_EQUAL(0, stat(p, &sb));
    /* an existing month still accepts entries (0001-01-01 00:00 UTC) */
    TEST_ASSERT_EQUAL(HY_OK, add(-62135596800LL + 5, 1, &id));
    hy_log_t out[4];
    TEST_ASSERT_EQUAL(1, q(-62135596800LL, -62135596800LL + DAY, 0, out, 4));
}

void test_update_into_new_month_when_index_full(void)
{
    make_month_files(1200);
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    uint32_t id;
    int64_t y1 = -62135596800LL;
    TEST_ASSERT_EQUAL(HY_OK, add(y1 + 5, 1, &id));
    hy_log_t out[4];
    TEST_ASSERT_EQUAL(1, q(y1, y1 + DAY, 0, out, 4));
    hy_log_t e = out[0];
    e.ts = TS_2026_01 + 5;
    TEST_ASSERT_EQUAL(HY_E_FULL, P->update_log(NULL, &e));
    TEST_ASSERT_EQUAL(1, q(y1, y1 + DAY, 0, out, 4));
    char p[160];
    struct stat sb;
    path(p, sizeof p, "log/2026-01.jsonl");
    TEST_ASSERT_NOT_EQUAL(0, stat(p, &sb));
}

/* ---- gaps ---- */
static void put_gap(int64_t s, int64_t e, uint8_t r)
{
    hy_gap_t g = { s, e };
    TEST_ASSERT_EQUAL(HY_OK, P->append_gap(NULL, &g, r));
}

static size_t gap_lines(void)
{
    char p[128], line[256];
    path(p, sizeof p, "gaps.jsonl");
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t n = 0;
    while (fgets(line, sizeof line, f)) n++;
    fclose(f);
    return n;
}

void test_purge_med(void)
{
    uint32_t keep[3];
    TEST_ASSERT_NOT_NULL(P->purge_med);
    /* med 4 in September, October and November; med 1 beside it in each */
    for (int i = 0; i < 3; i++) {
        int64_t m = i == 0 ? SEP : i == 1 ? OCT : NOV;
        TEST_ASSERT_EQUAL(HY_OK, add(m + 100, 4, NULL));
        TEST_ASSERT_EQUAL(HY_OK, add(m + 200, 1, &keep[i]));
        TEST_ASSERT_EQUAL(HY_OK, add(m + 300, 4, NULL));
    }
    TEST_ASSERT_EQUAL(6, P->purge_med(NULL, 4));
    hy_log_t out[32];
    TEST_ASSERT_EQUAL(0, q(SEP - DAY, NOV + 5 * DAY, 4, out, 32));
    TEST_ASSERT_EQUAL(3, q(SEP - DAY, NOV + 5 * DAY, 1, out, 32));
    for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL_UINT32(keep[i], out[i].id);
    TEST_ASSERT_EQUAL(0, hy_store_skipped_lines());
    TEST_ASSERT_FALSE(exists("log/2026-09.jsonl.tmp"));
    /* nothing left to remove: no rewrite, zero */
    TEST_ASSERT_EQUAL(0, P->purge_med(NULL, 4));
    TEST_ASSERT_EQUAL(0, P->purge_med(NULL, 99));
    TEST_ASSERT_EQUAL(HY_OK, add(SEP + 400, 4, NULL));
    TEST_ASSERT_EQUAL(1, P->purge_med(NULL, 4));
    TEST_ASSERT_EQUAL(3, q(SEP - DAY, NOV + 5 * DAY, 1, out, 32));
}

void test_next_id_port(void)
{
    TEST_ASSERT_NOT_NULL(P->save_meds_next);
    TEST_ASSERT_NOT_NULL(P->load_next_id);
    hy_med_t m[2], d[4];
    size_t n = 0;
    memset(m, 0, sizeof m);
    hy_med_defaults(&m[0]); hy_med_defaults(&m[1]);
    m[0].id = 1; strcpy(m[0].code, "A"); strcpy(m[0].name, "Antibiotic");
    m[1].id = 2; strcpy(m[1].code, "V"); strcpy(m[1].name, "Vitamin D");
    TEST_ASSERT_EQUAL(HY_OK, P->save_meds_next(NULL, m, 2, 9));
    TEST_ASSERT_EQUAL(HY_OK, P->load_meds(NULL, d, 4, &n));
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_UINT32(9, P->load_next_id(NULL));
    /* a reopen reads it from the file */
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    TEST_ASSERT_EQUAL(HY_OK, P->load_meds(NULL, d, 4, &n));
    TEST_ASSERT_EQUAL_UINT32(9, P->load_next_id(NULL));
    /* a file written without it gives max id + 1 */
    write_raw("meds.json", "{\"v\":1,\"meds\":[{\"id\":5,\"code\":\"A\",\"name\":\"a\"}]}");
    TEST_ASSERT_EQUAL(HY_OK, P->load_meds(NULL, d, 4, &n));
    TEST_ASSERT_EQUAL_UINT32(6, P->load_next_id(NULL));
}

void test_gaps_append_query(void)
{
    put_gap(1000, 2000, HY_GAP_OFF);
    put_gap(3000, 4000, HY_GAP_LOST);
    put_gap(5000, 6000, HY_GAP_JUMP);
    hy_gap_t out[8];
    uint8_t rs[8];
    TEST_ASSERT_EQUAL(3, P->query_gaps(NULL, 0, 10000, out, rs, 8));
    TEST_ASSERT_EQUAL_INT64(1000, out[0].start);
    TEST_ASSERT_EQUAL_INT64(5000, out[2].start);
    /* overlap, not containment; end is exclusive */
    TEST_ASSERT_EQUAL(2, P->query_gaps(NULL, 3500, 5001, out, rs, 8));
    TEST_ASSERT_EQUAL_INT64(3000, out[0].start);
    TEST_ASSERT_EQUAL_UINT8(HY_GAP_LOST, rs[0]);
    TEST_ASSERT_EQUAL_INT64(5000, out[1].start);
    TEST_ASSERT_EQUAL_UINT8(HY_GAP_JUMP, rs[1]);
    TEST_ASSERT_EQUAL(1, P->query_gaps(NULL, 3000, 4000, out, rs, 8));
    TEST_ASSERT_EQUAL(0, P->query_gaps(NULL, 2000, 3000, out, rs, 8));
    TEST_ASSERT_EQUAL(0, P->query_gaps(NULL, 7000, 7000, out, rs, 8));
    TEST_ASSERT_EQUAL(2, P->query_gaps(NULL, 0, 10000, out, rs, 2));
    /* survives reopen */
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    P = hy_store_port();
    TEST_ASSERT_EQUAL(3, P->query_gaps(NULL, 0, 10000, out, rs, 8));
}

void test_gaps_trim(void)
{
    for (int i = 0; i < 1100; i++) put_gap(10000 + i * 10, 10005 + i * 10, HY_GAP_OFF);
    TEST_ASSERT_EQUAL(1100, gap_lines());
    put_gap(10000 + 1100 * 10, 10005 + 1100 * 10, HY_GAP_LOST);   /* 1101st line */
    TEST_ASSERT_EQUAL(1000, gap_lines());
    static hy_gap_t out[1100];
    static uint8_t rs[1100];
    TEST_ASSERT_EQUAL(1000, P->query_gaps(NULL, 0, 1000000, out, rs, 1100));
    TEST_ASSERT_EQUAL_INT64(10000 + 101 * 10, out[0].start);        /* oldest 101 dropped */
    TEST_ASSERT_EQUAL_INT64(10000 + 1100 * 10, out[999].start);
    TEST_ASSERT_EQUAL_UINT8(HY_GAP_LOST, rs[999]);
    char p[128];
    path(p, sizeof p, "gaps.jsonl.tmp");
    TEST_ASSERT_NOT_EQUAL(0, access(p, F_OK));
}

void test_gaps_skip_corrupt(void)
{
    put_gap(1000, 2000, HY_GAP_OFF);
    char p[128];
    path(p, sizeof p, "gaps.jsonl");
    FILE *f = fopen(p, "ab");
    TEST_ASSERT_NOT_NULL(f);
    fputs("garbage\n{\"s\":9,\"e\":3,\"r\":\"off\"}\n", f);
    fclose(f);
    put_gap(3000, 4000, HY_GAP_CLOCK);
    hy_gap_t out[8];
    uint8_t rs[8];
    size_t before = hy_store_skipped_lines();
    TEST_ASSERT_EQUAL(2, P->query_gaps(NULL, 0, 10000, out, rs, 8));
    TEST_ASSERT_EQUAL(before + 2, hy_store_skipped_lines());
    TEST_ASSERT_EQUAL_INT64(3000, out[1].start);
}

/* ---- import staging ---- */

static size_t fake_free;
static size_t free_fn(void) { return fake_free; }

/* Old data: A in meds.json (next id 5), entries in Sep and Oct, one gap. */
static void old_data(void)
{
    hy_med_t m;
    hy_med_defaults(&m);
    m.id = 1; strcpy(m.code, "A"); strcpy(m.name, "Antibiotic"); m.times[0] = 480; m.n_times = 1;
    TEST_ASSERT_EQUAL(HY_OK, P->save_meds_next(NULL, &m, 1, 5));
    TEST_ASSERT_EQUAL(HY_OK, add(SEP, 1, NULL));
    TEST_ASSERT_EQUAL(HY_OK, add(OCT, 1, NULL));
    hy_gap_t g = { 1000, 2000 };
    TEST_ASSERT_EQUAL(HY_OK, P->append_gap(NULL, &g, HY_GAP_OFF));
}

/* New data: V id 7 (next id 9), entries 40 (Nov) and 41 (Sep), one gap. */
static void stage_new(void)
{
    hy_med_t m;
    hy_med_defaults(&m);
    m.id = 7; strcpy(m.code, "V"); strcpy(m.name, "Vitamin D"); m.times[0] = 480; m.n_times = 1;
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_meds(&m, 1, 9));
    hy_log_t e[2] = { mk(NOV, 7), mk(SEP + 60, 7) };
    e[0].id = 40; e[1].id = 41;
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_log(e, 2));
    hy_gap_t g = { 5000, 6000 };
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_gap(&g, HY_GAP_LOST));
}

static void assert_new_data(void)
{
    hy_med_t got[4];
    size_t n = 0;
    TEST_ASSERT_EQUAL(HY_OK, P->load_meds(NULL, got, 4, &n));
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("V", got[0].code);
    TEST_ASSERT_EQUAL_UINT32(9, P->load_next_id(NULL));
    hy_log_t out[8];
    TEST_ASSERT_EQUAL(2, q(0, NOV + DAY, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(41, out[0].id);
    TEST_ASSERT_EQUAL_UINT32(40, out[1].id);
    TEST_ASSERT_EQUAL_UINT32(42, hy_store_next_log_id());
    hy_gap_t g[4];
    uint8_t rs[4];
    TEST_ASSERT_EQUAL(1, P->query_gaps(NULL, 0, 10000, g, rs, 4));
    TEST_ASSERT_EQUAL_INT64(5000, g[0].start);
    TEST_ASSERT_EQUAL_UINT8(HY_GAP_LOST, rs[0]);
    TEST_ASSERT_FALSE(exists("import.tmp"));
}

void test_next_log_id(void)
{
    TEST_ASSERT_EQUAL_UINT32(1, hy_store_next_log_id());
    TEST_ASSERT_EQUAL(HY_OK, add(OCT, 1, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, hy_store_next_log_id());
}

void test_import_replace_commit(void)
{
    old_data();
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_begin(1000));
    TEST_ASSERT_TRUE(exists("import.tmp/log"));
    stage_new();
    /* the live store is untouched until the commit */
    hy_log_t out[8];
    TEST_ASSERT_EQUAL(2, q(0, NOV + DAY, 0, out, 8));
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_commit());
    assert_new_data();
    /* the old data sits in pre-import/ */
    TEST_ASSERT_TRUE(exists("pre-import/meds.json"));
    TEST_ASSERT_TRUE(exists("pre-import/gaps.jsonl"));
    TEST_ASSERT_TRUE(exists("pre-import/log/2026-09.jsonl"));
    TEST_ASSERT_TRUE(exists("pre-import/log/2026-10.jsonl"));
    TEST_ASSERT_FALSE(exists("log/2026-10.jsonl"));
    /* appends go on from the imported ids */
    uint32_t id = 0;
    TEST_ASSERT_EQUAL(HY_OK, add(OCT, 7, &id));
    TEST_ASSERT_EQUAL_UINT32(42, id);
    /* survives reopen */
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    TEST_ASSERT_EQUAL(3, q(0, NOV + DAY, 0, out, 8));
    /* a second import replaces the older pre-import */
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_begin(10));
    hy_med_t none[1];
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_meds(none, 0, 1));
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_commit());
    TEST_ASSERT_TRUE(exists("pre-import/log/2026-11.jsonl"));
    char pm[160], txt[512] = "";
    path(pm, sizeof pm, "pre-import/meds.json");
    FILE *f = fopen(pm, "rb");
    TEST_ASSERT_NOT_NULL(f);
    size_t got_n = fread(txt, 1, sizeof txt - 1, f);
    fclose(f);
    txt[got_n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(txt, "Vitamin D"));   /* the first import's data, not the original */
    TEST_ASSERT_EQUAL(0, q(0, NOV + DAY, 0, out, 8));
    TEST_ASSERT_EQUAL_UINT32(1, hy_store_next_log_id());
}

void test_import_abort_keeps_old(void)
{
    old_data();
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_begin(1000));
    stage_new();
    hy_store_import_abort();
    TEST_ASSERT_FALSE(exists("import.tmp"));
    TEST_ASSERT_FALSE(exists("pre-import"));
    hy_log_t out[8];
    TEST_ASSERT_EQUAL(2, q(0, NOV + DAY, 0, out, 8));
    hy_med_t got[4];
    size_t n = 0;
    TEST_ASSERT_EQUAL(HY_OK, P->load_meds(NULL, got, 4, &n));
    TEST_ASSERT_EQUAL_STRING("A", got[0].code);
    /* nothing staged any more */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_store_import_commit());
    hy_log_t e = mk(OCT, 1);
    e.id = 3;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_store_import_log(&e, 1));
    /* a staged entry without an id is refused */
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_begin(10));
    e.id = 0;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_store_import_log(&e, 1));
    hy_store_import_abort();
}

void test_import_space_check(void)
{
    hy_store_set_free_fn(free_fn);
    fake_free = 1999;
    TEST_ASSERT_EQUAL(HY_E_FULL, hy_store_import_begin(1000));
    TEST_ASSERT_FALSE(exists("import.tmp"));
    fake_free = 2000;
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_begin(1000));
    hy_store_import_abort();
    hy_store_set_free_fn(NULL);
}

void test_import_staging_without_marker_dropped_at_open(void)
{
    old_data();
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_begin(1000));
    stage_new();
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));   /* a reset before the commit */
    TEST_ASSERT_FALSE(exists("import.tmp"));
    hy_log_t out[8];
    TEST_ASSERT_EQUAL(2, q(0, NOV + DAY, 0, out, 8));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_store_import_commit());
}

void test_import_interrupted_commit_finished_at_open(void)
{
    old_data();
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_begin(1000));
    stage_new();
    /* a reset after the marker and the first moves: meds.json is in pre-import/, the staged one not yet in */
    char a[160], b[160];
    path(a, sizeof a, "pre-import");
    TEST_ASSERT_EQUAL(0, mkdir(a, 0775));
    path(a, sizeof a, "meds.json");
    path(b, sizeof b, "pre-import/meds.json");
    TEST_ASSERT_EQUAL(0, rename(a, b));
    write_raw("import.tmp/commit", "1\n");
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
    assert_new_data();
    TEST_ASSERT_TRUE(exists("pre-import/meds.json"));
    TEST_ASSERT_TRUE(exists("pre-import/log/2026-10.jsonl"));
    TEST_ASSERT_TRUE(exists("pre-import/gaps.jsonl"));
}

void test_import_begin_keeps_pending_commit(void)
{
    old_data();
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_begin(1000));
    stage_new();
    /* a commit that stopped after its marker, with a rename blocked by an item already in pre-import */
    char a[160];
    path(a, sizeof a, "pre-import");
    TEST_ASSERT_EQUAL(0, mkdir(a, 0775));
    write_raw("pre-import/meds.json", "blocker");
    write_raw("import.tmp/commit", "1\n");
    TEST_ASSERT_TRUE(hy_store_import_pending());
    /* begin neither deletes the marker nor the staged data: it refuses */
    TEST_ASSERT_EQUAL(HY_E_IO, hy_store_import_begin(10));
    TEST_ASSERT_TRUE(exists("import.tmp/commit"));
    TEST_ASSERT_TRUE(exists("import.tmp/meds.json"));
    TEST_ASSERT_TRUE(exists("pre-import/meds.json"));
    TEST_ASSERT_TRUE(exists("meds.json"));
    hy_store_import_abort();
    TEST_ASSERT_TRUE(exists("import.tmp/commit"));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_store_import_commit());
    /* once the blocker is gone the next begin finishes that commit, then stages afresh */
    path(a, sizeof a, "pre-import/meds.json");
    TEST_ASSERT_EQUAL(0, unlink(a));
    TEST_ASSERT_EQUAL(HY_OK, hy_store_import_begin(10));
    TEST_ASSERT_FALSE(hy_store_import_pending());
    hy_store_import_abort();
    assert_new_data();
    TEST_ASSERT_TRUE(exists("pre-import/meds.json"));
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
    RUN_TEST(test_ids_unique_across_months_after_reopen);
    RUN_TEST(test_ids_start_at_one_on_empty_store);
    RUN_TEST(test_open_missing_base_fails);
    RUN_TEST(test_month_index_ignores_strays);
    RUN_TEST(test_query_across_missing_months);
    RUN_TEST(test_new_month_after_open_is_indexed);
    RUN_TEST(test_update_moves_into_month_without_file);
    RUN_TEST(test_id_exhaustion);
    RUN_TEST(test_id_exhaustion_at_uint32_max);
    RUN_TEST(test_open_with_too_many_months_is_full);
    RUN_TEST(test_append_to_new_month_when_index_full);
    RUN_TEST(test_update_into_new_month_when_index_full);
    RUN_TEST(test_purge_med);
    RUN_TEST(test_next_id_port);
    RUN_TEST(test_gaps_append_query);
    RUN_TEST(test_gaps_trim);
    RUN_TEST(test_gaps_skip_corrupt);
    RUN_TEST(test_next_log_id);
    RUN_TEST(test_import_replace_commit);
    RUN_TEST(test_import_abort_keeps_old);
    RUN_TEST(test_import_space_check);
    RUN_TEST(test_import_staging_without_marker_dropped_at_open);
    RUN_TEST(test_import_interrupted_commit_finished_at_open);
    RUN_TEST(test_import_begin_keeps_pending_commit);
    return UNITY_END();
}
