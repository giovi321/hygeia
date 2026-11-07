#pragma once

/* Hand-off inside hy_integ: notification bits of the delivery task, the capture ring, the wanted mask and the
 * webhook transport. */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hy/hook.h"
#include "hy/integ.h"

/* Notification bits of the delivery task (xTaskNotify with eSetBits). */
#define HY_IB_EV    (1u << 0)   /* the capture ring has data */
#define HY_IB_NET   (1u << 1)   /* the network state changed: read it with hy_net_status */
#define HY_IB_APPLY (1u << 2)   /* a pending config (saved settings or a debug override) */
#define HY_IB_STATE (1u << 3)   /* MQTT state dirty */
#define HY_IB_MQTT  (1u << 4)   /* the MQTT queue has messages */
#define HY_IB_TEST  (1u << 5)   /* a test request; the index sits in an atomic */
#define HY_IB_KICK  (1u << 6)   /* debug: reset every hook's failures, deadline and pause */

/* Capture ring: single producer (the LVGL task), single consumer (the delivery task). head and tail are free-running
 * counters; a slot is (counter % HY_INTEG_RING). The producer fills the slot at head in place, then publishes head + 1;
 * a full ring counts lost and drops the new event. */
#define HY_INTEG_RING 128
extern hy_hook_ev_t g_integ_ring[HY_INTEG_RING];
extern atomic_uint g_integ_head, g_integ_tail, g_integ_lost;
/* One bit per hy_iev_t: set when MQTT is on or an on hook selects the type. Published by the delivery task after
 * every apply; the capture reads only this mask. */
extern atomic_uint g_integ_wanted;

/* Internal heap gates of a webhook attempt (bytes free). */
#define HY_WH_HEAP_HTTPS 63488
#define HY_WH_HEAP_HTTP  45056

/* Scheme and host of a hook URL (for gates and logs; the path is never logged). False when the URL does not parse. */
bool hy_wh_target(const char *url, bool *tls, char host[65]);
/* Internal free heap at or above the gate for the scheme. */
bool hy_wh_heap_ok(bool tls);
/* "sha256=<64 hex>" of HMAC-SHA256(secret, body), computed with PSA. False on a PSA error. */
bool hy_wh_sign(const char *secret, const char *body, size_t len, char out[72]);
/* POST body (application/json) to url, signed when secret is not empty. Returns the HTTP status, or -1 on a transport
 * error with *err set (HY_HE_CONNECT, HY_HE_TLS, HY_HE_TIMEOUT, HY_HE_MEMORY). The response body is read (at most
 * 1 KB) and discarded. Blocking, at most about 10 s per step. Delivery task only. */
int  hy_wh_post(const char *url, const char *secret, const char *body, size_t len, hy_integ_err_t *err);
