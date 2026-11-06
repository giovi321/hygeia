#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "unity.h"
#include "hy/hook.h"
#include "hy/outbox.h"

#define NOW 1793145600LL
#define DAY 86400LL

static char base[64];
static char odir[96];
static int64_t fake_now;
static hy_ob_rec_t rec; /* static: the record is large */

static int64_t mono(void) { return fake_now; }

static void rmtree(void)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    (void)system(cmd);
}

void setUp(void)
{
    strcpy(base, "/tmp/hy_ob_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(base));
    snprintf(odir, sizeof odir, "%s/outbox", base);
    fake_now = 1000;
}
void tearDown(void) { rmtree(); }

/* Fresh outbox: file backend in a clean directory, or the RAM backend. */
static hy_ob_t *op(bool file)
{
    hy_ob_t *ob = NULL;
    if (file) {
        char cmd[160];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", odir);
        (void)system(cmd);
    }
    TEST_ASSERT_EQUAL(HY_OK, hy_ob_open(file ? odir : NULL, mono, &ob));
    TEST_ASSERT_NOT_NULL(ob);
    return ob;
}

static hy_ob_t *reopen(void)
{
    hy_ob_t *ob = NULL;
    TEST_ASSERT_EQUAL(HY_OK, hy_ob_open(odir, mono, &ob));
    return ob;
}

static uint32_t add(hy_ob_t *ob, uint8_t mask, unsigned i)
{
    char p[48];
    uint32_t seq = 0xFFFFFFFFu;
    snprintf(p, sizeof p, "{\"i\":%u}", i);
    TEST_ASSERT_EQUAL(HY_OK, hy_ob_append(ob, NOW, mask, p, &seq));
    return seq;
}

static void deliver(hy_ob_t *ob, uint8_t d, uint32_t want)
{
    TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, d, &rec));
    TEST_ASSERT_EQUAL_UINT32(want, rec.seq);
    hy_ob_done(ob, d, rec.seq, HY_OBO_DELIVERED);
}

static bool exists(const char *name)
{
    char p[160];
    snprintf(p, sizeof p, "%s/%s", odir, name);
    return access(p, F_OK) == 0;
}

static size_t count_files(const char *dir)
{
    size_t n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    TEST_ASSERT_NOT_NULL(d);
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    closedir(d);
    return n;
}

static void test_ob_append_peek_in_order(void)
{
    for (int f = 0; f < 2; f++) {
        hy_ob_t *ob = op(f);
        for (unsigned i = 0; i < 3; i++) TEST_ASSERT_EQUAL_UINT32(i, add(ob, 0x03, i));
        for (uint8_t d = 0; d < 2; d++) {
            for (uint32_t s = 0; s < 3; s++) {
                TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, d, &rec));
                TEST_ASSERT_EQUAL_UINT32(s, rec.seq);
                TEST_ASSERT_EQUAL_UINT8(0x03, rec.mask);
                TEST_ASSERT_EQUAL_INT64(NOW, rec.at);
                char want[24];
                snprintf(want, sizeof want, "{\"i\":%u}", (unsigned)s);
                TEST_ASSERT_EQUAL_STRING(want, rec.payload);
                hy_ob_done(ob, d, s, HY_OBO_DELIVERED);
            }
            TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_ob_peek(ob, d, &rec));
        }
        hy_ob_close(ob);
    }
}

static void test_ob_dests_independent(void)
{
    for (int f = 0; f < 2; f++) {
        hy_ob_t *ob = op(f);
        hy_ob_stats_t st;
        for (unsigned i = 0; i < 3; i++) add(ob, 0x03, i);
        for (uint32_t s = 0; s < 3; s++) deliver(ob, 0, s);
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 1, &rec));
        TEST_ASSERT_EQUAL_UINT32(0, rec.seq);
        TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_ob_peek(ob, 2, &rec));
        hy_ob_stats(ob, &st);
        TEST_ASSERT_EQUAL_UINT32(0, st.pending[0]);
        TEST_ASSERT_EQUAL_UINT32(3, st.pending[1]);
        TEST_ASSERT_EQUAL_UINT32(0, st.pending[2]);
        hy_ob_close(ob);
    }
}

static void test_ob_mask_selects(void)
{
    for (int f = 0; f < 2; f++) {
        hy_ob_t *ob = op(f);
        add(ob, 0x02, 0);
        TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_ob_peek(ob, 0, &rec));
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 1, &rec));
        add(ob, 0x01, 1);
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 0, &rec));
        TEST_ASSERT_EQUAL_UINT32(1, rec.seq);
        hy_ob_close(ob);
    }
}

static void test_ob_capacity_drops_oldest(void)
{
    for (int f = 0; f < 2; f++) {
        hy_ob_t *ob = op(f);
        hy_ob_stats_t st;
        for (unsigned i = 0; i < 1005; i++) add(ob, 0x01, i);
        hy_ob_stats(ob, &st);
        TEST_ASSERT_EQUAL_UINT32(1000, st.live);
        TEST_ASSERT_EQUAL_UINT32(5, st.dropped);
        TEST_ASSERT_EQUAL_UINT32(1000, st.pending[0]);
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 0, &rec));
        TEST_ASSERT_EQUAL_UINT32(5, rec.seq);
        TEST_ASSERT_EQUAL_STRING("{\"i\":5}", rec.payload);
        hy_ob_close(ob);
    }
}

static void test_ob_capacity_counts_from_oldest_pending(void)
{
    for (int f = 0; f < 2; f++) {
        hy_ob_t *ob = op(f);
        hy_ob_stats_t st;
        for (unsigned i = 0; i < 1000; i++) add(ob, 0x01, i);
        for (uint32_t s = 0; s < 10; s++) deliver(ob, 0, s);
        for (unsigned i = 1000; i < 1010; i++) add(ob, 0x01, i);
        hy_ob_stats(ob, &st);
        TEST_ASSERT_EQUAL_UINT32(0, st.dropped);
        TEST_ASSERT_EQUAL_UINT32(1000, st.live);
        hy_ob_close(ob);
    }
}

static void test_ob_expire_and_reject_counters(void)
{
    for (int f = 0; f < 2; f++) {
        hy_ob_t *ob = op(f);
        hy_ob_stats_t st;
        for (unsigned i = 0; i < 3; i++) add(ob, 0x01, i);
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 0, &rec));
        hy_ob_done(ob, 0, rec.seq, HY_OBO_EXPIRED);
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 0, &rec));
        TEST_ASSERT_EQUAL_UINT32(1, rec.seq);
        hy_ob_done(ob, 0, rec.seq, HY_OBO_REJECTED);
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 0, &rec));
        TEST_ASSERT_EQUAL_UINT32(2, rec.seq);
        /* a seq that peek did not give is ignored */
        hy_ob_done(ob, 0, 0, HY_OBO_EXPIRED);
        hy_ob_done(ob, 0, 7, HY_OBO_EXPIRED);
        hy_ob_stats(ob, &st);
        TEST_ASSERT_EQUAL_UINT32(1, st.expired);
        TEST_ASSERT_EQUAL_UINT32(1, st.rejected);
        TEST_ASSERT_EQUAL_UINT32(1, st.pending[0]);
        hy_ob_close(ob);
    }
}

static void test_ob_expiry_uses_enqueue_time(void)
{
    for (int f = 0; f < 2; f++) {
        hy_ob_t *ob = op(f);
        char p[96];
        snprintf(p, sizeof p, "{\"id\":0,\"event\":\"taken\",\"ts\":%lld}", (long long)(NOW - 30 * DAY));
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_append(ob, NOW, 0x01, p, NULL));
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 0, &rec));
        TEST_ASSERT_EQUAL_INT64(NOW, rec.at);
        TEST_ASSERT_FALSE(hy_ob_expired(rec.at, NOW, true));
        TEST_ASSERT_FALSE(hy_ob_expired(rec.at, NOW + 7 * DAY, true));
        TEST_ASSERT_TRUE(hy_ob_expired(rec.at, NOW + 7 * DAY + 1, true));
        hy_ob_close(ob);
    }
}

static void test_ob_drop_dest(void)
{
    for (int f = 0; f < 2; f++) {
        hy_ob_t *ob = op(f);
        hy_ob_stats_t st;
        for (unsigned i = 0; i < 5; i++) add(ob, 0x03, i);
        hy_ob_drop_dest(ob, 0);
        hy_ob_stats(ob, &st);
        TEST_ASSERT_EQUAL_UINT32(0, st.pending[0]);
        TEST_ASSERT_EQUAL_UINT32(5, st.pending[1]);
        TEST_ASSERT_EQUAL_UINT32(0, st.dropped);
        TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_ob_peek(ob, 0, &rec));
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 1, &rec));
        TEST_ASSERT_EQUAL_UINT32(0, rec.seq);
        /* new events for the dropped destination arrive again */
        add(ob, 0x01, 5);
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 0, &rec));
        TEST_ASSERT_EQUAL_UINT32(5, rec.seq);
        hy_ob_close(ob);
    }
}

static void test_ob_reopen_keeps_order(void)
{
    hy_ob_t *ob = op(true);
    hy_ob_stats_t st;
    for (unsigned i = 0; i < 150; i++) add(ob, 0x03, i);
    for (uint32_t s = 0; s < 20; s++) deliver(ob, 0, s);
    TEST_ASSERT_TRUE(hy_ob_flush(ob, true));
    hy_ob_close(ob);
    ob = reopen();
    TEST_ASSERT_EQUAL_UINT32(150, hy_ob_next_seq(ob));
    deliver(ob, 0, 20);
    TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 1, &rec));
    TEST_ASSERT_EQUAL_UINT32(0, rec.seq);
    hy_ob_stats(ob, &st);
    TEST_ASSERT_EQUAL_UINT32(129, st.pending[0]);
    TEST_ASSERT_EQUAL_UINT32(150, st.pending[1]);
    TEST_ASSERT_EQUAL_UINT32(2, st.segments);
    hy_ob_close(ob);
}

static void test_ob_reopen_without_flush_redelivers(void)
{
    hy_ob_t *ob = op(true);
    for (unsigned i = 0; i < 10; i++) add(ob, 0x01, i);
    for (uint32_t s = 0; s < 5; s++) deliver(ob, 0, s);
    hy_ob_t *ob2 = reopen();
    TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob2, 0, &rec));
    TEST_ASSERT_EQUAL_UINT32(0, rec.seq);
    TEST_ASSERT_EQUAL_UINT32(10, hy_ob_next_seq(ob2));
    hy_ob_close(ob2);
    hy_ob_close(ob);
}

static void test_ob_seq_survives_delete_without_flush(void)
{
    hy_ob_t *ob = op(true);
    hy_ob_stats_t st;
    for (unsigned i = 0; i < 100; i++) add(ob, 0x01, i);
    for (uint32_t s = 0; s < 100; s++) deliver(ob, 0, s);
    TEST_ASSERT_TRUE(exists("00000000.q"));
    hy_ob_t *ob2 = reopen();
    TEST_ASSERT_EQUAL_UINT32(100, hy_ob_next_seq(ob2));
    hy_ob_close(ob2); /* leaves the files as they were: nothing changed there */
    TEST_ASSERT_TRUE(exists("00000000.q"));
    /* now the flush: state first, then the segment */
    TEST_ASSERT_TRUE(hy_ob_flush(ob, false));
    TEST_ASSERT_TRUE(exists("state.json"));
    TEST_ASSERT_FALSE(exists("00000000.q"));
    hy_ob_stats(ob, &st);
    TEST_ASSERT_EQUAL_UINT32(1, st.state_writes);
    TEST_ASSERT_EQUAL_UINT32(0, st.segments);
    hy_ob_close(ob);
    ob2 = reopen();
    TEST_ASSERT_EQUAL_UINT32(100, hy_ob_next_seq(ob2));
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_ob_peek(ob2, 0, &rec));
    TEST_ASSERT_EQUAL_UINT32(100, add(ob2, 0x01, 100));
    hy_ob_close(ob2);
}

static void test_ob_torn_last_line(void)
{
    hy_ob_t *ob = op(true);
    for (unsigned i = 0; i < 3; i++) add(ob, 0x01, i);
    hy_ob_close(ob);
    char path[160];
    snprintf(path, sizeof path, "%s/00000000.q", odir);
    FILE *f = fopen(path, "a");
    TEST_ASSERT_NOT_NULL(f);
    fputs("3\t1000\t1\t0123", f);
    fclose(f);
    ob = reopen();
    hy_ob_stats_t st;
    hy_ob_stats(ob, &st);
    TEST_ASSERT_EQUAL_UINT32(3, st.pending[0]);
    TEST_ASSERT_EQUAL_UINT32(3, hy_ob_next_seq(ob));
    f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, -1, SEEK_END);
    TEST_ASSERT_EQUAL_INT('\n', fgetc(f));
    fclose(f);
    TEST_ASSERT_EQUAL_UINT32(3, add(ob, 0x01, 3));
    hy_ob_close(ob);
    ob = reopen();
    hy_ob_stats(ob, &st);
    TEST_ASSERT_EQUAL_UINT32(4, st.pending[0]);
    TEST_ASSERT_EQUAL_UINT32(0, count_files(odir) > 3 ? 1 : 0); /* no stray temp file */
    hy_ob_close(ob);
}

static void test_ob_bad_crc_line_skipped(void)
{
    hy_ob_t *ob = op(true);
    for (unsigned i = 0; i < 3; i++) add(ob, 0x01, i);
    hy_ob_close(ob);
    char path[160];
    static char buf[4096];
    snprintf(path, sizeof path, "%s/00000000.q", odir);
    FILE *f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *p = strstr(buf, "{\"i\":1}");
    TEST_ASSERT_NOT_NULL(p);
    p[5] = '7';
    f = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(buf, 1, n, f);
    fclose(f);
    ob = reopen();
    hy_ob_stats_t st;
    hy_ob_stats(ob, &st);
    TEST_ASSERT_EQUAL_UINT32(1, st.corrupt);
    TEST_ASSERT_EQUAL_UINT32(2, st.pending[0]);
    deliver(ob, 0, 0);
    deliver(ob, 0, 2);
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_ob_peek(ob, 0, &rec));
    TEST_ASSERT_EQUAL_UINT32(3, hy_ob_next_seq(ob));
    hy_ob_close(ob);
}

static void test_ob_segments_roll_and_delete(void)
{
    hy_ob_t *ob = op(true);
    hy_ob_stats_t st;
    for (unsigned i = 0; i < 250; i++) add(ob, 0x01, i);
    TEST_ASSERT_TRUE(exists("00000000.q"));
    TEST_ASSERT_TRUE(exists("00000064.q"));
    TEST_ASSERT_TRUE(exists("000000c8.q"));
    hy_ob_stats(ob, &st);
    TEST_ASSERT_EQUAL_UINT32(3, st.segments);
    for (uint32_t s = 0; s < 250; s++) deliver(ob, 0, s);
    TEST_ASSERT_TRUE(hy_ob_flush(ob, false));
    TEST_ASSERT_FALSE(exists("00000000.q"));
    TEST_ASSERT_FALSE(exists("00000064.q"));
    TEST_ASSERT_TRUE(exists("000000c8.q"));
    hy_ob_stats(ob, &st);
    TEST_ASSERT_EQUAL_UINT32(1, st.segments);
    TEST_ASSERT_EQUAL_UINT32(250, st.tail);
    TEST_ASSERT_EQUAL_UINT32(250, add(ob, 0x01, 250));
    hy_ob_close(ob);
}

static void test_ob_seq_survives_empty_reopen(void)
{
    hy_ob_t *ob = op(true);
    for (unsigned i = 0; i < 5; i++) add(ob, 0x01, i);
    for (uint32_t s = 0; s < 5; s++) deliver(ob, 0, s);
    TEST_ASSERT_TRUE(hy_ob_flush(ob, true));
    hy_ob_close(ob);
    ob = reopen();
    TEST_ASSERT_EQUAL_UINT32(5, hy_ob_next_seq(ob));
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_ob_peek(ob, 0, &rec));
    TEST_ASSERT_EQUAL_UINT32(5, add(ob, 0x01, 5));
    hy_ob_close(ob);
}

static void test_ob_state_writes_coalesced(void)
{
    for (int f = 0; f < 2; f++) {
        fake_now = 1000;
        hy_ob_t *ob = op(f);
        hy_ob_stats_t st;
        for (unsigned i = 0; i < 100; i++) add(ob, 0x01, i);
        for (uint32_t s = 0; s < 100; s++) {
            deliver(ob, 0, s);
            if (s % 11 == 10) fake_now++; /* 9 s in all */
            (void)hy_ob_flush(ob, false);
        }
        TEST_ASSERT_EQUAL_INT64(1009, fake_now);
        hy_ob_stats(ob, &st);
        TEST_ASSERT_TRUE(st.state_writes <= 1);
        uint32_t w1 = st.state_writes;
        TEST_ASSERT_EQUAL_UINT32(100, st.appends);
        add(ob, 0x01, 100);
        deliver(ob, 0, 100);
        TEST_ASSERT_FALSE(hy_ob_flush(ob, false));
        fake_now += 10;
        TEST_ASSERT_TRUE(hy_ob_flush(ob, false));
        hy_ob_stats(ob, &st);
        TEST_ASSERT_EQUAL_UINT32(w1 + 1, st.state_writes);
        TEST_ASSERT_EQUAL_UINT32(101, st.appends);
        hy_ob_close(ob);
    }
}

static void test_ob_payload_rules(void)
{
    for (int f = 0; f < 2; f++) {
        hy_ob_t *ob = op(f);
        static char big[1100];
        uint32_t seq = 77;
        TEST_ASSERT_EQUAL(HY_E_INVALID, hy_ob_append(ob, NOW, 0, "{}", &seq));
        TEST_ASSERT_EQUAL(HY_E_INVALID, hy_ob_append(ob, NOW, 0x20, "{}", &seq));
        TEST_ASSERT_EQUAL(HY_E_INVALID, hy_ob_append(ob, NOW, 1, "{\"a\":\n1}", &seq));
        TEST_ASSERT_EQUAL(HY_E_INVALID, hy_ob_append(ob, NOW, 1, "{\"a\":\t1}", &seq));
        TEST_ASSERT_EQUAL(HY_E_INVALID, hy_ob_append(ob, NOW, 1, "{\"a\":\r1}", &seq));
        TEST_ASSERT_EQUAL(HY_E_INVALID, hy_ob_append(ob, NOW, 1, "", &seq));
        memset(big, 'x', 1025);
        big[1025] = '\0';
        TEST_ASSERT_EQUAL(HY_E_INVALID, hy_ob_append(ob, NOW, 1, big, &seq));
        TEST_ASSERT_EQUAL_UINT32(77, seq);
        TEST_ASSERT_EQUAL_UINT32(0, hy_ob_next_seq(ob));
        memset(big, 'x', 1024);
        big[1024] = '\0';
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_append(ob, NOW, 1, big, &seq));
        TEST_ASSERT_EQUAL_UINT32(0, seq);
        TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 0, &rec));
        TEST_ASSERT_EQUAL_STRING(big, rec.payload);
        hy_ob_close(ob);
    }
    /* the longest payload and the largest numbers survive a reopen */
    hy_ob_t *ob = op(true);
    static char big2[1100];
    memset(big2, 'y', 1024);
    big2[1024] = '\0';
    TEST_ASSERT_EQUAL(HY_OK, hy_ob_append(ob, 9999999999LL, 0x1F, big2, NULL));
    hy_ob_close(ob);
    ob = reopen();
    TEST_ASSERT_EQUAL(HY_OK, hy_ob_peek(ob, 4, &rec));
    TEST_ASSERT_EQUAL_STRING(big2, rec.payload);
    TEST_ASSERT_EQUAL_INT64(9999999999LL, rec.at);
    TEST_ASSERT_EQUAL_UINT8(0x1F, rec.mask);
    hy_ob_close(ob);
}

static void test_ob_ram_backend_no_files(void)
{
    hy_ob_t *ob = NULL;
    TEST_ASSERT_EQUAL(HY_OK, hy_ob_open(NULL, mono, &ob));
    for (unsigned i = 0; i < 20; i++) add(ob, 0x01, i);
    for (uint32_t s = 0; s < 10; s++) deliver(ob, 0, s);
    (void)hy_ob_flush(ob, true);
    hy_ob_close(ob);
    TEST_ASSERT_EQUAL_UINT(0, count_files(base));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ob_append_peek_in_order);
    RUN_TEST(test_ob_dests_independent);
    RUN_TEST(test_ob_mask_selects);
    RUN_TEST(test_ob_capacity_drops_oldest);
    RUN_TEST(test_ob_capacity_counts_from_oldest_pending);
    RUN_TEST(test_ob_expire_and_reject_counters);
    RUN_TEST(test_ob_expiry_uses_enqueue_time);
    RUN_TEST(test_ob_drop_dest);
    RUN_TEST(test_ob_reopen_keeps_order);
    RUN_TEST(test_ob_reopen_without_flush_redelivers);
    RUN_TEST(test_ob_seq_survives_delete_without_flush);
    RUN_TEST(test_ob_torn_last_line);
    RUN_TEST(test_ob_bad_crc_line_skipped);
    RUN_TEST(test_ob_segments_roll_and_delete);
    RUN_TEST(test_ob_seq_survives_empty_reopen);
    RUN_TEST(test_ob_state_writes_coalesced);
    RUN_TEST(test_ob_payload_rules);
    RUN_TEST(test_ob_ram_backend_no_files);
    return UNITY_END();
}
