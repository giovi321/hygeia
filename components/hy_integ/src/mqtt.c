/* MQTT client: lifecycle, availability and outbox events.
 *
 * Threads: the esp-mqtt event handler runs in the MQTT task. It only records the link state in atomics and posts
 * acknowledgements and received messages to the MQTT queue (command payloads into the pool); it calls no esp-mqtt
 * API. Everything else runs in the delivery task, which owns the client handle, the in-flight table and the outbox.
 * Nothing here logs a password, a full URL or a payload: logs name the scheme, host and port of the broker only. */

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hy/clock.h"
#include "hy/mqttfmt.h"
#include "hy/outbox.h"
#include "integ_int.h"
#include "mqtt_client.h"

static const char *TAG = "integ_mq";

#define MQ_STACK          6144    /* internal: the MQTT task touches no flash */
#define MQ_PRIO           4       /* the delivery task's priority, below the LVGL task */
#define MQ_BUF            2048    /* in and out buffers, PSRAM through Kconfig */
#define MQ_KEEPALIVE_S    60
#define MQ_RECONNECT_MS   30000
#define MQ_NET_TIMEOUT_MS 10000
#define MQ_INFLIGHT       4       /* QoS 1 publishes not yet acknowledged */
#define MQ_ACK_MS         30000   /* in-flight timeout */
#define MQ_OFFLINE_MS     2000    /* wait for the acknowledgement of "offline" before a deliberate stop */
#define MQ_RETRY_MS       60000   /* no memory, or the client did not start */
#define MQ_HOLD_MS        5000    /* the client refused a publish */
#define MQ_IO_MS          10000   /* a transient outbox read failure */
#define MQ_EV_BATCH       20      /* expired records passed over per pass */
#define MQ_QLEN           16
#define MQ_POOL           4
#define MQ_CMD_MAX        512     /* bytes of a command payload */

/* ---------------------------------------------------------------- shared with the MQTT task */

enum { MQK_PUBLISHED = 0, MQK_DELETED, MQK_DATA };

typedef struct {
    uint8_t kind;
    bool retain;
    bool cmd;              /* a command topic of the running base */
    bool trunc;            /* topic longer than HY_MQTT_TOPIC_LEN: cut */
    int8_t pool;           /* pool slot holding the payload of a command, -1 none */
    int msg_id;
    uint16_t topic_len;
    uint16_t pay_len;
    uint32_t total_len;    /* whole payload length (a long message comes in several DATA events; only the first is
                            * posted) */
    char topic[HY_MQTT_TOPIC_LEN + 1];
} mq_msg_t;

static QueueHandle_t s_q;                       /* mq_msg_t, storage in PSRAM; made at the first start */
static HY_EXT_BSS char s_pool[MQ_POOL][MQ_CMD_MAX + 1];
static atomic_uint s_pool_free = (1u << MQ_POOL) - 1;   /* bit i: slot i free */
static atomic_uint s_link;                      /* (generation << 1) | connected; written by the handler only */
static atomic_uint s_link_err;                  /* hy_integ_err_t of the last ERROR event, cleared on CONNECTED */
static atomic_uint s_q_drops, s_cmd_drops;
static atomic_uint s_stack_min = UINT32_MAX;    /* MQTT task stack high-water mark, bytes */
static char s_base[HY_INTEG_TOPIC_MAX + 1];     /* base of the running client, read by the handler */

/* ---------------------------------------------------------------- delivery task state */

enum { MQF_FREE = 0, MQF_ONLINE, MQF_EVENT };

typedef struct {
    int msg_id;
    uint8_t kind;
    int64_t sent_ms;
    uint32_t seq;          /* MQF_EVENT: the outbox record */
    uint32_t epoch;        /* MQF_EVENT: the outbox open it came from */
} mq_flight_t;

static esp_mqtt_client_handle_t s_cl;
static HY_EXT_BSS hy_mqtt_cfg_t s_run;          /* settings of the running client; holds the password, wiped at stop */
static HY_EXT_BSS esp_mqtt_client_config_t s_mc;
static HY_EXT_BSS char s_uri[HY_INTEG_URL_MAX + 1];
static HY_EXT_BSS char s_t_status[HY_MQTT_TOPIC_LEN + 1];
static HY_EXT_BSS char s_t_event[HY_MQTT_TOPIC_LEN + 1];
static HY_EXT_BSS char s_t_sub[HY_MQTT_TOPIC_LEN + 1];
static HY_EXT_BSS mq_msg_t s_msg;
static unsigned s_seen;                         /* the s_link value last handled */
static bool s_up;                               /* connected to the broker, as the delivery task saw it */
static mq_flight_t s_fl[MQ_INFLIGHT];
static int64_t s_retry_ms;                      /* no start before (monotonic); 0 none */
static int64_t s_ev_at;                         /* no event send before (monotonic); 0 none */
static uint8_t s_err;                           /* memory or start error while there is no client */
static bool s_want;                             /* MQTT on and connected as a client, at the last pass */
static hy_mqtt_state_t s_state;
static uint8_t s_state_err;
static int64_t s_since;
static uint32_t s_heap_before;                  /* internal free at the last start */

#if CONFIG_HY_DEBUG_CONSOLE
static const char *const k_flight_name[] = {"", "online", "event"};
typedef struct {
    hy_mqtt_state_t state;
    uint8_t err;
    bool client, tls, user_set, pass_set;
    uint16_t port;
    char host[65];
    char base[HY_INTEG_TOPIC_MAX + 1];
    int ids[MQ_INFLIGHT];
    uint8_t kinds[MQ_INFLIGHT];
    uint32_t heap_before;
} mq_dbg_t;
static portMUX_TYPE s_dbg_mux = portMUX_INITIALIZER_UNLOCKED;
static HY_EXT_BSS mq_dbg_t s_dbg;
#endif

static int64_t mono_ms(void) { return esp_timer_get_time() / 1000; }

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) {
        *v++ = 0;
    }
}

/* ---------------------------------------------------------------- event handler (MQTT task) */

static int pool_take(void)
{
    unsigned f = atomic_load(&s_pool_free);
    while (f) {
        unsigned bit = f & (0u - f);
        if (atomic_compare_exchange_weak(&s_pool_free, &f, f & ~bit)) {
            return __builtin_ctz(bit);
        }
    }
    return -1;
}

static void pool_put(int k)
{
    if (k >= 0 && k < MQ_POOL) {
        atomic_fetch_or(&s_pool_free, 1u << k);
    }
}

static hy_integ_err_t map_err(const esp_mqtt_error_codes_t *e)
{
    if (!e) {
        return HY_HE_CONNECT;
    }
    if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        return e->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                       e->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
                   ? HY_HE_AUTH
                   : HY_HE_CONNECT;
    }
    esp_err_t t = e->esp_tls_last_esp_err;
    if (t >= ESP_ERR_MBEDTLS_CERT_PARTLY_OK && t < ESP_ERR_ESP_TLS_BASE + 0x30) {
        return HY_HE_TLS;
    }
    return HY_HE_CONNECT;
}

static void post(const mq_msg_t *m)
{
    if (!s_q || xQueueSend(s_q, m, 0) != pdTRUE) {
        pool_put(m->pool);
        atomic_fetch_add(&s_q_drops, 1);   /* an acknowledgement lost here is covered by the in-flight timeout */
        return;
    }
    hy_integ_notify(HY_IB_MQTT);
}

/* One queue message per received message: topic (up to HY_MQTT_TOPIC_LEN bytes), retain flag and length, for
 * every topic; the payload only for a command topic, at most MQ_CMD_MAX bytes. */
static void post_data(const esp_mqtt_event_t *ev)
{
    if (ev->current_data_offset != 0) {
        return;   /* a later part of a long message: its first part was posted */
    }
    mq_msg_t m;
    memset(&m, 0, sizeof m);
    m.kind = MQK_DATA;
    m.retain = ev->retain;
    m.pool = -1;
    m.total_len = ev->total_data_len > 0 ? (uint32_t)ev->total_data_len : 0;
    size_t tl = ev->topic && ev->topic_len > 0 ? (size_t)ev->topic_len : 0;
    if (tl > HY_MQTT_TOPIC_LEN) {
        tl = HY_MQTT_TOPIC_LEN;
        m.trunc = true;
    }
    if (tl) {
        memcpy(m.topic, ev->topic, tl);
    }
    m.topic[tl] = '\0';
    m.topic_len = (uint16_t)tl;
    uint32_t id;
    hy_mqtt_cmd_t cmd;
    if (!m.trunc && tl && hy_mqtt_parse_cmd(s_base, m.topic, tl, &id, &cmd)) {
        m.cmd = true;
        if (m.total_len > MQ_CMD_MAX || ev->data_len != ev->total_data_len) {
            atomic_fetch_add(&s_cmd_drops, 1);
            ESP_LOGW(TAG, "command payload of %u bytes dropped (at most %d)", (unsigned)m.total_len, MQ_CMD_MAX);
            return;
        }
        int k = pool_take();
        if (k < 0) {
            atomic_fetch_add(&s_cmd_drops, 1);
            ESP_LOGW(TAG, "command dropped: no free payload slot");
            return;
        }
        if (ev->data_len > 0) {
            memcpy(s_pool[k], ev->data, (size_t)ev->data_len);
        }
        s_pool[k][ev->data_len > 0 ? ev->data_len : 0] = '\0';
        m.pool = (int8_t)k;
        m.pay_len = (uint16_t)(ev->data_len > 0 ? ev->data_len : 0);
    }
    post(&m);
}

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_t *ev = data;
    unsigned hw = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    if (hw < atomic_load(&s_stack_min)) {
        atomic_store(&s_stack_min, hw);   /* single writer: the MQTT task */
    }
    unsigned l = atomic_load(&s_link);
    mq_msg_t m;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        atomic_store(&s_link_err, HY_HE_NONE);
        atomic_store(&s_link, (((l >> 1) + 1u) << 1) | 1u);
        hy_integ_notify(HY_IB_MQTT);
        break;
    case MQTT_EVENT_DISCONNECTED:
        atomic_store(&s_link, ((l >> 1) + 1u) << 1);
        hy_integ_notify(HY_IB_MQTT);
        break;
    case MQTT_EVENT_ERROR:
        atomic_store(&s_link_err, map_err(ev ? ev->error_handle : NULL));
        hy_integ_notify(HY_IB_MQTT);
        break;
    case MQTT_EVENT_PUBLISHED:
    case MQTT_EVENT_DELETED:
        memset(&m, 0, offsetof(mq_msg_t, topic));
        m.kind = id == MQTT_EVENT_PUBLISHED ? MQK_PUBLISHED : MQK_DELETED;
        m.pool = -1;
        m.msg_id = ev ? ev->msg_id : 0;
        m.topic[0] = '\0';
        post(&m);
        break;
    case MQTT_EVENT_DATA:
        if (ev) {
            post_data(ev);
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- in-flight table */

static int fl_free_slot(void)
{
    for (int i = 0; i < MQ_INFLIGHT; i++) {
        if (s_fl[i].kind == MQF_FREE) {
            return i;
        }
    }
    return -1;
}

static mq_flight_t *fl_find(int msg_id)
{
    for (int i = 0; i < MQ_INFLIGHT; i++) {
        if (s_fl[i].kind != MQF_FREE && s_fl[i].msg_id == msg_id) {
            return &s_fl[i];
        }
    }
    return NULL;
}

static bool fl_has_event(void)
{
    for (int i = 0; i < MQ_INFLIGHT; i++) {
        if (s_fl[i].kind == MQF_EVENT) {
            return true;
        }
    }
    return false;
}

static void fl_clear(void)
{
    memset(s_fl, 0, sizeof s_fl);   /* an event in flight stays pending in the outbox */
}

/* A QoS 1 publish under the in-flight rule. The msg id, or -1 when every slot is taken or the client refused it. */
static int pub1(const char *topic, const char *data, int len, bool retain, uint8_t kind, uint32_t seq, uint32_t epoch)
{
    int k = fl_free_slot();
    if (k < 0) {
        return -1;
    }
    int id = esp_mqtt_client_publish(s_cl, topic, data, len, 1, retain ? 1 : 0);
    if (id <= 0) {
        return -1;
    }
    s_fl[k].msg_id = id;
    s_fl[k].kind = kind;
    s_fl[k].sent_ms = mono_ms();
    s_fl[k].seq = seq;
    s_fl[k].epoch = epoch;
    return id;
}

/* ---------------------------------------------------------------- received messages */

/* A message on a subscribed topic. Commands (not retained) are acted on by the state and command handling, and
 * other topics feed the retained-topic cleanup of the discovery; neither is part of this file yet, so the message is
 * only taken off the queue. */
static void on_message(const mq_msg_t *m, const char *payload)
{
    (void)payload;
    if (m->cmd && !hy_mqtt_cmd_accept(m->retain)) {
        return;   /* retained command messages are ignored */
    }
    ESP_LOGD(TAG, "%s message, %u bytes", m->cmd ? "command" : "other", (unsigned)m->total_len);
}

/* One queue message: an acknowledgement (PUBLISHED or DELETED) or a received message. */
static void handle(hy_mq_ctx_t *c, const mq_msg_t *m)
{
    if (m->kind == MQK_DATA) {
        on_message(m, m->pool >= 0 ? s_pool[m->pool] : NULL);
        pool_put(m->pool);
        return;
    }
    mq_flight_t *f = fl_find(m->msg_id);
    if (!f) {
        return;   /* an id not in flight: ignored */
    }
    if (f->kind == MQF_EVENT && m->kind == MQK_PUBLISHED && c->ob && f->epoch == c->ob_epoch) {
        hy_ob_done(c->ob, 0, f->seq, HY_OBO_DELIVERED);
        c->acked = true;
    }
    /* DELETED of the event in flight: the record stays at the head and goes out again with a new msg id */
    memset(f, 0, sizeof *f);
}

static void drain(hy_mq_ctx_t *c)
{
    while (s_q && xQueueReceive(s_q, &s_msg, 0) == pdTRUE) {
        handle(c, &s_msg);
    }
}

/* ---------------------------------------------------------------- lifecycle */

static bool cfg_differs(const hy_mqtt_cfg_t *a)
{
    return strcmp(a->url, s_run.url) != 0 || strcmp(a->user, s_run.user) != 0 || strcmp(a->pass, s_run.pass) != 0 ||
           strcmp(a->base, s_run.base) != 0;
}

static void link_reset(void)
{
    atomic_store(&s_link, 0);
    atomic_store(&s_link_err, HY_HE_NONE);
    s_seen = 0;
    s_up = false;
    fl_clear();
}

/* Deliberate stop: "offline" retained QoS 1 while connected and up to MQ_OFFLINE_MS for its acknowledgement (a clean
 * DISCONNECT does not fire the last will), then stop and destroy. Acknowledgements that arrive meanwhile are applied. */
static void mq_stop(hy_mq_ctx_t *c)
{
    if (!s_cl) {
        return;
    }
    if (s_up) {
        int id = esp_mqtt_client_publish(s_cl, s_t_status, "offline", 7, 1, 1);
        int64_t end = mono_ms() + MQ_OFFLINE_MS;
        while (id > 0) {
            int64_t left = end - mono_ms();
            if (left <= 0 || xQueueReceive(s_q, &s_msg, pdMS_TO_TICKS(left) + 1) != pdTRUE) {
                ESP_LOGW(TAG, "no acknowledgement of offline within %d ms", MQ_OFFLINE_MS);
                break;
            }
            if (s_msg.kind == MQK_PUBLISHED && s_msg.msg_id == id) {
                break;
            }
            handle(c, &s_msg);
        }
    }
    esp_mqtt_client_stop(s_cl);   /* sends DISCONNECT when connected; waits for the MQTT task to end */
    esp_mqtt_client_destroy(s_cl);
    s_cl = NULL;
    while (xQueueReceive(s_q, &s_msg, 0) == pdTRUE) {
        handle(c, &s_msg);
    }
    atomic_store(&s_pool_free, (1u << MQ_POOL) - 1);
    link_reset();
    s_ev_at = 0;
    wipe(&s_run, sizeof s_run);
    ESP_LOGI(TAG, "client stopped");
}

static void mq_start(hy_mq_ctx_t *c)
{
    s_heap_before = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_heap_before < HY_MQ_HEAP_MIN) {
        s_err = HY_HE_MEMORY;
        s_retry_ms = mono_ms() + MQ_RETRY_MS;
        ESP_LOGW(TAG, "not started: internal free %u below %d", (unsigned)s_heap_before, HY_MQ_HEAP_MIN);
        return;
    }
    hy_url_t u;
    if (hy_url_parse(c->cfg->url, true, &u) != HY_OK || hy_integ_check_topic(c->cfg->base) != HY_OK ||
        hy_mqtt_topic(c->cfg->base, "status", s_t_status, sizeof s_t_status) < 0 ||
        hy_mqtt_topic(c->cfg->base, "event", s_t_event, sizeof s_t_event) < 0) {
        s_err = HY_HE_CONNECT;
        s_retry_ms = mono_ms() + MQ_RETRY_MS;
        ESP_LOGW(TAG, "not started: broker URL or base topic not valid");
        return;
    }
    if (!s_q) {
        s_q = xQueueCreateWithCaps(MQ_QLEN, sizeof(mq_msg_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_q) {
            s_err = HY_HE_MEMORY;
            s_retry_ms = mono_ms() + MQ_RETRY_MS;
            ESP_LOGW(TAG, "not started: no memory for the queue");
            return;
        }
    }
    memcpy(&s_run, c->cfg, sizeof s_run);
    strlcpy(s_base, s_run.base, sizeof s_base);
    hy_url_canon(s_run.url, s_uri, sizeof s_uri);
    memset(&s_mc, 0, sizeof s_mc);
    s_mc.broker.address.uri = s_uri;
    if (u.tls) {
        s_mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    s_mc.credentials.client_id = c->client_id;
    if (s_run.user[0]) {
        s_mc.credentials.username = s_run.user;
    }
    if (s_run.pass[0]) {
        s_mc.credentials.authentication.password = s_run.pass;
    }
    s_mc.session.keepalive = MQ_KEEPALIVE_S;   /* clean session (the default): the broker keeps no commands */
    s_mc.session.last_will.topic = s_t_status;
    s_mc.session.last_will.msg = "offline";
    s_mc.session.last_will.msg_len = 7;
    s_mc.session.last_will.qos = 1;
    s_mc.session.last_will.retain = 1;
    s_mc.network.reconnect_timeout_ms = MQ_RECONNECT_MS;
    s_mc.network.timeout_ms = MQ_NET_TIMEOUT_MS;
    s_mc.buffer.size = MQ_BUF;
    s_mc.buffer.out_size = MQ_BUF;
    s_mc.task.stack_size = MQ_STACK;
    s_mc.task.priority = MQ_PRIO;
    xQueueReset(s_q);
    atomic_store(&s_pool_free, (1u << MQ_POOL) - 1);
    link_reset();
    atomic_store(&s_stack_min, UINT32_MAX);
    s_cl = esp_mqtt_client_init(&s_mc);   /* copies the strings it keeps */
    memset(&s_mc, 0, sizeof s_mc);
    if (!s_cl) {
        wipe(&s_run, sizeof s_run);
        s_err = HY_HE_MEMORY;
        s_retry_ms = mono_ms() + MQ_RETRY_MS;
        ESP_LOGW(TAG, "not started: no memory for the client");
        return;
    }
    if (esp_mqtt_client_register_event(s_cl, MQTT_EVENT_ANY, on_mqtt, NULL) != ESP_OK ||
        esp_mqtt_client_start(s_cl) != ESP_OK) {
        esp_mqtt_client_destroy(s_cl);
        s_cl = NULL;
        wipe(&s_run, sizeof s_run);
        s_err = HY_HE_MEMORY;
        s_retry_ms = mono_ms() + MQ_RETRY_MS;
        ESP_LOGW(TAG, "not started: the client task did not start");
        return;
    }
    s_err = HY_HE_NONE;
    s_retry_ms = 0;
    ESP_LOGI(TAG, "connecting to %s://%s:%u, internal free %u", u.tls ? "mqtts" : "mqtt", u.host, (unsigned)u.port,
             (unsigned)s_heap_before);
}

/* On every connect: "online" retained, the command subscriptions, then the outbox events. */
static void mq_connected(void)
{
    pub1(s_t_status, "online", 6, true, MQF_ONLINE, 0, 0);
    static const char *const leaves[] = {"med/+/log", "med/+/skip", "med/+/snooze"};
    for (unsigned i = 0; i < sizeof leaves / sizeof leaves[0]; i++) {
        if (hy_mqtt_topic(s_run.base, leaves[i], s_t_sub, sizeof s_t_sub) > 0 &&
            esp_mqtt_client_subscribe_single(s_cl, s_t_sub, 1) < 0) {
            ESP_LOGW(TAG, "subscribe %u failed", i);
        }
    }
    s_ev_at = 0;
    ESP_LOGI(TAG, "connected");
}

static void link_check(void)
{
    unsigned l = atomic_load(&s_link);
    if (l == s_seen) {
        return;
    }
    s_seen = l;
    fl_clear();   /* a disconnect or a new session: every in-flight id is void */
    s_up = (l & 1u) != 0;
    if (s_up) {
        mq_connected();
    }
}

static void timeouts(void)
{
    int64_t now = mono_ms();
    for (int i = 0; i < MQ_INFLIGHT; i++) {
        if (s_fl[i].kind != MQF_FREE && now - s_fl[i].sent_ms >= MQ_ACK_MS) {
            memset(&s_fl[i], 0, sizeof s_fl[i]);   /* an event goes out again with a new msg id */
        }
    }
}

/* Outbox records of dest 0, one in flight at a time, in order. An expired record is counted before a send. */
static void send_events(hy_mq_ctx_t *c)
{
    if (!c->ob || fl_has_event()) {
        return;
    }
    int64_t now = mono_ms();
    if (s_ev_at && now < s_ev_at) {
        return;
    }
    s_ev_at = 0;
    bool valid = hy_clock_valid();
    for (int k = 0; k < MQ_EV_BATCH; k++) {
        if (fl_free_slot() < 0) {
            return;   /* an acknowledgement or a timeout frees one and wakes the task */
        }
        hy_err_t r = hy_ob_peek(c->ob, 0, c->rec);
        if (r == HY_E_NOT_FOUND) {
            return;
        }
        if (r != HY_OK) {
            s_ev_at = now + MQ_IO_MS;   /* a transient read failure: the record stays */
            return;
        }
        if (hy_ob_expired(c->rec->at, (int64_t)time(NULL), valid)) {
            hy_ob_done(c->ob, 0, c->rec->seq, HY_OBO_EXPIRED);
            c->acked = true;
            continue;
        }
        if (pub1(s_t_event, c->rec->payload, (int)strlen(c->rec->payload), false, MQF_EVENT, c->rec->seq,
                 c->ob_epoch) < 0) {
            s_ev_at = now + MQ_HOLD_MS;
        }
        return;
    }
    s_ev_at = now;   /* more expired records: the next pass follows at once */
}

static void state_update(const hy_mq_ctx_t *c)
{
    hy_mqtt_state_t st;
    uint8_t err = HY_HE_NONE;
    if (!c->cfg->on) {
        st = HY_MQS_OFF;
    } else if (!s_cl) {
        st = s_err && c->net ? HY_MQS_ERROR : HY_MQS_WAITING;
        err = st == HY_MQS_ERROR ? s_err : HY_HE_NONE;
    } else if (s_up) {
        st = HY_MQS_CONNECTED;
    } else {
        err = (uint8_t)atomic_load(&s_link_err);
        st = err ? HY_MQS_ERROR : HY_MQS_CONNECTING;
    }
    if (st != s_state || err != s_state_err) {
        s_state = st;
        s_state_err = err;
        s_since = hy_clock_valid() ? (int64_t)time(NULL) : 0;
    }
}

#if CONFIG_HY_DEBUG_CONSOLE
static void dbg_publish(void)
{
    hy_url_t u;
    bool ok = s_cl && hy_url_parse(s_run.url, true, &u) == HY_OK;
    portENTER_CRITICAL(&s_dbg_mux);
    s_dbg.state = s_state;
    s_dbg.err = s_state_err;
    s_dbg.client = s_cl != NULL;
    s_dbg.tls = ok && u.tls;
    s_dbg.port = ok ? u.port : 0;
    if (ok) {
        memcpy(s_dbg.host, u.host, sizeof s_dbg.host);
    } else {
        s_dbg.host[0] = '\0';
    }
    memcpy(s_dbg.base, s_run.base, sizeof s_dbg.base);
    s_dbg.user_set = s_run.user[0] != '\0';
    s_dbg.pass_set = s_run.pass[0] != '\0';
    for (int i = 0; i < MQ_INFLIGHT; i++) {
        s_dbg.ids[i] = s_fl[i].msg_id;
        s_dbg.kinds[i] = s_fl[i].kind;
    }
    s_dbg.heap_before = s_heap_before;
    portEXIT_CRITICAL(&s_dbg_mux);
}
#endif

/* ---------------------------------------------------------------- API (delivery task) */

void hy_mq_step(hy_mq_ctx_t *c)
{
    s_want = c->cfg->on && c->net;
    if (s_cl && (!s_want || cfg_differs(c->cfg))) {
        mq_stop(c);
    }
    if (!s_want) {
        s_retry_ms = 0;
        if (!c->cfg->on) {
            s_err = HY_HE_NONE;
        }
    } else if (!s_cl && mono_ms() >= s_retry_ms) {
        mq_start(c);
    }
    if (s_cl) {
        drain(c);       /* acknowledgements received before a link change still count */
        link_check();
        if (s_up) {
            timeouts();
            send_events(c);
        }
    }
    state_update(c);
#if CONFIG_HY_DEBUG_CONSOLE
    dbg_publish();
#endif
}

int64_t hy_mq_next_ms(void)
{
    int64_t next = INT64_MAX;
    if (!s_cl) {
        if (s_want && s_retry_ms > 0) {
            next = s_retry_ms;
        }
        return next;
    }
    if (!s_up) {
        return next;   /* esp-mqtt reconnects by itself and its events wake the task */
    }
    for (int i = 0; i < MQ_INFLIGHT; i++) {
        if (s_fl[i].kind != MQF_FREE && s_fl[i].sent_ms + MQ_ACK_MS < next) {
            next = s_fl[i].sent_ms + MQ_ACK_MS;
        }
    }
    if (s_ev_at && s_ev_at < next) {
        next = s_ev_at;
    }
    return next;
}

void hy_mq_status(hy_mqtt_state_t *st, uint8_t *err, int64_t *since)
{
    *st = s_state;
    *err = s_state_err;
    *since = s_since;
}

#if CONFIG_HY_DEBUG_CONSOLE
void hy_mq_debug_print(void)
{
    static const char *const names[] = {"off", "waiting", "connecting", "connected", "error"};
    mq_dbg_t d;
    portENTER_CRITICAL(&s_dbg_mux);
    d = s_dbg;
    portEXIT_CRITICAL(&s_dbg_mux);
    const char *err = hy_integ_err_name((hy_integ_err_t)d.err);
    printf("integ: mqtt %s%s%s", d.state <= HY_MQS_ERROR ? names[d.state] : "?", err && err[0] ? ", error " : "",
           err ? err : "");
    if (d.client) {
        printf("; broker %s://%s:%u, base %s, user %s, password %s", d.tls ? "mqtts" : "mqtt", d.host,
               (unsigned)d.port, d.base, d.user_set ? "set" : "none", d.pass_set ? "set" : "none");
    }
    printf("\ninteg: mqtt in flight:");
    unsigned n = 0;
    for (int i = 0; i < MQ_INFLIGHT; i++) {
        if (d.kinds[i] != MQF_FREE) {
            printf(" %d(%s)", d.ids[i], k_flight_name[d.kinds[i]]);
            n++;
        }
    }
    unsigned hw = atomic_load(&s_stack_min);
    printf("%s; internal free before connect %u; mqtt task stack free min ", n ? "" : " none",
           (unsigned)d.heap_before);
    if (hw == UINT32_MAX) {
        printf("unknown");
    } else {
        printf("%u bytes", hw);
    }
    printf("; queue drops %u, command drops %u\n", (unsigned)atomic_load(&s_q_drops),
           (unsigned)atomic_load(&s_cmd_drops));
}
#endif
