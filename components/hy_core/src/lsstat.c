#include "hy/lsstat.h"

#include <stdio.h>
#include <string.h>

/* ---- sleep length histogram ---- */

int hy_ls_bucket(int64_t requested_us)
{
    if (requested_us < 50 * 1000) {
        return 0;
    }
    if (requested_us < 200 * 1000) {
        return 1;
    }
    if (requested_us < 600 * 1000) {
        return 2;
    }
    if (requested_us < 1100 * 1000) {
        return 3;
    }
    if (requested_us < 2100 * 1000) {
        return 4;
    }
    return 5;
}

const char *hy_ls_bucket_label(int bucket)
{
    static const char *const k[HY_LS_BUCKETS] = {"<50 ms", "50-200 ms", "200-600 ms", "600-1100 ms", "1100-2100 ms",
                                                 ">=2100 ms"};
    return bucket >= 0 && bucket < HY_LS_BUCKETS ? k[bucket] : "?";
}

hy_ls_exit_t hy_ls_exit_class(int64_t slept_us)
{
    if (slept_us <= 0) {
        return HY_LS_NOT_TRIED;
    }
    return slept_us < HY_LS_MIN_SLEPT_US ? HY_LS_REJECTED : HY_LS_RAN;
}

/* ---- time on battery per screen state ---- */

int hy_scrtime_category(bool vbus, uint8_t scr, bool asleep, bool held)
{
    if (vbus) {
        return HY_ST_NONE;
    }
    if (scr == 0) {
        return HY_ST_ON;
    }
    if (scr == 1) {
        return HY_ST_DIM;
    }
    if (asleep) {
        return HY_ST_OFF_ASLEEP;
    }
    return held ? HY_ST_OFF_HELD : HY_ST_OFF_AWAKE;
}

void hy_scrtime_mark(hy_scrtime_t *t, int64_t now_ms, int cat)
{
    if (t->cur >= 0 && t->cur < HY_ST_N && now_ms > t->last_ms) {
        t->ms[t->cur] += (uint64_t)(now_ms - t->last_ms);
    }
    t->cur = (int8_t)(cat >= 0 && cat < HY_ST_N ? cat : HY_ST_NONE);
    t->last_ms = now_ms;
}

void hy_scrtime_reset(hy_scrtime_t *t)
{
    memset(t, 0, sizeof *t);
    t->cur = HY_ST_NONE;
}

const char *hy_scrstate_name(int state)
{
    static const char *const k[HY_ST_N] = {"ON", "DIM", "OFF asleep", "OFF held by radio", "OFF awake"};
    return state >= 0 && state < HY_ST_N ? k[state] : "?";
}

/* ---- ring of battery readings ---- */

void hy_mv_ring_clear(hy_mv_ring_t *r)
{
    memset(r, 0, sizeof *r);
}

void hy_mv_ring_push(hy_mv_ring_t *r, uint32_t t_s, uint16_t mv, int8_t pct, uint8_t scr)
{
    hy_mv_pt_t *p = &r->pt[r->total % HY_MV_RING];
    p->t_s = t_s;
    p->mv = mv;
    p->pct = pct;
    p->scr = scr;
    r->total++;
}

uint32_t hy_mv_ring_len(const hy_mv_ring_t *r)
{
    return r->total < HY_MV_RING ? r->total : HY_MV_RING;
}

bool hy_mv_ring_get(const hy_mv_ring_t *r, uint32_t i, hy_mv_pt_t *out)
{
    uint32_t n = hy_mv_ring_len(r);
    if (i >= n) {
        return false;
    }
    uint32_t first = r->total - n;   /* the index (counted from the first push) of the oldest kept point */
    *out = r->pt[(first + i) % HY_MV_RING];
    return true;
}

/* ---- pwrx ---- */

const char *hy_px_name(int flag)
{
    static const char *const k[HY_PX_N] = {"touch", "nonet", "panel", "wakers", "lsvbus"};
    return flag >= 0 && flag < HY_PX_N ? k[flag] : "?";
}

/* Splits args into at most three words; false when there are more or one is too long */
static bool words(const char *args, char w[3][12], int *n)
{
    *n = 0;
    const char *p = args ? args : "";
    for (;;) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            return true;
        }
        if (*n == 3) {
            return false;
        }
        size_t len = 0;
        while (p[len] && p[len] != ' ') {
            len++;
        }
        if (len >= 12) {
            return false;
        }
        memcpy(w[*n], p, len);
        w[*n][len] = '\0';
        (*n)++;
        p += len;
    }
}

hy_pxc_t hy_px_parse(const char *args)
{
    hy_pxc_t bad = {.kind = HY_PXC_BAD};
    char w[3][12];
    int n;
    if (!words(args, w, &n)) {
        return bad;
    }
    if (n == 0) {
        return (hy_pxc_t){.kind = HY_PXC_LIST};
    }
    if (n == 1 && strcmp(w[0], "off") == 0) {
        return (hy_pxc_t){.kind = HY_PXC_ALL_OFF};
    }
    if (n == 1 && strcmp(w[0], "mv") == 0) {
        return (hy_pxc_t){.kind = HY_PXC_MV};
    }
    if (n == 2 && strcmp(w[0], "mv") == 0 && strcmp(w[1], "clear") == 0) {
        return (hy_pxc_t){.kind = HY_PXC_MV_CLEAR};
    }
    if (n == 2 && (strcmp(w[1], "on") == 0 || strcmp(w[1], "off") == 0)) {
        for (int f = 0; f < HY_PX_N; f++) {
            if (strcmp(w[0], hy_px_name(f)) == 0) {
                return (hy_pxc_t){.kind = HY_PXC_SET, .flag = f, .on = w[1][1] == 'n'};
            }
        }
    }
    return bad;
}

void hy_px_names(uint32_t mask, char *out, size_t len)
{
    if (len == 0) {
        return;
    }
    out[0] = '\0';
    size_t pos = 0;
    for (int f = 0; f < HY_PX_N; f++) {
        if (!(mask & (1u << f))) {
            continue;
        }
        int w = snprintf(out + pos, len - pos, "%s%s", pos ? " " : "", hy_px_name(f));
        if (w < 0 || (size_t)w >= len - pos) {
            out[len - 1] = '\0';
            return;
        }
        pos += (size_t)w;
    }
    if (pos == 0) {
        snprintf(out, len, "none");
    }
}
