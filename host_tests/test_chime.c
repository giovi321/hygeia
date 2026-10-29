#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "unity.h"
#include "hy/chime.h"

void setUp(void) {}
void tearDown(void) {}

static const double FREQ[3] = {880.0, 1109.0, 1319.0};

static size_t ms(unsigned v) { return (size_t)llround(v / 1000.0 * HY_CHIME_RATE); }

static void test_chime_len(void)
{
    size_t expect = ms(30) + 3 * ms(180) + 2 * ms(70) + ms(30);
    TEST_ASSERT_EQUAL_UINT32(expect, hy_chime_len(HY_CHIME_RATE));
}

static void test_chime_shape(void)
{
    size_t n = hy_chime_len(HY_CHIME_RATE);
    int16_t *b = malloc(n * sizeof *b);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT32(n, hy_chime_render(b, n, HY_CHIME_RATE));

    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        int a = abs(b[i]);
        if (a > peak) peak = a;
    }
    TEST_ASSERT_TRUE(peak <= 16384);
    TEST_ASSERT_TRUE(peak >= 15000);

    for (size_t i = 0; i < 600; i++) {
        TEST_ASSERT_EQUAL_INT16(0, b[i]);
        TEST_ASSERT_EQUAL_INT16(0, b[n - 1 - i]);
    }

    size_t pos = ms(30);
    for (int t = 0; t < 3; t++) {
        size_t len = ms(180);
        int cross = 0;
        for (size_t i = pos + 1; i < pos + len; i++) {
            if ((b[i - 1] < 0 && b[i] >= 0) || (b[i - 1] >= 0 && b[i] < 0)) cross++;
        }
        double f = cross / 2.0 / ((double)len / HY_CHIME_RATE);
        char msg[64];
        snprintf(msg, sizeof msg, "tone %d measured %.1f Hz, expected %.0f", t, f, FREQ[t]);
        TEST_ASSERT_TRUE_MESSAGE(fabs(f - FREQ[t]) <= FREQ[t] * 0.03, msg);
        pos += len;
        if (t < 2) {
            size_t gap = ms(70);
            double sum = 0;
            for (size_t i = pos; i < pos + gap; i++) sum += (double)b[i] * b[i];
            TEST_ASSERT_TRUE(sqrt(sum / gap) < 1.0);
            pos += gap;
        }
    }
    free(b);
}

static void test_chime_small_buffer(void)
{
    size_t n = hy_chime_len(HY_CHIME_RATE);
    int16_t *b = malloc(n * sizeof *b);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT32(0, hy_chime_render(b, n - 1, HY_CHIME_RATE));
    free(b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_chime_len);
    RUN_TEST(test_chime_shape);
    RUN_TEST(test_chime_small_buffer);
    return UNITY_END();
}
