#include "hy/dns.h"

#include <stdbool.h>
#include <string.h>

#define DNS_HDR 12
#define DNS_MAX 512
#define QTYPE_A 1
#define QTYPE_ANY 255
#define QCLASS_IN 1
#define ANSWER_LEN 16

size_t hy_dns_answer(const uint8_t *q, size_t qlen, uint32_t ip_be, uint8_t *out, size_t out_len)
{
    if (!q || !out || qlen < DNS_HDR || qlen > DNS_MAX) return 0;
    if (q[2] & 0x80) return 0;            /* a response, not a query */
    if (q[2] & 0x78) return 0;            /* opcode other than 0 */
    if (q[4] != 0 || q[5] != 1) return 0; /* QDCOUNT must be 1 */

    /* Walk the question name. A length byte with either top bit set is a compression pointer or a reserved
     * form; labels are then at most 63 bytes. */
    size_t pos = DNS_HDR;
    for (;;) {
        if (pos >= qlen) return 0;
        uint8_t l = q[pos];
        if (l & 0xC0) return 0;
        if (l == 0) break;
        pos += 1u + l;
    }
    pos++; /* root label */
    if (pos - DNS_HDR > 255) return 0;    /* name longer than the 255 byte limit */
    if (pos + 4 > qlen) return 0;         /* QTYPE and QCLASS must fit */
    size_t qend = pos + 4;

    uint16_t qtype = (uint16_t)((q[pos] << 8) | q[pos + 1]);
    uint16_t qclass = (uint16_t)((q[pos + 2] << 8) | q[pos + 3]);
    bool answer = (qtype == QTYPE_A || qtype == QTYPE_ANY) && qclass == QCLASS_IN;

    size_t need = qend + (answer ? ANSWER_LEN : 0);
    if (out_len < need) return 0;

    out[0] = q[0];
    out[1] = q[1];
    out[2] = (uint8_t)(0x84 | (q[2] & 0x01)); /* QR, AA, RD echoed; TC clear */
    out[3] = 0x80;                            /* RA; Z, AD, CD clear; RCODE 0 */
    out[4] = 0; out[5] = 1;                   /* QDCOUNT */
    out[6] = 0; out[7] = answer ? 1 : 0;      /* ANCOUNT */
    memset(out + 8, 0, 4);                    /* NSCOUNT, ARCOUNT */
    memcpy(out + DNS_HDR, q + DNS_HDR, qend - DNS_HDR);

    if (answer) {
        uint8_t *a = out + qend;
        a[0] = 0xC0; a[1] = 0x0C;             /* name: pointer to the question */
        a[2] = 0; a[3] = QTYPE_A;
        a[4] = 0; a[5] = QCLASS_IN;
        a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 60; /* TTL */
        a[10] = 0; a[11] = 4;                 /* RDLENGTH */
        memcpy(a + 12, &ip_be, 4);            /* already in network byte order */
    }
    return need;
}
