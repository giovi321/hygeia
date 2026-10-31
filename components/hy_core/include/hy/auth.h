#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hy/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HY_M_GET = 0, HY_M_POST, HY_M_PUT, HY_M_DELETE } hy_method_t;

#define HY_PAIR_TTL_S      120
#define HY_PAIR_MAX_FAILS  5
#define HY_PAIR_BLOCK_S    60

typedef struct {
    char code[7];
    int64_t expires;
    uint8_t fails;
    int64_t blocked_until;
} hy_pair_t;

void hy_pair_init(hy_pair_t *p);

/* code = rnd % 1000000 as 6 digits, zero-padded; replaces a running code and resets the fail count.
 * HY_E_INVALID while now < blocked_until. now is monotonic seconds (the wall clock may be invalid). */
hy_err_t hy_pair_start(hy_pair_t *p, uint32_t rnd, int64_t now);

/* HY_OK on a match while now < expires; the code is cleared. HY_E_NOT_FOUND when no code runs or it expired.
 * HY_E_INVALID on a mismatch; the fifth mismatch clears the code and sets blocked_until = now + 60.
 * ASCII spaces in `code` are ignored; any other non-digit, or a digit count other than 6, is a mismatch.
 * The comparison runs over 6 bytes whatever the input. */
hy_err_t hy_pair_try(hy_pair_t *p, const char *code, int64_t now);

bool hy_pair_active(const hy_pair_t *p, int64_t now);

bool hy_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

/* Value of cookie `name` in a Cookie header ("a=1; hy_s=abc"). The name must match exactly. False when
 * absent or the value does not fit in len (value length > len - 1). */
bool hy_cookie_get(const char *header, const char *name, char *out, size_t len);

/* Session token text: exactly 64 lowercase hex characters. */
bool hy_token_well_formed(const char *t);

/* Host header value, with an optional ":port" removed, compared case-insensitively and in full against the
 * allowed names. */
bool hy_host_allowed(const char *host, const char *const *allowed, size_t n);

typedef enum { HY_GUARD_OK = 0, HY_GUARD_UNPAIRED, HY_GUARD_FORBIDDEN } hy_guard_t;

typedef struct {
    hy_method_t m;
    bool open_route;
    bool session_ok;
    const char *x_hygeia; /* NULL = absent */
    const char *origin;   /* NULL = absent */
    const char *host;
} hy_guard_in_t;

/* Writes (POST, PUT, DELETE): FORBIDDEN unless x_hygeia is "1" and, when an origin is present, it is
 * "http://" plus a host equal (case-insensitively, port ignored) to the request host. Then UNPAIRED unless
 * open_route or session_ok. The write check comes first, also on open routes. */
hy_guard_t hy_auth_guard(const hy_guard_in_t *in);

#ifdef __cplusplus
}
#endif
