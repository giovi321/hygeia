#include <string.h>

#include "unity.h"
#include "hy/dns.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t IPB[4] = {192, 168, 4, 1};

static uint32_t ip_be(void)
{
    uint32_t v;
    memcpy(&v, IPB, 4); /* bytes in memory (network) order */
    return v;
}

/* Header (id 0xBEEF, RD, QDCOUNT 1) + hygeia.local + QTYPE + QCLASS. Returns the length. */
static size_t build(uint8_t *p, uint16_t qtype, uint16_t qclass)
{
    static const uint8_t hdr[12] = {0xBE, 0xEF, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    static const uint8_t name[] = {6, 'h', 'y', 'g', 'e', 'i', 'a', 5, 'l', 'o', 'c', 'a', 'l', 0};
    memcpy(p, hdr, 12);
    memcpy(p + 12, name, sizeof name);
    size_t n = 12 + sizeof name;
    p[n++] = (uint8_t)(qtype >> 8); p[n++] = (uint8_t)qtype;
    p[n++] = (uint8_t)(qclass >> 8); p[n++] = (uint8_t)qclass;
    return n;
}

static void test_dns_a_query(void)
{
    uint8_t q[64], out[512];
    size_t ql = build(q, 1, 1);
    size_t n = hy_dns_answer(q, ql, ip_be(), out, sizeof out);
    TEST_ASSERT_EQUAL_UINT(ql + 16, n);
    TEST_ASSERT_EQUAL_HEX8(0xBE, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, out[1]);
    TEST_ASSERT_TRUE(out[2] & 0x80);                 /* QR */
    TEST_ASSERT_TRUE(out[2] & 0x04);                 /* AA */
    TEST_ASSERT_EQUAL_HEX8(0x80, out[3] & 0x8F);     /* RA, RCODE 0 */
    TEST_ASSERT_EQUAL_UINT8(0, out[4]); TEST_ASSERT_EQUAL_UINT8(1, out[5]);   /* QDCOUNT */
    TEST_ASSERT_EQUAL_UINT8(0, out[6]); TEST_ASSERT_EQUAL_UINT8(1, out[7]);   /* ANCOUNT */
    TEST_ASSERT_EQUAL_UINT8(0, out[8]); TEST_ASSERT_EQUAL_UINT8(0, out[9]);   /* NSCOUNT */
    TEST_ASSERT_EQUAL_UINT8(0, out[10]); TEST_ASSERT_EQUAL_UINT8(0, out[11]); /* ARCOUNT */
    TEST_ASSERT_EQUAL_MEMORY(q + 12, out + 12, ql - 12);
    const uint8_t want[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
    TEST_ASSERT_EQUAL_MEMORY(want, out + ql, 16);
}

static void test_dns_any_and_class(void)
{
    uint8_t q[64], out[512];
    size_t ql = build(q, 255, 1);
    TEST_ASSERT_EQUAL_UINT(ql + 16, hy_dns_answer(q, ql, ip_be(), out, sizeof out));
    TEST_ASSERT_EQUAL_UINT8(1, out[7]);
    ql = build(q, 1, 3); /* class CH: no answer, not a drop */
    TEST_ASSERT_EQUAL_UINT(ql, hy_dns_answer(q, ql, ip_be(), out, sizeof out));
    TEST_ASSERT_EQUAL_UINT8(0, out[7]);
}

static void test_dns_additional_ignored(void)
{
    uint8_t q[80], out[512];
    size_t ql = build(q, 1, 1);
    q[11] = 1; /* ARCOUNT 1: EDNS OPT */
    const uint8_t opt[11] = {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0};
    memcpy(q + ql, opt, sizeof opt);
    size_t n = hy_dns_answer(q, ql + sizeof opt, ip_be(), out, sizeof out);
    TEST_ASSERT_EQUAL_UINT(ql + 16, n);
    TEST_ASSERT_EQUAL_UINT8(0, out[11]);
}

static void test_dns_aaaa_no_answer(void)
{
    uint8_t q[64], out[512];
    size_t ql = build(q, 28, 1);
    size_t n = hy_dns_answer(q, ql, ip_be(), out, sizeof out);
    TEST_ASSERT_EQUAL_UINT(ql, n);
    TEST_ASSERT_EQUAL_UINT8(0, out[6]); TEST_ASSERT_EQUAL_UINT8(0, out[7]);
    TEST_ASSERT_EQUAL_HEX8(0, out[3] & 0x0F);
    TEST_ASSERT_TRUE(out[2] & 0x80);
}

static void test_dns_drop_response(void)
{
    uint8_t q[64], out[512];
    size_t ql = build(q, 1, 1);
    q[2] |= 0x80;
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, ql, ip_be(), out, sizeof out));
}

static void test_dns_drop_opcode(void)
{
    uint8_t q[64], out[512];
    size_t ql = build(q, 1, 1);
    q[2] |= 0x08; /* opcode 1 */
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, ql, ip_be(), out, sizeof out));
}

static void test_dns_drop_two_questions(void)
{
    uint8_t q[64], out[512];
    size_t ql = build(q, 1, 1);
    q[5] = 2;
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, ql, ip_be(), out, sizeof out));
    q[5] = 0;
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, ql, ip_be(), out, sizeof out));
}

static void test_dns_compressed_question(void)
{
    uint8_t q[64], out[512];
    size_t ql = build(q, 1, 1);
    q[12] = 0xC0;
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, ql, ip_be(), out, sizeof out));
    q[12] = 0x40;
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, ql, ip_be(), out, sizeof out));
    q[12] = 0x80;
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, ql, ip_be(), out, sizeof out));
}

static void test_dns_truncated(void)
{
    uint8_t q[64], out[512];
    size_t ql = build(q, 1, 1);
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, 12 + 4, ip_be(), out, sizeof out)); /* mid-label */
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, ql - 1, ip_be(), out, sizeof out)); /* QCLASS cut */
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, 11, ip_be(), out, sizeof out));
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, 12, ip_be(), out, sizeof out));
}

static void test_dns_long_label(void)
{
    uint8_t q[128], out[512];
    memset(q, 0, sizeof q);
    q[0] = 1; q[2] = 0x01; q[5] = 1;
    q[12] = 64;
    memset(q + 13, 'a', 64);
    size_t n = 13 + 64 + 1; /* label, root */
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, n, ip_be(), out, sizeof out));
    /* a 63 byte label is legal */
    memset(q, 0, sizeof q);
    q[0] = 1; q[2] = 0x01; q[5] = 1;
    q[12] = 63;
    memset(q + 13, 'a', 63);
    n = 13 + 63 + 1;
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;
    TEST_ASSERT_EQUAL_UINT(n + 16, hy_dns_answer(q, n, ip_be(), out, sizeof out));
}

static void test_dns_oversize(void)
{
    uint8_t q[600], out[1024];
    memset(q, 0, sizeof q);
    size_t ql = build(q, 1, 1);
    (void)ql;
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, 513, ip_be(), out, sizeof out));
}

static void test_dns_out_small(void)
{
    uint8_t q[64], out[512];
    size_t ql = build(q, 1, 1);
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, ql, ip_be(), out, ql + 15));
    TEST_ASSERT_EQUAL_UINT(ql + 16, hy_dns_answer(q, ql, ip_be(), out, ql + 16));
    ql = build(q, 28, 1);
    TEST_ASSERT_EQUAL_UINT(0, hy_dns_answer(q, ql, ip_be(), out, ql - 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dns_a_query);
    RUN_TEST(test_dns_any_and_class);
    RUN_TEST(test_dns_additional_ignored);
    RUN_TEST(test_dns_aaaa_no_answer);
    RUN_TEST(test_dns_drop_response);
    RUN_TEST(test_dns_drop_opcode);
    RUN_TEST(test_dns_drop_two_questions);
    RUN_TEST(test_dns_compressed_question);
    RUN_TEST(test_dns_truncated);
    RUN_TEST(test_dns_long_label);
    RUN_TEST(test_dns_oversize);
    RUN_TEST(test_dns_out_small);
    return UNITY_END();
}
