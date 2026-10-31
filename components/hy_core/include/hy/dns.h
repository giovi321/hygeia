#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reply to one DNS query: copies the header id, RD and the question, sets QR, AA, RA, RCODE 0 and clears TC, Z,
 * AD and CD; for QTYPE A or ANY in class IN adds one A record (name pointer 0xC00C, TTL 60, ip_be in network byte
 * order); other types and classes get no answer. Additional records in the query are not copied.
 * Returns the reply length, or 0 to drop: len < 12 or > 512, QR set, opcode not 0, QDCOUNT != 1, a label over
 * 63 bytes, a compression pointer inside the question, a question running past len, out_len too small. */
size_t hy_dns_answer(const uint8_t *q, size_t qlen, uint32_t ip_be, uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif
