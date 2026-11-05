#include "hy/cabretry.h"

#include <string.h>

void hy_cabretry_init(hy_cabretry_t *r)
{
    memset(r, 0, sizeof *r);
}

/* Drops the retries that left the one-hour window */
static void expire(hy_cabretry_t *r, int64_t now_ms)
{
    uint8_t k = 0;
    while (k < r->n && now_ms - r->at[k] >= HY_CABRETRY_WINDOW_MS) {
        k++;
    }
    if (k) {
        memmove(r->at, r->at + k, (size_t)(r->n - k) * sizeof r->at[0]);
        r->n = (uint8_t)(r->n - k);
    }
}

uint8_t hy_cabretry_count(const hy_cabretry_t *r, int64_t now_ms)
{
    uint8_t c = 0;
    for (uint8_t i = 0; i < r->n; i++) {
        if (now_ms - r->at[i] < HY_CABRETRY_WINDOW_MS) {
            c++;
        }
    }
    return c;
}

bool hy_cabretry_wake(hy_cabretry_t *r, int64_t now_ms, int64_t *wait_ms)
{
    if (r->n && now_ms < r->at[r->n - 1]) {
        hy_cabretry_init(r);   /* the clock went back: forget the history rather than block for good */
    }
    expire(r, now_ms);
    int64_t wait = 0;
    if (r->n) {
        int64_t gap = r->at[r->n - 1] + HY_CABRETRY_GAP_MS - now_ms;
        if (gap > wait) {
            wait = gap;
        }
    }
    if (r->n >= HY_CABRETRY_PER_HOUR) {
        int64_t win = r->at[0] + HY_CABRETRY_WINDOW_MS - now_ms;
        if (win > wait) {
            wait = win;
        }
    }
    if (wait > 0) {
        if (wait_ms) {
            *wait_ms = wait;
        }
        return false;
    }
    r->at[r->n++] = now_ms;
    if (wait_ms) {
        *wait_ms = 0;
    }
    return true;
}
