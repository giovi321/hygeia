#include "unity.h"
#include <string.h>
#include "hy/tzdb.h"

void setUp(void) {}
void tearDown(void) {}

static void test_lookup_known(void)
{
    TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", hy_tzdb_posix("Europe/Zurich"));
    TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", hy_tzdb_posix("America/New_York"));
    TEST_ASSERT_NOT_NULL(hy_tzdb_posix("Etc/UTC"));
    TEST_ASSERT_NULL(hy_tzdb_posix("Nowhere/City"));
    TEST_ASSERT_NULL(hy_tzdb_posix(""));
}

static void test_sorted_and_count(void)
{
    TEST_ASSERT_GREATER_THAN_size_t(300, hy_tzdb_count());
    const char *prev = NULL;
    for (size_t i = 0; i < hy_tzdb_count(); i++) {
        const char *n = NULL, *p = NULL;
        TEST_ASSERT_TRUE(hy_tzdb_at(i, &n, &p));
        TEST_ASSERT_NOT_NULL(n);
        TEST_ASSERT_NOT_NULL(p);
        if (prev) TEST_ASSERT_TRUE(strcmp(prev, n) < 0);
        prev = n;
    }
    const char *n, *p;
    TEST_ASSERT_FALSE(hy_tzdb_at(hy_tzdb_count(), &n, &p));
}

static void test_regions(void)
{
    const char *r[32];
    size_t n = hy_tzdb_regions(r, 32);
    TEST_ASSERT_TRUE(n > 5 && n < 32);
    int europe = 0, america = 0;
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(r[i], "Europe")) europe++;
        if (!strcmp(r[i], "America")) america++;
        for (size_t j = i + 1; j < n; j++) TEST_ASSERT_TRUE(strcmp(r[i], r[j]) != 0);
    }
    TEST_ASSERT_EQUAL_INT(1, europe);
    TEST_ASSERT_EQUAL_INT(1, america);
    TEST_ASSERT_EQUAL_size_t(2, hy_tzdb_regions(r, 2));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lookup_known);
    RUN_TEST(test_sorted_and_count);
    RUN_TEST(test_regions);
    return UNITY_END();
}
