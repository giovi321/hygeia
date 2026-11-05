#ifndef HY_CABRETRY_H
#define HY_CABRETRY_H

/* Retry rule for a paused Cabinet open (the motion sensor faulted). Pure: no IO, no clock. The runner calls
   hy_cabretry_wake() once per screen wake while the feature is paused; the answer says whether this wake retries.

   A retry is allowed when no retry ran in the last HY_CABRETRY_GAP_MS and fewer than HY_CABRETRY_PER_HOUR retries
   ran in the last hour. So a sensor that is really gone costs at most that many attempts an hour, each logged, and one
   that was wedged once is back at the next wake. Times are monotonic milliseconds. */

#include <stdbool.h>
#include <stdint.h>

#define HY_CABRETRY_PER_HOUR 4
#define HY_CABRETRY_GAP_MS   (60 * 1000)
#define HY_CABRETRY_WINDOW_MS (60 * 60 * 1000)

typedef struct {
    int64_t at[HY_CABRETRY_PER_HOUR];   /* start times of the retries that count, oldest first */
    uint8_t n;
} hy_cabretry_t;

void hy_cabretry_init(hy_cabretry_t *r);
/* A wake while the feature is paused. true: retry now (recorded). false: not yet; *wait_ms (if not NULL) is how long
   until a retry would be allowed, counting only the rules that apply now. */
bool hy_cabretry_wake(hy_cabretry_t *r, int64_t now_ms, int64_t *wait_ms);
/* Retries recorded inside the hour that ends at now_ms */
uint8_t hy_cabretry_count(const hy_cabretry_t *r, int64_t now_ms);

#endif
