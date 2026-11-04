#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "unity.h"
#include "hy/cabinet.h"
#include "hy/cabinet_file.h"
#include "hy/store.h"

static char base[64];
static hy_cab_model_t s_m, s_d;   /* static: about 12 KB each */
static uint8_t s_buf[HY_CAB_FILE_MAX + 16];

static void fill_take(hy_cab_seq_t *q, uint16_t n, int seed, bool gyro)
{
    memset(q, 0, sizeof *q);
    q->n = n;
    q->has_gyro = gyro;
    q->rest.x = (int16_t)(seed * 3 - 40);
    q->rest.y = -1000 + seed;
    q->rest.z = (int16_t)(-seed);
    for (uint16_t i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            q->s[i].a[k] = (int16_t)((i * 7 + k * 131 + seed * 17) % 2001 - 1000);
            q->s[i].g[k] = gyro ? (int16_t)(-((i * 13 + k * 29 + seed) % 3001)) : 0;
        }
}

static void make_model(hy_cab_model_t *m, uint16_t n0, uint16_t n1, uint16_t n2, bool gyro)
{
    memset(m, 0, sizeof *m);
    fill_take(&m->take[0], n0, 1, gyro);
    fill_take(&m->take[1], n1, 2, gyro);
    fill_take(&m->take[2], n2, 3, gyro);
    m->n = 3;   /* a model from a file written with three takes */
    m->d_self = 0.4375f;
    m->trig_mg = 85;
    m->valid = true;
}

/* count takes of len samples each */
static void make_model_n(hy_cab_model_t *m, int count, uint16_t len, bool gyro)
{
    memset(m, 0, sizeof *m);
    for (int t = 0; t < count && t < HY_CAB_TAKES; t++) fill_take(&m->take[t], len, t + 1, gyro);
    m->n = (uint8_t)count;
    m->d_self = 0.4375f;
    m->trig_mg = 85;
    m->valid = true;
}

static void rmtree(void)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    (void)system(cmd);
}

void setUp(void)
{
    strcpy(base, "/tmp/hy_cab_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(base));
    TEST_ASSERT_EQUAL(HY_OK, hy_store_open(base));
}
void tearDown(void) { rmtree(); }

static void same_model(const hy_cab_model_t *a, const hy_cab_model_t *b)
{
    TEST_ASSERT_TRUE(b->valid);
    TEST_ASSERT_EQUAL_UINT16(a->trig_mg, b->trig_mg);
    TEST_ASSERT_EQUAL_FLOAT(a->d_self, b->d_self);
    TEST_ASSERT_EQUAL_UINT8(a->n, b->n);
    for (int t = 0; t < a->n; t++) {
        TEST_ASSERT_EQUAL_UINT16(a->take[t].n, b->take[t].n);
        TEST_ASSERT_EQUAL(a->take[t].has_gyro, b->take[t].has_gyro);
        TEST_ASSERT_EQUAL_INT16(a->take[t].rest.x, b->take[t].rest.x);
        TEST_ASSERT_EQUAL_INT16(a->take[t].rest.y, b->take[t].rest.y);
        TEST_ASSERT_EQUAL_INT16(a->take[t].rest.z, b->take[t].rest.z);
        TEST_ASSERT_EQUAL_MEMORY(a->take[t].s, b->take[t].s, (size_t)a->take[t].n * sizeof(hy_imu6_t));
    }
}

void test_cab_roundtrip(void)
{
    make_model(&s_m, 40, 55, 61, true);
    int64_t created = 1793145600LL;
    size_t n = hy_cab_encode(&s_m, created, s_buf, sizeof s_buf);
    TEST_ASSERT_EQUAL_UINT32(32 + 3 * 8 + 12 * (40 + 55 + 61) + 4, n);
    int64_t got = 0;
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_decode(s_buf, n, &s_d, &got));
    TEST_ASSERT_EQUAL_INT64(created, got);
    same_model(&s_m, &s_d);

    make_model(&s_m, 10, 11, 12, false);   /* has_gyro false comes back false */
    n = hy_cab_encode(&s_m, -5, s_buf, sizeof s_buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_decode(s_buf, n, &s_d, &got));
    TEST_ASSERT_EQUAL_INT64(-5, got);
    same_model(&s_m, &s_d);
}

void test_cab_max_size(void)
{
    TEST_ASSERT_EQUAL_UINT32(5, HY_CAB_TAKES);
    TEST_ASSERT_EQUAL_UINT32(12076, HY_CAB_FILE_MAX);
    make_model_n(&s_m, HY_CAB_TAKES, 200, true);
    TEST_ASSERT_EQUAL_UINT32(HY_CAB_FILE_MAX, hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf));
    TEST_ASSERT_EQUAL_UINT32(0, hy_cab_encode(&s_m, 1, s_buf, HY_CAB_FILE_MAX - 1));
    make_model_n(&s_m, HY_CAB_TAKES, 200, true);
    s_m.take[1].n = 201;   /* beyond the capture limit */
    TEST_ASSERT_EQUAL_UINT32(0, hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf));
    make_model_n(&s_m, HY_CAB_TAKES, 20, true);
    s_m.take[4].n = 0;   /* an empty take */
    TEST_ASSERT_EQUAL_UINT32(0, hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf));
    make_model(&s_m, 20, 20, 20, true);
    s_m.valid = false;
    TEST_ASSERT_EQUAL_UINT32(0, hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf));
}

/* count takes in use: 5 round-trip, 3 (an older file) too, 4 as well; fewer than 3 or more than 5 are not encoded */
void test_cab_five_takes_roundtrip(void)
{
    make_model_n(&s_m, 5, 33, true);
    size_t n = hy_cab_encode(&s_m, 1793145600LL, s_buf, sizeof s_buf);
    TEST_ASSERT_EQUAL_UINT32(32 + 5 * 8 + 12 * 5 * 33 + 4, n);
    TEST_ASSERT_EQUAL_UINT8(5, s_buf[6]);
    int64_t got = 0;
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_decode(s_buf, n, &s_d, &got));
    TEST_ASSERT_EQUAL_INT64(1793145600LL, got);
    TEST_ASSERT_EQUAL_UINT8(5, s_d.n);
    same_model(&s_m, &s_d);

    make_model_n(&s_m, 4, 33, false);
    n = hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf);
    TEST_ASSERT_EQUAL_UINT32(32 + 4 * 8 + 12 * 4 * 33 + 4, n);
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_decode(s_buf, n, &s_d, NULL));
    same_model(&s_m, &s_d);

    make_model_n(&s_m, 2, 33, true);
    TEST_ASSERT_EQUAL_UINT32(0, hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf));
    make_model_n(&s_m, 5, 33, true);
    s_m.n = 6;
    TEST_ASSERT_EQUAL_UINT32(0, hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf));
    s_m.n = 0;
    TEST_ASSERT_EQUAL_UINT32(0, hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf));
}

/* A file written with three takes (the earlier format, version 1) still decodes, with n 3 */
void test_cab_three_take_file_still_decodes(void)
{
    make_model_n(&s_m, 3, 200, true);
    size_t n = hy_cab_encode(&s_m, 7, s_buf, sizeof s_buf);
    TEST_ASSERT_EQUAL_UINT32(7260, n);   /* 32 + 3 x (8 + 200 x 12) + 4 */
    TEST_ASSERT_EQUAL_UINT8(1, s_buf[4]);
    TEST_ASSERT_EQUAL_UINT8(3, s_buf[6]);
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_decode(s_buf, n, &s_d, NULL));
    TEST_ASSERT_EQUAL_UINT8(3, s_d.n);
    same_model(&s_m, &s_d);
}

/* A 3-take file built by hand (not by hy_cab_encode): header, three takes of two samples with distinct values,
   CRC-32 over everything before it. It must decode with n 3, whatever the encoder does today. */
static const uint8_t GOLDEN_3[] = {
    0x48, 0x59, 0x43, 0x42, 0x01, 0x32, 0x03, 0x01, 0x64, 0x00, 0xDC, 0x00,
    0x00, 0x00, 0xC0, 0x3F, 0x00, 0x3B, 0xE1, 0x6A, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x0A, 0x00,
    0xEC, 0xFF, 0xE8, 0x03, 0x64, 0x00, 0x00, 0x00, 0xD4, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x65, 0x00, 0xFB, 0xFF, 0xD4, 0x03, 0x07, 0x00,
    0xFD, 0xFF, 0x00, 0x00, 0x02, 0x00, 0x0B, 0x00, 0xEC, 0xFF, 0xE9, 0x03,
    0xC8, 0x00, 0x00, 0x00, 0xD5, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0xC9, 0x00, 0xFB, 0xFF, 0xD5, 0x03, 0x07, 0x00, 0xFD, 0xFF, 0x01, 0x00,
    0x02, 0x00, 0x0C, 0x00, 0xEC, 0xFF, 0xEA, 0x03, 0x2C, 0x01, 0x00, 0x00,
    0xD6, 0x03, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x2D, 0x01, 0xFB, 0xFF,
    0xD6, 0x03, 0x07, 0x00, 0xFD, 0xFF, 0x02, 0x00, 0x45, 0x4C, 0xBB, 0x17,
};

void test_cab_golden_three_take_file_decodes(void)
{
    TEST_ASSERT_EQUAL_UINT32(132, sizeof GOLDEN_3);
    int64_t created = 0;
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_decode(GOLDEN_3, sizeof GOLDEN_3, &s_d, &created));
    TEST_ASSERT_EQUAL_INT64(1793145600LL, created);
    TEST_ASSERT_EQUAL_UINT8(3, s_d.n);
    TEST_ASSERT_EQUAL_UINT16(100, s_d.trig_mg);
    TEST_ASSERT_EQUAL_FLOAT(1.5f, s_d.d_self);
    for (int t = 0; t < 3; t++) {
        TEST_ASSERT_EQUAL_UINT16(2, s_d.take[t].n);
        TEST_ASSERT_TRUE(s_d.take[t].has_gyro);
        TEST_ASSERT_EQUAL_INT16(10 + t, s_d.take[t].rest.x);
        TEST_ASSERT_EQUAL_INT16(-20, s_d.take[t].rest.y);
        TEST_ASSERT_EQUAL_INT16(1000 + t, s_d.take[t].rest.z);
        for (int i = 0; i < 2; i++) {
            TEST_ASSERT_EQUAL_INT16(100 * (t + 1) + i, s_d.take[t].s[i].a[0]);
            TEST_ASSERT_EQUAL_INT16(-5 * i, s_d.take[t].s[i].a[1]);
            TEST_ASSERT_EQUAL_INT16(980 + t, s_d.take[t].s[i].a[2]);
            TEST_ASSERT_EQUAL_INT16(i * 7, s_d.take[t].s[i].g[0]);
            TEST_ASSERT_EQUAL_INT16(-i * 3, s_d.take[t].s[i].g[1]);
            TEST_ASSERT_EQUAL_INT16(t, s_d.take[t].s[i].g[2]);
        }
    }
    /* the encoder writes the same bytes for the same model */
    static uint8_t again[sizeof GOLDEN_3];
    TEST_ASSERT_EQUAL_UINT32(sizeof GOLDEN_3, hy_cab_encode(&s_d, created, again, sizeof again));
    TEST_ASSERT_EQUAL_MEMORY(GOLDEN_3, again, sizeof GOLDEN_3);
    /* a flipped byte fails the CRC */
    static uint8_t bad[sizeof GOLDEN_3];
    memcpy(bad, GOLDEN_3, sizeof bad);
    bad[40] ^= 1;
    TEST_ASSERT_EQUAL(HY_E_CORRUPT, hy_cab_decode(bad, sizeof bad, &s_d, NULL));
}

static void put_crc(size_t n);

/* The takes field outside 3 to HY_CAB_TAKES is refused even with a good CRC, and so is one that disagrees with the length */
void test_cab_decode_takes_field_range(void)
{
    make_model_n(&s_m, 3, 20, true);
    size_t n = hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf);
    static const uint8_t bad[] = { 0, 1, 2, 4, 5, 6, 255 };
    for (unsigned i = 0; i < sizeof bad; i++) {
        s_buf[6] = bad[i];
        put_crc(n);
        TEST_ASSERT_EQUAL_MESSAGE(HY_E_CORRUPT, hy_cab_decode(s_buf, n, &s_d, NULL), "takes field");
        TEST_ASSERT_FALSE(s_d.valid);
    }
    s_buf[6] = 3;
    put_crc(n);
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_decode(s_buf, n, &s_d, NULL));
}

static void put_crc(size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n - 4; i++) {
        c ^= s_buf[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    c = ~c;
    for (int i = 0; i < 4; i++) s_buf[n - 4 + i] = (uint8_t)(c >> (8 * i));
}

void test_cab_crc_known_value(void)
{
    /* The stored CRC equals the test's own bitwise CRC-32, which gives the IEEE check value for "123456789". */
    make_model(&s_m, 20, 20, 20, true);
    size_t n = hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf);
    uint8_t keep[4];
    memcpy(keep, s_buf + n - 4, 4);
    put_crc(n);
    TEST_ASSERT_EQUAL_MEMORY(keep, s_buf + n - 4, 4);
    static const uint8_t v[] = "123456789";
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < 9; i++) {
        c ^= v[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, ~c);
}

void test_cab_decode_bad_crc(void)
{
    make_model(&s_m, 30, 30, 30, true);
    size_t n = hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf);
    s_buf[40] ^= 0x01;   /* one flipped bit in a sample */
    TEST_ASSERT_EQUAL(HY_E_CORRUPT, hy_cab_decode(s_buf, n, &s_d, NULL));
    TEST_ASSERT_FALSE(s_d.valid);
    s_buf[40] ^= 0x01;
    s_buf[n - 1] ^= 0x80;   /* flipped bit in the CRC */
    TEST_ASSERT_EQUAL(HY_E_CORRUPT, hy_cab_decode(s_buf, n, &s_d, NULL));
    s_buf[n - 1] ^= 0x80;
    s_buf[0] = 'X';   /* bad magic */
    TEST_ASSERT_EQUAL(HY_E_CORRUPT, hy_cab_decode(s_buf, n, &s_d, NULL));
    s_buf[0] = 'H';
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_decode(s_buf, n, &s_d, NULL));
}

void test_cab_decode_bad_version(void)
{
    make_model(&s_m, 30, 30, 30, true);
    size_t n = hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf);
    s_buf[4] = 2;
    put_crc(n);   /* a valid file of another version */
    TEST_ASSERT_EQUAL(HY_E_VERSION, hy_cab_decode(s_buf, n, &s_d, NULL));
    TEST_ASSERT_FALSE(s_d.valid);
}

void test_cab_decode_truncated(void)
{
    make_model(&s_m, 25, 26, 27, true);
    size_t n = hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf);
    /* A copy at the very end of a heap block, so a read past len shows up under a sanitizer or as a crash. */
    for (size_t len = 0; len < n; len++) {
        uint8_t *tight = malloc(len ? len : 1);
        TEST_ASSERT_NOT_NULL(tight);
        memcpy(tight, s_buf, len);
        TEST_ASSERT_EQUAL_MESSAGE(HY_E_CORRUPT, hy_cab_decode(tight, len, &s_d, NULL), "truncated");
        TEST_ASSERT_FALSE(s_d.valid);
        free(tight);
    }
    TEST_ASSERT_EQUAL(HY_E_CORRUPT, hy_cab_decode(s_buf, n + 1, &s_d, NULL));   /* trailing byte */
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_decode(s_buf, n, &s_d, NULL));
}

void test_cab_decode_bad_count(void)
{
    make_model(&s_m, 30, 30, 30, true);
    size_t n = hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf);
    s_buf[32] = 31;   /* take 0 claims 31 samples, with a valid CRC */
    put_crc(n);
    TEST_ASSERT_EQUAL(HY_E_CORRUPT, hy_cab_decode(s_buf, n, &s_d, NULL));
}

/* A file with a good CRC but a trigger or d_self no teach can produce is refused. */
void test_cab_decode_bad_trigger_or_dself(void)
{
    static const uint16_t trig[] = { 0, HY_CAB_TRIG_MIN_MG - 1, HY_CAB_TRIG_MAX_MG + 1, 65535 };
    for (unsigned i = 0; i < sizeof trig / sizeof trig[0]; i++) {
        make_model(&s_m, 20, 20, 20, true);
        s_m.trig_mg = trig[i];
        size_t n = hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf);
        TEST_ASSERT_TRUE(n > 0);
        TEST_ASSERT_EQUAL(HY_E_CORRUPT, hy_cab_decode(s_buf, n, &s_d, NULL));
        TEST_ASSERT_FALSE(s_d.valid);
    }
    static const float dself[] = { 0.0f, -1.0f, (float)(1.0 / 0.0), (float)(0.0 / 0.0) };
    for (unsigned i = 0; i < sizeof dself / sizeof dself[0]; i++) {
        make_model(&s_m, 20, 20, 20, true);
        s_m.d_self = dself[i];
        size_t n = hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf);
        TEST_ASSERT_TRUE(n > 0);
        TEST_ASSERT_EQUAL(HY_E_CORRUPT, hy_cab_decode(s_buf, n, &s_d, NULL));
    }
    make_model(&s_m, 20, 20, 20, true);
    s_m.trig_mg = HY_CAB_TRIG_MIN_MG;
    size_t n = hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf);
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_decode(s_buf, n, &s_d, NULL));
    s_m.trig_mg = HY_CAB_TRIG_MAX_MG;
    n = hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf);
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_decode(s_buf, n, &s_d, NULL));
}

/* The file has one gyro flag: takes that disagree are not encoded (take 0's flag is never used for all). */
void test_cab_encode_refuses_mixed_gyro(void)
{
    make_model(&s_m, 20, 20, 20, true);
    s_m.take[1].has_gyro = false;
    TEST_ASSERT_EQUAL_UINT32(0, hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf));
    make_model(&s_m, 20, 20, 20, false);
    s_m.take[2].has_gyro = true;
    TEST_ASSERT_EQUAL_UINT32(0, hy_cab_encode(&s_m, 1, s_buf, sizeof s_buf));
}

void test_cab_csv_rows(void)
{
    hy_cab_seq_t q;
    memset(&q, 0, sizeof q);
    q.n = 2;
    q.s[0] = (hy_imu6_t){ { 12, -1000, 5 }, { 0, 0, 0 } };
    q.s[1] = (hy_imu6_t){ { -30, -985, -7 }, { 15, -240, 3 } };
    char out[128];
    size_t n = hy_cab_csv_rows(&q, "teach", 2, out, sizeof out);
    const char *want = "teach,2,0,12,-1000,5,0,0,0\nteach,2,20,-30,-985,-7,15,-240,3\n";
    TEST_ASSERT_EQUAL_UINT32(strlen(want), n);
    TEST_ASSERT_EQUAL_STRING(want, out);
    TEST_ASSERT_EQUAL_UINT32(0, hy_cab_csv_rows(&q, "teach", 2, out, strlen(want)));   /* no room for the NUL */
    TEST_ASSERT_EQUAL_UINT32(strlen(want), hy_cab_csv_rows(&q, "teach", 2, out, strlen(want) + 1));
    TEST_ASSERT_EQUAL_STRING("kind,take,t_ms,ax,ay,az,gx,gy,gz\n", HY_CAB_CSV_HEADER);
}

static void path_of(const char *rel, char *out, size_t n) { snprintf(out, n, "%s/%s", base, rel); }

static void write_junk(const char *rel)
{
    char p[128];
    path_of(rel, p, sizeof p);
    FILE *f = fopen(p, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs("half a template", f);
    fclose(f);
}

static bool exists(const char *rel)
{
    char p[128];
    struct stat st;
    path_of(rel, p, sizeof p);
    return stat(p, &st) == 0;
}

void test_cab_store_atomic(void)
{
    int64_t created = 0;
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_cab_file_load(&s_d, &created));

    /* a save cut before the rename leaves only a temp file: still "not taught" */
    write_junk("cabinet.bin.tmp");
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_cab_file_load(&s_d, &created));

    make_model(&s_m, 40, 50, 60, true);
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_file_save(&s_m, 111));
    TEST_ASSERT_FALSE(exists("cabinet.bin.tmp"));   /* the save replaced the leftover */
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_file_load(&s_d, &created));
    TEST_ASSERT_EQUAL_INT64(111, created);
    same_model(&s_m, &s_d);

    /* another cut save: the old file still loads */
    write_junk("cabinet.bin.tmp");
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_file_load(&s_d, &created));
    TEST_ASSERT_EQUAL_INT64(111, created);
    same_model(&s_m, &s_d);

    /* the next save replaces both */
    make_model_n(&s_m, HY_CAB_TAKES, 100, true);   /* five takes through the file */
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_file_save(&s_m, 222));
    TEST_ASSERT_FALSE(exists("cabinet.bin.tmp"));
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_file_load(&s_d, &created));
    TEST_ASSERT_EQUAL_INT64(222, created);
    same_model(&s_m, &s_d);

    /* a bad file is reported and left in place; a new save replaces it */
    write_junk("cabinet.bin");
    TEST_ASSERT_EQUAL(HY_E_CORRUPT, hy_cab_file_load(&s_d, &created));
    TEST_ASSERT_TRUE(exists("cabinet.bin"));
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_file_save(&s_m, 333));
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_file_load(&s_d, &created));
    TEST_ASSERT_EQUAL_INT64(333, created);

    /* an invalid model is refused and the file stays */
    s_m.valid = false;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_cab_file_save(&s_m, 444));
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_file_load(&s_d, &created));
    TEST_ASSERT_EQUAL_INT64(333, created);

    write_junk("cabinet.bin.tmp");
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_file_delete());
    TEST_ASSERT_FALSE(exists("cabinet.bin"));
    TEST_ASSERT_FALSE(exists("cabinet.bin.tmp"));
    TEST_ASSERT_EQUAL(HY_E_NOT_FOUND, hy_cab_file_load(&s_d, &created));
    TEST_ASSERT_EQUAL(HY_OK, hy_cab_file_delete());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cab_roundtrip);
    RUN_TEST(test_cab_max_size);
    RUN_TEST(test_cab_five_takes_roundtrip);
    RUN_TEST(test_cab_three_take_file_still_decodes);
    RUN_TEST(test_cab_golden_three_take_file_decodes);
    RUN_TEST(test_cab_decode_takes_field_range);
    RUN_TEST(test_cab_crc_known_value);
    RUN_TEST(test_cab_decode_bad_crc);
    RUN_TEST(test_cab_decode_bad_version);
    RUN_TEST(test_cab_decode_truncated);
    RUN_TEST(test_cab_decode_bad_count);
    RUN_TEST(test_cab_decode_bad_trigger_or_dself);
    RUN_TEST(test_cab_encode_refuses_mixed_gyro);
    RUN_TEST(test_cab_csv_rows);
    RUN_TEST(test_cab_store_atomic);
    return UNITY_END();
}
