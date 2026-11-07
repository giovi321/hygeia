#pragma once

/* Hand-off inside hy_integ: notification bits of the delivery task, the capture ring, the wanted mask, the webhook
 * transport and the MQTT client. */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hy/hook.h"
#include "hy/integ.h"
#include "hy/outbox.h"
#include "sdkconfig.h"

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

/* Sets notification bits of the delivery task (any task; nothing happens before the task exists). */
void hy_integ_notify(uint32_t bits);

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
/* The URL with scheme and host lower-cased (the path keeps its case), cut to len - 1 bytes. url has passed
 * hy_url_parse (no userinfo), so the authority runs from "://" to the first '/'. */
void hy_url_canon(const char *url, char *out, size_t len);

/* ---- MQTT client (mqtt.c) ---- */

/* Internal free heap needed to start the client (bytes). */
#define HY_MQ_HEAP_MIN 45056

/* One pass of the delivery task. */
typedef struct {
    const hy_mqtt_cfg_t *cfg;   /* the MQTT settings in force */
    bool net;                   /* connected as a client */
    hy_ob_t *ob;                /* the outbox, NULL while closed */
    uint32_t ob_epoch;          /* counts outbox opens: an acknowledgement from before a reopen is not applied */
    hy_ob_rec_t *rec;           /* scratch record (PSRAM) */
    const char *client_id;      /* hygeia-<4 hex> */
    bool acked;                 /* out: hy_ob_done was called, the outbox owes a state write */
    bool state_req;             /* the STATE bit was set: something that shows in the state may have changed */
    bool dry;                   /* a debug override is in force: commands are logged and do nothing */
} hy_mq_ctx_t;

/* Delivery task only. Creates and starts the client when MQTT is on and the network is connected as a client
 * (internal free at least HY_MQ_HEAP_MIN, else error "memory", tried again at a later wake); stops and destroys it
 * when the network leaves that state, MQTT is turned off or its broker, credentials or base change (publishing
 * "offline" first while connected). Takes the acknowledgements and messages the esp-mqtt task queued, publishes
 * "online" and subscribes to the command topics on every connect, and sends the MQTT outbox records one at a time. */
void    hy_mq_step(hy_mq_ctx_t *c);
/* Delivery task only. Takes the messages the MQTT task queued (acknowledgements, commands) without the other work of
 * a step. Called between webhook requests, so a slow POST does not let the queue fill. */
void    hy_mq_pump(hy_mq_ctx_t *c);
/* Monotonic ms of the next deadline of the client (an in-flight timeout, a start retry, a held send), INT64_MAX
 * when none. Delivery task only. */
int64_t hy_mq_next_ms(void);
/* State for the status view, as of the last hy_mq_step. Delivery task only. */
void    hy_mq_status(hy_mqtt_state_t *st, uint8_t *err, int64_t *since);
#if CONFIG_HY_DEBUG_CONSOLE
/* "integ" status lines of the client (any task): state, broker host, in-flight ids, heap and stack numbers. */
void    hy_mq_debug_print(void);
#endif
