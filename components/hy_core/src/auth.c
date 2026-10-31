#include "hy/auth.h"

#include <string.h>

#define PAIR_DIGITS 6

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c; }

/* Equal length and equal text, ASCII case-insensitive. */
static bool ieq(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen)
        return false;
    for (size_t i = 0; i < alen; i++) {
        if (lower(a[i]) != lower(b[i]))
            return false;
    }
    return true;
}

/* Length of the host part of `s`: up to the first ':' (port) or end. */
static size_t host_len(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0' && s[n] != ':')
        n++;
    return n;
}

void hy_pair_init(hy_pair_t *p) { memset(p, 0, sizeof *p); }

hy_err_t hy_pair_start(hy_pair_t *p, uint32_t rnd, int64_t now)
{
    if (now < p->blocked_until)
        return HY_E_INVALID;
    if (p->blocked_until != 0) { /* the block has ended */
        p->blocked_until = 0;
        p->fails = 0;
    }
    uint32_t v = rnd % 1000000u;
    for (int i = PAIR_DIGITS - 1; i >= 0; i--) {
        p->code[i] = (char)('0' + v % 10u);
        v /= 10u;
    }
    p->code[PAIR_DIGITS] = '\0';
    p->expires = now + HY_PAIR_TTL_S;
    return HY_OK;
}

bool hy_pair_active(const hy_pair_t *p, int64_t now)
{
    return p->code[0] != '\0' && now < p->expires;
}

void hy_pair_gate_init(hy_pair_gate_t *g) { memset(g, 0, sizeof *g); }

hy_pair_wait_t hy_pair_gate_check(hy_pair_gate_t *g, int64_t now, int64_t *wait)
{
    if (g->open && now >= g->open_until) { /* expired unused */
        g->open = false;
        if (g->open_until + HY_PAIR_COOLDOWN_S > g->cool_until)
            g->cool_until = g->open_until + HY_PAIR_COOLDOWN_S;
    }
    hy_pair_wait_t why = HY_PAIR_GO;
    int64_t until = 0;
    if (g->n_starts > 0 && now < g->starts[g->n_starts - 1] + HY_PAIR_START_GAP_S) {
        why = HY_PAIR_WAIT_GAP;
        until = g->starts[g->n_starts - 1] + HY_PAIR_START_GAP_S;
    } else if (now < g->cool_until) {
        why = HY_PAIR_WAIT_COOLDOWN;
        until = g->cool_until;
    } else if (g->n_starts == HY_PAIR_STARTS_MAX && now < g->starts[0] + HY_PAIR_STARTS_WINDOW_S) {
        why = HY_PAIR_WAIT_CAP;
        until = g->starts[0] + HY_PAIR_STARTS_WINDOW_S;
    }
    if (wait != NULL)
        *wait = why == HY_PAIR_GO ? 0 : (until - now > 0 ? until - now : 1);
    return why;
}

void hy_pair_gate_started(hy_pair_gate_t *g, int64_t now)
{
    if (g->n_starts == HY_PAIR_STARTS_MAX) {
        memmove(&g->starts[0], &g->starts[1], (HY_PAIR_STARTS_MAX - 1) * sizeof g->starts[0]);
        g->n_starts--;
    }
    g->starts[g->n_starts++] = now;
    g->open = true;
    g->open_until = now + HY_PAIR_TTL_S;
}

void hy_pair_gate_ended(hy_pair_gate_t *g, int64_t now, bool cancelled)
{
    if (!g->open)
        return;
    g->open = false;
    if (cancelled && now + HY_PAIR_COOLDOWN_S > g->cool_until)
        g->cool_until = now + HY_PAIR_COOLDOWN_S;
}

hy_err_t hy_pair_try(hy_pair_t *p, const char *code, int64_t now)
{
    if (!hy_pair_active(p, now)) {
        p->code[0] = '\0';
        return HY_E_NOT_FOUND;
    }

    /* Collect up to 6 digits, skipping spaces; anything else, or a wrong digit count, is a mismatch. */
    uint8_t got[PAIR_DIGITS] = {0};
    size_t digits = 0;
    bool bad = (code == NULL);
    for (size_t i = 0; !bad && code[i] != '\0'; i++) {
        char c = code[i];
        if (c == ' ')
            continue;
        if (c < '0' || c > '9' || digits >= PAIR_DIGITS) {
            bad = true;
            break;
        }
        got[digits++] = (uint8_t)c;
    }

    bool match = hy_ct_equal(got, (const uint8_t *)p->code, PAIR_DIGITS);
    if (match && !bad && digits == PAIR_DIGITS) {
        memset(p->code, 0, sizeof p->code);
        p->fails = 0;
        return HY_OK;
    }

    if (++p->fails >= HY_PAIR_MAX_FAILS) {
        memset(p->code, 0, sizeof p->code);
        p->blocked_until = now + HY_PAIR_BLOCK_S;
    }
    return HY_E_INVALID;
}

bool hy_ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

bool hy_cookie_get(const char *header, const char *name, char *out, size_t len)
{
    if (header == NULL || name == NULL || out == NULL || len == 0)
        return false;
    size_t nlen = strlen(name);
    if (nlen == 0)
        return false;

    const char *p = header;
    while (*p != '\0') {
        while (*p == ' ' || *p == ';')
            p++;
        const char *end = p;
        while (*end != '\0' && *end != ';')
            end++;
        if ((size_t)(end - p) > nlen && strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            size_t vlen = (size_t)(end - v);
            while (vlen > 0 && v[vlen - 1] == ' ')
                vlen--;
            if (vlen > len - 1)
                return false;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return true;
        }
        p = end;
    }
    return false;
}

bool hy_token_well_formed(const char *t)
{
    if (t == NULL)
        return false;
    for (size_t i = 0; i < 64; i++) {
        char c = t[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false; /* includes an early NUL */
    }
    return t[64] == '\0';
}

bool hy_host_allowed(const char *host, const char *const *allowed, size_t n)
{
    if (host == NULL || allowed == NULL || host[0] == '[')
        return false;
    size_t hlen = host_len(host);
    if (hlen == 0)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (allowed[i] != NULL && ieq(host, hlen, allowed[i], strlen(allowed[i])))
            return true;
    }
    return false;
}

bool hy_host_first_label_is(const char *host, const char *label)
{
    if (host == NULL || label == NULL || host[0] == '[')
        return false;
    size_t llen = strlen(label);
    size_t hlen = host_len(host);
    if (llen == 0 || hlen < llen || !ieq(host, llen, label, llen))
        return false;
    return hlen == llen || host[llen] == '.';
}

hy_guard_t hy_auth_guard(const hy_guard_in_t *in)
{
    if (in == NULL)
        return HY_GUARD_FORBIDDEN;
    if (in->m != HY_M_GET) {
        if (in->x_hygeia == NULL || strcmp(in->x_hygeia, "1") != 0)
            return HY_GUARD_FORBIDDEN;
        if (in->origin != NULL) {
            static const char scheme[] = "http://";
            if (in->host == NULL || strncmp(in->origin, scheme, sizeof scheme - 1) != 0)
                return HY_GUARD_FORBIDDEN;
            const char *o = in->origin + sizeof scheme - 1;
            if (o[0] == '[' || in->host[0] == '[')
                return HY_GUARD_FORBIDDEN;
            size_t olen = 0;
            while (o[olen] != '\0' && o[olen] != ':' && o[olen] != '/')
                olen++;
            size_t hlen = host_len(in->host);
            if (olen == 0 || !ieq(o, olen, in->host, hlen))
                return HY_GUARD_FORBIDDEN;
        }
    }
    if (in->open_route || in->session_ok)
        return HY_GUARD_OK;
    return HY_GUARD_UNPAIRED;
}
