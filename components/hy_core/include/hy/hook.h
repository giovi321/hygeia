#ifndef HY_HOOK_H
#define HY_HOOK_H

/* Event payload, signature header, response verdict and retry rules for webhooks and MQTT events. Pure C. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hy/integ.h"
#include "hy/types.h"

#define HY_HOOK_PAYLOAD_MAX 1024

typedef struct {
    hy_iev_t type; int64_t ts; /* 0 = clock invalid */ int64_t slot; char zone[48]; char device[12];
    bool has_med; uint32_t med_id; char code[3]; char name[33]; char dose[17];
    bool has_log; hy_log_t log; uint32_t log_id; /* undone: the id when has_log is false */
    int8_t batt_pct; /* -1 unknown */ int32_t value; /* battery events: percent */
} hy_hook_ev_t;

/* {"id":seq,"event":"<name>","ts":ts|null,"tz":"<zone>"|null,"device":"hygeia-1A2B",
 *  "medicine":{"id":n,"code":"A","name":"Antibiotic","dose":"1 tab"}|null,"slot":slot|null,
 *  "log":<codec log object>|{"id":n}|null,"battery":pct|null}
 * in this member order, no spaces, no newline. ts is null when 0, tz when the zone is empty, slot when 0.
 * battery: value for the two battery events, else batt_pct (null when negative). log: the codec object for
 * has_log, {"id":log_id} for UNDONE without has_log, else null. Length, or -1 when it does not fit or the type is
 * out of range. */
int hy_hook_payload(const hy_hook_ev_t *e, uint32_t seq, char *buf, size_t len);
/* {"id":0,"event":"test","ts":ts|null,"tz":..,"device":..,"medicine":null,"slot":null,"log":null,"battery":..} */
int hy_hook_test_payload(int64_t ts, const char *zone, const char *device, int8_t batt_pct, char *buf, size_t len);
/* "sha256=" + 64 lowercase hex digits; returns 71. */
int hy_hook_sig_header(const uint8_t mac[32], char out[72]);

typedef enum { HY_HV_DELIVERED = 0, HY_HV_RETRY, HY_HV_REJECT, HY_HV_PAUSE } hy_hook_verdict_t;
/* status <= 0 (transport error, timeout): RETRY. 200..299 DELIVERED. 408, 429, 500..599 RETRY. 401, 403 PAUSE (the
 * receiver refuses the credentials: the hook waits for a settings change). Anything else (1xx, 3xx: redirects are
 * not followed, other 4xx): REJECT. */
hy_hook_verdict_t hy_hook_verdict(int status);
/* Seconds to wait after the n-th failed attempt in a row: n 0 gives 0, 1 gives 60, 2 gives 300, 3 or more 1800. */
uint32_t hy_hook_retry_s(uint32_t failures);
#define HY_OB_MAX_AGE_S (7 * 86400)
/* at > 0, clock_valid and now - at > HY_OB_MAX_AGE_S. at is the outbox enqueue time, not the event time. A record
 * enqueued while the clock was invalid (at 0) never expires by age. */
bool hy_ob_expired(int64_t at, int64_t now, bool clock_valid);

#endif /* HY_HOOK_H */
