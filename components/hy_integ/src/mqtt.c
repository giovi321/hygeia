/* MQTT client: lifecycle, availability, outbox events, medicine and device state, and the log, skip and snooze
 * commands.
 *
 * Threads: the esp-mqtt event handler runs in the MQTT task. It only records the link state in atomics and posts
 * acknowledgements and received messages to the MQTT queue (command payloads into the pool); it calls no esp-mqtt
 * API. Everything else runs in the delivery task, which owns the client handle, the in-flight table and the outbox.
 * Nothing here logs a password, a full URL or a payload: logs name the scheme, host and port of the broker only. */

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hy/actions.h"
#include "hy/clock.h"
#include "hy/job.h"
#include "hy/mqttfmt.h"
#include "hy/net.h"
#include "hy/occ.h"
#include "hy/outbox.h"
#include "hy/power_run.h"
#include "hy/sched_run.h"
#include "hy/store.h"
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
#define MQ_QLEN           32      /* a webhook POST holds the delivery task about 10 s; the pump empties it between steps */
#define MQ_SETTLE_MS      1500    /* the scheduler snapshot is rewritten at the next tick after an event */
#define MQ_STATE_MS       60000   /* state job period while connected */
#define MQ_DEVICE_MS      300000  /* the device state goes out at least this often (the RSSI changes without a trigger) */
#define MQ_JOB_RETRY_MS   5000    /* state job timed out */
#define MQ_TOO_SOON_ROWS  32
#define MQ_POOL           4
#define MQ_CMD_MAX        512     /* bytes of a command payload */
#define MQ_PACE_MS        50      /* one discovery or cleanup publish per this time */
#define MQ_SCAN_MS        3000    /* the cleanup scan listens this long */
#define MQ_SCAN_ARENA     32768   /* PSRAM: the retained topics of one scan (about 300 fit for 50 medicines) */
#define MQ_SCAN_ROUNDS    6       /* scans in a row that may follow a full arena or a lost clear */
#define MQ_ADH_MS         (30 * 60 * 1000)   /* adherence is computed at most this often after an event */
#define MQ_NDMED          (2 * HY_MAX_MEDS)  /* medicines with discovery state: active ones and those being cleared */

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

/* The cleanup scan stores every retained topic it receives here, in the handler, so none is lost to the queue:
 * [length byte][topic bytes] records. s_scan_on, s_arena_len and the arena are written under s_scan_mux while
 * s_scan_on is true; the delivery task reads the arena only after it cleared s_scan_on under the same lock. */
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;
static HY_EXT_BSS uint8_t s_arena[MQ_SCAN_ARENA];
static size_t s_arena_len;
static bool s_scan_on;
static atomic_bool s_scan_over;                 /* a record did not fit: the scan is incomplete */

/* ---------------------------------------------------------------- delivery task state */

enum { MQF_FREE = 0, MQF_ONLINE, MQF_EVENT, MQF_STATE, MQF_DISC, MQF_CLEAR };
#define MQ_DEVICE_ID UINT32_MAX   /* seq of a MQF_STATE flight that carries the device state */
/* Work owed after a connect, retried when a publish or subscribe is refused. */
enum { TODO_ONLINE = 1u << 0, TODO_SUB0 = 1u << 1, TODO_SUBS = 7u << 1 };

typedef struct {
    int msg_id;
    uint8_t kind;
    int64_t sent_ms;
    uint32_t seq;          /* MQF_EVENT: the outbox record; MQF_STATE: the medicine id; MQF_DISC: the medicine id */
    uint32_t epoch;        /* MQF_EVENT: the outbox open it came from; MQF_DISC: object, DISC_DEV and DISC_RETRACT bits */
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
static unsigned s_todo;                         /* TODO_* bits */
static int64_t s_todo_at;                       /* no retry of the todo before (monotonic); 0 none */
static bool s_todo_logged;
static unsigned s_drops_seen;                   /* queue + command drops already logged */

/* State publishing. The job fills s_sctx in the LVGL task; the delivery task diffs it against what it sent. */
typedef struct {
    hy_api_tile_t tiles[HY_MAX_MEDS];
    size_t n;
    hy_mqtt_device_t dev;
} mq_sctx_t;
typedef struct {
    uint32_t id;
    uint32_t hash;
} mq_sent_t;
static HY_EXT_BSS mq_sctx_t s_sctx;
static HY_EXT_BSS mq_sent_t s_sent[HY_MAX_MEDS];   /* the state last published this connection, per medicine */
static size_t s_n_sent;
static HY_EXT_BSS char s_jbuf[384];
static HY_EXT_BSS char s_t_pub[HY_MQTT_TOPIC_LEN + 1];
static bool s_have_ctx;                         /* s_sctx holds a result of this connection */
static bool s_st_pend;                          /* part of s_sctx is not published yet */
static int64_t s_pend_at;                       /* no publish pass before (monotonic); 0 none */
static int64_t s_state_at;                      /* next state job (monotonic); 0 none */
static bool s_dev_have;
static uint32_t s_dev_hash;
static int64_t s_dev_at;
static char s_node[16];                         /* discovery node id hygeia_<4 hex> */
static int16_t s_adh_pct = -1;                  /* 30 day adherence of the last pass, -1 unknown */

#if CONFIG_HY_DEBUG_CONSOLE
static const char *const k_flight_name[] = {"", "online", "event", "state", "config", "clear"};
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
    bool ha;
    uint8_t scan_ph, scan_rounds;
    unsigned arena_len, scan_cleared, cfg_sent, cfg_wanted;
    int16_t adh;
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

/* A command topic goes to the queue with its payload (at most MQ_CMD_MAX bytes). Any other topic is a retained one
 * of the cleanup scan: while a scan listens, its topic is stored in the arena (not in the queue); otherwise it is
 * ignored. */
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
    bool is_cmd = !m.trunc && tl && hy_mqtt_parse_cmd(s_base, m.topic, tl, &id, &cmd);
    if (!is_cmd) {
        /* A retained topic of the cleanup scan: stored whole, never queued. */
        if (m.retain && m.total_len > 0 && !m.trunc && tl) {
            portENTER_CRITICAL(&s_scan_mux);
            if (s_scan_on) {
                if (s_arena_len + tl + 1 <= sizeof s_arena) {
                    s_arena[s_arena_len++] = (uint8_t)tl;
                    memcpy(s_arena + s_arena_len, m.topic, tl);
                    s_arena_len += tl;
                } else {
                    atomic_store(&s_scan_over, true);
                }
            }
            portEXIT_CRITICAL(&s_scan_mux);
        }
        return;
    }
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

static bool fl_has(uint8_t kind)
{
    for (int i = 0; i < MQ_INFLIGHT; i++) {
        if (s_fl[i].kind == kind) {
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

/* ---------------------------------------------------------------- commands */

typedef struct {
    uint32_t id;
    uint8_t cmd;
    uint16_t len;
    hy_err_t res;
    char payload[MQ_CMD_MAX + 1];
} mq_cmd_ctx_t;

static const char *const k_cmd_name[] = {"log", "skip", "snooze"};

static const hy_med_t *find_active(uint32_t id)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    for (size_t i = 0; i < n; i++) {
        if (ms[i].id == id) {
            return ms[i].archived ? NULL : &ms[i];
        }
    }
    return NULL;
}

/* LVGL task: the medicine must exist and be active, then the same checks as the web. */
static void job_cmd(void *arg)
{
    mq_cmd_ctx_t *c = arg;
    const hy_med_t *m = find_active(c->id);
    if (!m) {
        c->res = HY_E_NOT_FOUND;
        return;
    }
    uint32_t out_id = 0;
    switch ((hy_mqtt_cmd_t)c->cmd) {
    case HY_MC_LOG: {
        hy_mqtt_log_req_t req;
        if (!hy_mqtt_parse_log(c->payload, c->len, &req)) {
            c->res = HY_E_INVALID;
            return;
        }
        int64_t now = (int64_t)time(NULL);
        if (req.has_ts && !hy_mqtt_log_ts_ok(req.ts, now, hy_clock_build_epoch())) {
            c->res = HY_E_INVALID;
            return;
        }
        hy_err_t chk = HY_OK;
        if (!req.confirm && !req.has_ts && m->type == HY_SCHED_AS_NEEDED && m->min_interval_min > 0 &&
            hy_clock_valid()) {
            hy_log_t *rows = heap_caps_malloc(MQ_TOO_SOON_ROWS * sizeof *rows, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!rows) {
                c->res = HY_E_FULL;   /* no memory: refused, the command can be sent again */
                return;
            }
            size_t n = hy_store_port()->query_log(NULL, now - (int64_t)m->min_interval_min * 60, now + 1, m->id, rows,
                                                  MQ_TOO_SOON_ROWS);
            int32_t since = -1;
            chk = hy_min_interval_check(m, now, rows, n, &since);
            heap_caps_free(rows);
        }
        if (!hy_mqtt_log_decide(m, &req, chk)) {
            c->res = HY_W_TOO_SOON;
            return;
        }
        c->res = hy_act_log_taken(c->id, req.has_ts ? req.ts : 0, req.has_note ? req.note : NULL, HY_SRC_MQTT, &out_id);
        break;
    }
    case HY_MC_SKIP:
        c->res = hy_act_skip(c->id, HY_SRC_MQTT, &out_id);
        break;
    case HY_MC_SNOOZE: {
        uint16_t minutes;
        if (!hy_mqtt_parse_snooze(c->payload, c->len, hy_settings()->snooze_min, &minutes)) {
            c->res = HY_E_INVALID;
            return;
        }
        int64_t until = 0;
        c->res = hy_act_snooze(c->id, minutes, HY_SRC_MQTT, &until);
        break;
    }
    }
}

static void run_cmd(const hy_mq_ctx_t *x, uint32_t id, hy_mqtt_cmd_t cmd, const char *payload, size_t len)
{
    const char *name = (unsigned)cmd < 3 ? k_cmd_name[cmd] : "?";
    if (x->dry) {
        ESP_LOGI(TAG, "mqtt command: %s med %u (dry run)", name, (unsigned)id);
        return;
    }
    hy_job_t *j = hy_job_new(job_cmd, sizeof(mq_cmd_ctx_t));
    if (!j) {
        ESP_LOGI(TAG, "mqtt command: %s med %u: no memory", name, (unsigned)id);
        return;
    }
    mq_cmd_ctx_t *c = hy_job_ctx(j);
    c->id = id;
    c->cmd = (uint8_t)cmd;
    c->len = (uint16_t)(len > MQ_CMD_MAX ? MQ_CMD_MAX : len);
    if (c->len) {
        memcpy(c->payload, payload, c->len);
    }
    hy_err_t res;
    if (hy_job_run(j, HY_JOB_TIMEOUT_MS) == ESP_OK) {
        res = c->res;
        if (res == HY_OK) {
            hy_net_activity();   /* a person acted */
        }
        ESP_LOGI(TAG, "mqtt command: %s med %u: %s", name, (unsigned)id, hy_err_str(res));
    } else {
        ESP_LOGI(TAG, "mqtt command: %s med %u: busy, not run", name, (unsigned)id);
    }
    hy_job_put(j);
    s_state_at = mono_ms() + MQ_SETTLE_MS;   /* the outcome shows in the state topic */
}

/* ---------------------------------------------------------------- received messages */

/* A command message. One that is not retained runs. */
static void on_message(hy_mq_ctx_t *c, const mq_msg_t *m, const char *payload)
{
    uint32_t id;
    hy_mqtt_cmd_t cmd;
    if (!hy_mqtt_cmd_accept(m->retain) || !hy_mqtt_parse_cmd(s_base, m->topic, m->topic_len, &id, &cmd)) {
        return;   /* retained command messages are ignored */
    }
    run_cmd(c, id, cmd, payload ? payload : "", m->pay_len);
}

static void state_lost(uint32_t seq);
static void state_reset(void);
static void disc_state_reset(void);
static void disc_sync(void);
static void flight_lost(const mq_flight_t *f);

/* One queue message: an acknowledgement (PUBLISHED or DELETED) or a received message. */
static void handle(hy_mq_ctx_t *c, const mq_msg_t *m)
{
    if (m->kind == MQK_DATA) {
        on_message(c, m, m->pool >= 0 ? s_pool[m->pool] : NULL);
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
    if (m->kind == MQK_DELETED) {
        flight_lost(f);
    }
    memset(f, 0, sizeof *f);
}

static void drain(hy_mq_ctx_t *c)
{
    while (s_q && xQueueReceive(s_q, &s_msg, 0) == pdTRUE) {
        handle(c, &s_msg);
    }
    unsigned d = atomic_load(&s_q_drops) + atomic_load(&s_cmd_drops);
    if (d != s_drops_seen) {   /* logged here, not in the MQTT task */
        s_drops_seen = d;
        ESP_LOGW(TAG, "messages dropped so far: queue %u, command %u", (unsigned)atomic_load(&s_q_drops),
                 (unsigned)atomic_load(&s_cmd_drops));
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
    s_todo = 0;
    s_todo_at = 0;
    state_reset();
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
    hy_disc_node_id(c->client_id, s_node);
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

/* ---------------------------------------------------------------- state */

static uint32_t fnv1a(const char *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) {
        h = (h ^ (uint8_t)*p++) * 16777619u;
    }
    return h;
}

/* LVGL task: the tiles and the device fields into the static context. */
static void job_state(void *arg)
{
    (void)arg;
    s_sctx.n = hy_sched_tiles(s_sctx.tiles, HY_MAX_MEDS);
    hy_mqtt_device_t *d = &s_sctx.dev;
    memset(d, 0, sizeof *d);
    hy_power_state_t ps;
    hy_power_state(&ps);
    d->batt_pct = ps.pmu_ok ? ps.batt.pct : -1;
    d->usb = ps.pmu_ok ? ps.batt.raw.vbus : true;
    d->charging = ps.batt.raw.chg == HY_CHG_CHARGING;
    hy_net_status_t ns;
    hy_net_status(&ns);
    d->net_mode = (uint8_t)ns.saved;
    d->rssi = ns.rssi;
    int64_t at;
    hy_clock_last_sync(d->clock_src, sizeof d->clock_src, &at);
    d->clock_valid = hy_clock_valid();
    d->adherence = -1;
    strlcpy(d->fw, esp_app_get_description()->version, sizeof d->fw);
}

static mq_sent_t *sent_find(uint32_t id)
{
    for (size_t i = 0; i < s_n_sent; i++) {
        if (s_sent[i].id == id) {
            return &s_sent[i];
        }
    }
    return NULL;
}

static void sent_drop(uint32_t id)
{
    mq_sent_t *e = sent_find(id);
    if (e) {
        *e = s_sent[--s_n_sent];
    }
}

/* A state publish was lost (DELETED by the client, or not acknowledged in time): forget what was recorded for it so
 * the next pass sends the current state again. */
static void state_lost(uint32_t seq)
{
    if (seq == MQ_DEVICE_ID) {
        s_dev_have = false;
    } else {
        sent_drop(seq);
    }
    s_st_pend = true;
    s_pend_at = 0;
}

static bool in_tiles(uint32_t id)
{
    for (size_t i = 0; i < s_sctx.n; i++) {
        if (s_sctx.tiles[i].id == id) {
            return true;
        }
    }
    return false;
}

/* Publishes what differs from the last state sent this connection, through the in-flight rule. A free slot missing
 * leaves the rest pending (an acknowledgement wakes the task); a refusal holds the pass for MQ_HOLD_MS. */
static void state_publish(void)
{
    int64_t now = mono_ms();
    s_st_pend = false;
    s_pend_at = 0;
    for (size_t i = 0; i < s_sctx.n; i++) {
        const hy_api_tile_t *t = &s_sctx.tiles[i];
        int n = hy_mqtt_med_state(t, s_jbuf, sizeof s_jbuf);
        if (n < 0) {
            continue;
        }
        uint32_t h = fnv1a(s_jbuf, (size_t)n);
        mq_sent_t *e = sent_find(t->id);
        if (e && e->hash == h) {
            continue;
        }
        if (fl_free_slot() < 0) {
            s_st_pend = true;
            return;
        }
        if (hy_mqtt_med_topic(s_run.base, t->id, "state", s_t_pub, sizeof s_t_pub) < 0) {
            continue;
        }
        if (pub1(s_t_pub, s_jbuf, n, true, MQF_STATE, t->id, 0) < 0) {
            s_st_pend = true;
            s_pend_at = now + MQ_HOLD_MS;
            return;
        }
        if (e) {
            e->hash = h;
        } else if (s_n_sent < HY_MAX_MEDS) {
            s_sent[s_n_sent].id = t->id;
            s_sent[s_n_sent++].hash = h;
        }
    }
    /* A medicine that left the list while connected: its retained state is cleared. */
    for (size_t i = 0; i < s_n_sent;) {
        uint32_t id = s_sent[i].id;
        if (in_tiles(id)) {
            i++;
            continue;
        }
        if (fl_free_slot() < 0) {
            s_st_pend = true;
            return;
        }
        if (hy_mqtt_med_topic(s_run.base, id, "state", s_t_pub, sizeof s_t_pub) < 0 ||
            pub1(s_t_pub, "", 0, true, MQF_STATE, id, 0) < 0) {
            s_st_pend = true;
            s_pend_at = now + MQ_HOLD_MS;
            return;
        }
        sent_drop(id);   /* the last entry moved into slot i */
    }
    /* The device: when anything but the RSSI changed, and at least every MQ_DEVICE_MS for the RSSI. */
    hy_mqtt_device_t d = s_sctx.dev;
    d.adherence = s_adh_pct;
    int8_t rssi = d.rssi;
    d.rssi = 0;
    int n = hy_mqtt_device_state(&d, s_jbuf, sizeof s_jbuf);
    if (n < 0) {
        return;
    }
    uint32_t h = fnv1a(s_jbuf, (size_t)n);
    if (s_dev_have && h == s_dev_hash && now - s_dev_at < MQ_DEVICE_MS) {
        return;
    }
    if (fl_free_slot() < 0) {
        s_st_pend = true;
        return;
    }
    d.rssi = rssi;
    n = hy_mqtt_device_state(&d, s_jbuf, sizeof s_jbuf);
    if (n < 0 || hy_mqtt_topic(s_run.base, "device", s_t_pub, sizeof s_t_pub) < 0) {
        return;
    }
    if (pub1(s_t_pub, s_jbuf, n, true, MQF_STATE, MQ_DEVICE_ID, 0) < 0) {
        s_st_pend = true;
        s_pend_at = now + MQ_HOLD_MS;
        return;
    }
    s_dev_have = true;
    s_dev_hash = h;
    s_dev_at = now;
}

/* Delivery task, connected: the state job when due (on connect, after an event, every MQ_STATE_MS), then the publish
 * pass when something is pending. */
static void state_pass(const hy_mq_ctx_t *c)
{
    int64_t now = mono_ms();
    if (c->state_req && (s_state_at == 0 || s_state_at > now + MQ_SETTLE_MS)) {
        s_state_at = now + MQ_SETTLE_MS;
    }
    if (s_state_at && now >= s_state_at) {
        hy_job_t *j = hy_job_new(job_state, 8);
        bool ok = false;
        if (j) {
            ok = hy_job_run(j, HY_JOB_TIMEOUT_MS) == ESP_OK;
            hy_job_put(j);
        }
        if (ok) {
            s_have_ctx = true;
            disc_sync();
            s_st_pend = true;
            s_pend_at = 0;
            s_state_at = now + MQ_STATE_MS;
        } else {
            s_state_at = now + MQ_JOB_RETRY_MS;
            return;   /* a job that timed out may still be writing the context: publish nothing from it now */
        }
    }
    if (s_have_ctx && s_st_pend && now >= s_pend_at) {
        state_publish();
    }
}

static void state_reset(void)
{
    s_n_sent = 0;
    s_have_ctx = false;
    s_st_pend = false;
    s_pend_at = 0;
    s_state_at = 0;
    s_dev_have = false;
    disc_state_reset();
}

/* ---------------------------------------------------------------- Home Assistant discovery */

/* Per medicine: the objects wanted now (want), those that may sit retained at the broker (held) and those published
 * on this connection with the current name (sent). held minus want is cleared with an empty retained publish, want
 * minus sent is published. The configs go out one per MQ_PACE_MS through the in-flight table. */
#define DISC_MED_ALL ((1u << HY_DO_MED_COUNT) - 1)
#define DISC_DEV_ALL ((1u << HY_DO_DEV_COUNT) - 1)
#define DISC_DEV     0x100u   /* epoch of a MQF_DISC flight: a device object */
#define DISC_RETRACT 0x200u   /* ... an empty publish */

typedef struct {
    uint32_t id, hash;
    uint8_t want, held, sent;
    bool gone, pills;
    char name[33];
} mq_dmed_t;

static HY_EXT_BSS mq_dmed_t s_dmed[MQ_NDMED];
static size_t s_n_dmed;
static uint8_t s_ddev_sent;                     /* device objects published on this connection */
static HY_EXT_BSS hy_disc_med_t s_dm[HY_MAX_MEDS];   /* the active medicines of the last state job, for the rules */
static size_t s_n_dm;
static bool s_ha, s_ha_init;                    /* discovery on, and whether it was read on this connection */
static char s_pfx[HY_INTEG_TOPIC_MAX + 1];      /* discovery prefix in use */
static char s_old_pfx[HY_INTEG_TOPIC_MAX + 1];  /* a prefix that was in use, its configs still to be removed */
static bool s_disc_ctx;                         /* s_dmed was synced with a state job of this connection */
static bool s_disc_go;                          /* online and the first state went out: configs may follow */
static int64_t s_disc_at;                       /* no discovery or clear publish before (monotonic) */
static HY_EXT_BSS char s_t_cfg[HY_MQTT_TOPIC_LEN + 1];

/* Cleanup scan. Phases: idle, subscribing, listening (MQ_SCAN_MS), clearing. */
enum { SC_IDLE = 0, SC_SUB, SC_WAIT, SC_DEL };
static uint8_t s_scan_ph;
static bool s_req_new, s_req_old;               /* a scan of the current prefix / of s_old_pfx is wanted */
static bool s_scan_old;                         /* the running scan is the one of s_old_pfx */
static bool s_scan_redo;                        /* a clear was lost: scan again */
static unsigned s_scan_subs;                    /* bit i: filter i subscribed */
static uint8_t s_scan_rounds;
static int64_t s_scan_at;                       /* deadline of the phase, or no start before */
static size_t s_scan_cur;                       /* read position in the arena while clearing */
static unsigned s_scan_cleared;                 /* clears published by the running scan */
static HY_EXT_BSS char s_t_scan[2][HY_MQTT_TOPIC_LEN + 1];

static mq_dmed_t *dmed_find(uint32_t id)
{
    for (size_t i = 0; i < s_n_dmed; i++) {
        if (s_dmed[i].id == id) {
            return &s_dmed[i];
        }
    }
    return NULL;
}

static uint8_t med_want(const mq_dmed_t *e)
{
    if (!s_ha || e->gone) {
        return 0;
    }
    return (uint8_t)(e->pills ? DISC_MED_ALL : DISC_MED_ALL & ~(1u << HY_DO_PILLS));
}

static void disc_rewant(void)
{
    for (size_t i = 0; i < s_n_dmed; i++) {
        s_dmed[i].want = med_want(&s_dmed[i]);
    }
}

/* Nothing of ours is retained any more as far as the device knows (discovery off, or the prefix changed). */
static void disc_forget(void)
{
    for (size_t i = 0; i < s_n_dmed; i++) {
        s_dmed[i].held = s_dmed[i].sent = 0;
    }
    s_ddev_sent = 0;
}

static void disc_reset(void)
{
    s_n_dmed = 0;
    s_n_dm = 0;
    s_ddev_sent = 0;
    s_ha_init = false;
    s_disc_ctx = false;
    s_disc_go = false;
    s_disc_at = 0;
}

/* After a state job of this connection: the medicines now active, their names and pill switches. A new medicine has
 * nothing at the broker yet, except on the first sync after a connect, where the scan keeps what is wanted. */
static void disc_sync(void)
{
    bool first = !s_disc_ctx;
    s_n_dm = 0;
    for (size_t i = 0; i < s_n_dmed; i++) {
        s_dmed[i].gone = true;
    }
    for (size_t i = 0; i < s_sctx.n && s_n_dm < HY_MAX_MEDS; i++) {
        const hy_api_tile_t *t = &s_sctx.tiles[i];
        s_dm[s_n_dm].id = t->id;
        s_dm[s_n_dm].pills = t->pills;
        s_n_dm++;
        uint32_t h = fnv1a(t->name, strlen(t->name));
        mq_dmed_t *e = dmed_find(t->id);
        if (!e) {
            if (s_n_dmed >= MQ_NDMED) {
                continue;   /* cleared entries are freed within seconds; this one is picked up at the next sync */
            }
            e = &s_dmed[s_n_dmed++];
            memset(e, 0, sizeof *e);
            e->id = t->id;
            e->hash = h;
            e->pills = t->pills;
            e->want = med_want(e);
            e->held = first ? e->want : 0;
        } else {
            e->gone = false;
            e->pills = t->pills;
            e->want = med_want(e);
            if (h != e->hash) {
                e->hash = h;
                e->sent &= (uint8_t)~e->want;   /* the name is in every config: publish them again */
            }
        }
        strlcpy(e->name, t->name, sizeof e->name);
    }
    for (size_t i = 0; i < s_n_dmed;) {   /* archived or deleted: everything it held is cleared, then it goes */
        mq_dmed_t *e = &s_dmed[i];
        if (e->gone) {
            e->want = 0;
            if (e->held == 0) {
                *e = s_dmed[--s_n_dmed];
                continue;
            }
        }
        i++;
    }
    s_disc_ctx = true;
}

typedef struct {
    bool dev, retract;
    unsigned obj;
    mq_dmed_t *e;
} disc_act_t;

/* The next discovery publish: clears first, then medicine configs, then the device objects. */
static bool disc_find(disc_act_t *a)
{
    for (size_t i = 0; i < s_n_dmed; i++) {
        unsigned r = (unsigned)(s_dmed[i].held & ~s_dmed[i].want);
        if (r) {
            *a = (disc_act_t){false, true, (unsigned)__builtin_ctz(r), &s_dmed[i]};
            return true;
        }
    }
    for (size_t i = 0; i < s_n_dmed; i++) {
        unsigned p = (unsigned)(s_dmed[i].want & ~s_dmed[i].sent);
        if (p) {
            *a = (disc_act_t){false, false, (unsigned)__builtin_ctz(p), &s_dmed[i]};
            return true;
        }
    }
    if (s_ha) {
        unsigned p = DISC_DEV_ALL & ~(unsigned)s_ddev_sent;
        if (p) {
            *a = (disc_act_t){true, false, (unsigned)__builtin_ctz(p), NULL};
            return true;
        }
    }
    return false;
}

static void disc_pass(void)
{
    if (!s_disc_go) {
        if (!(s_up && s_disc_ctx && !s_st_pend && !(s_todo & TODO_ONLINE))) {
            return;   /* online and the first state go out before the configs */
        }
        s_disc_go = true;
    }
    int64_t now = mono_ms();
    disc_act_t a;
    if (now < s_disc_at || fl_free_slot() < 0 || !disc_find(&a)) {
        return;
    }
    int n;
    if (a.dev) {
        n = hy_disc_dev_topic(s_pfx, s_node, (hy_disc_dev_obj_t)a.obj, s_t_cfg, sizeof s_t_cfg);
    } else {
        n = hy_disc_med_topic(s_pfx, s_node, a.e->id, (hy_disc_med_obj_t)a.obj, s_t_cfg, sizeof s_t_cfg);
    }
    unsigned bit = 1u << a.obj;
    if (n < 0) {   /* cannot be built: nothing can be published for it */
        if (a.dev) {
            s_ddev_sent |= (uint8_t)bit;
        } else if (a.retract) {
            a.e->held &= (uint8_t)~bit;
        } else {
            a.e->sent |= (uint8_t)bit;
        }
        return;
    }
    char *json = NULL;
    if (!a.retract) {
        hy_disc_ctx_t dc = {s_run.base, s_node, esp_app_get_description()->version};
        json = a.dev ? hy_disc_dev_config(&dc, (hy_disc_dev_obj_t)a.obj)
                     : hy_disc_med_config(&dc, a.e->id, a.e->name, (hy_disc_med_obj_t)a.obj);
        if (!json) {
            s_disc_at = now + MQ_HOLD_MS;   /* no memory: later */
            return;
        }
    }
    uint32_t meta = a.obj | (a.dev ? DISC_DEV : 0u) | (a.retract ? DISC_RETRACT : 0u);
    int id = pub1(s_t_cfg, json ? json : "", json ? (int)strlen(json) : 0, true, MQF_DISC, a.dev ? 0 : a.e->id, meta);
    free(json);
    if (id < 0) {
        s_disc_at = now + MQ_HOLD_MS;
        return;
    }
    s_disc_at = now + MQ_PACE_MS;
    if (a.dev) {
        s_ddev_sent |= (uint8_t)bit;
    } else if (a.retract) {
        a.e->held &= (uint8_t)~bit;
        a.e->sent &= (uint8_t)~bit;
        if (a.e->gone && a.e->held == 0) {
            *a.e = s_dmed[--s_n_dmed];
        }
    } else {
        a.e->held |= (uint8_t)bit;
        a.e->sent |= (uint8_t)bit;
    }
}

/* A discovery publish was lost (DELETED, or not acknowledged in time): it is sent again. */
static void disc_lost(const mq_flight_t *f)
{
    unsigned bit = 1u << (f->epoch & 0xffu);
    if (f->epoch & DISC_DEV) {
        s_ddev_sent &= (uint8_t)~bit;
        return;
    }
    mq_dmed_t *e = dmed_find(f->seq);
    if (!e) {
        return;
    }
    if (f->epoch & DISC_RETRACT) {
        e->held |= (uint8_t)bit;
    } else {
        e->sent &= (uint8_t)~bit;
    }
}

/* Discovery switched on or off, or its prefix changed, while connected. */
static void ha_check(const hy_mq_ctx_t *c)
{
    bool ha = c->cfg->ha;
    if (!s_ha_init) {
        s_ha_init = true;
        s_ha = ha;
        strlcpy(s_pfx, c->cfg->ha_prefix, sizeof s_pfx);
        disc_rewant();
        return;
    }
    if (strcmp(s_pfx, c->cfg->ha_prefix) != 0) {
        if (s_ha && !s_req_old) {   /* what was published under the old prefix is removed by a scan of it */
            strlcpy(s_old_pfx, s_pfx, sizeof s_old_pfx);
            s_req_old = true;
            s_scan_rounds = 0;
        }
        strlcpy(s_pfx, c->cfg->ha_prefix, sizeof s_pfx);
        disc_forget();
    }
    if (ha != s_ha) {
        s_ha = ha;
        if (!ha) {   /* the scan removes every config of this node */
            disc_forget();
            s_req_new = true;
            s_scan_rounds = 0;
            if (s_scan_ph != SC_IDLE && !s_scan_old) {
                s_scan_redo = true;   /* the running scan listened before this: it cannot have seen our configs */
            }
        }
    }
    disc_rewant();
}

/* ---------------------------------------------------------------- cleanup scan */

static void scan_stop_listening(void)
{
    portENTER_CRITICAL(&s_scan_mux);
    s_scan_on = false;
    portEXIT_CRITICAL(&s_scan_mux);
}

static void scan_reset(void)
{
    scan_stop_listening();
    s_scan_ph = SC_IDLE;
    s_req_new = s_req_old = s_scan_old = s_scan_redo = false;
    s_scan_subs = 0;
    s_scan_rounds = 0;
    s_scan_at = 0;
    s_scan_cur = 0;
    s_arena_len = 0;
    atomic_store(&s_scan_over, false);
}

/* Is the retained topic one that should not exist? Config topics of this node that the rules refuse, and state topics of
 * medicines that are not active. Each topic is judged alone, so a scan that missed some topics can only leave stale
 * ones behind; it never clears a wanted one. */
static bool scan_stale(const char *t, size_t n, bool ha, const char *pfx)
{
    char obj[32];
    if (hy_disc_parse_topic(pfx, s_node, t, n, obj)) {
        return !hy_disc_wanted(obj, ha, s_dm, s_n_dm);
    }
    return hy_mqtt_state_stale(s_run.base, t, n, s_dm, s_n_dm);
}

static void scan_pass(void)
{
    int64_t now = mono_ms();
    if (s_scan_ph == SC_IDLE) {
        if ((!s_req_old && !s_req_new) || now < s_scan_at || !s_up) {
            return;
        }
        s_scan_old = s_req_old;
        const char *spfx = s_scan_old ? s_old_pfx : s_pfx;
        if (snprintf(s_t_scan[0], sizeof s_t_scan[0], "%s/+/%s/+/config", spfx, s_node) >= (int)sizeof s_t_scan[0] ||
            hy_mqtt_topic(s_run.base, "med/+/state", s_t_scan[1], sizeof s_t_scan[1]) < 0) {
            s_req_old = s_req_new = false;
            return;
        }
        portENTER_CRITICAL(&s_scan_mux);
        s_arena_len = 0;
        s_scan_on = true;
        portEXIT_CRITICAL(&s_scan_mux);
        atomic_store(&s_scan_over, false);
        s_scan_subs = 0;
        s_scan_cleared = 0;
        s_scan_ph = SC_SUB;
    }
    if (s_scan_ph == SC_SUB) {
        if (now < s_scan_at) {
            return;
        }
        unsigned want = s_scan_old ? 1u : 3u;   /* a scan of a former prefix looks at its configs only */
        for (unsigned i = 0; i < 2; i++) {
            if (!(want & (1u << i)) || (s_scan_subs & (1u << i))) {
                continue;
            }
            if (esp_mqtt_client_subscribe_single(s_cl, s_t_scan[i], 1) < 0) {
                s_scan_at = now + MQ_HOLD_MS;   /* refused: tried again */
                return;
            }
            s_scan_subs |= 1u << i;
        }
        s_scan_at = now + MQ_SCAN_MS;
        s_scan_ph = SC_WAIT;
        return;
    }
    if (s_scan_ph == SC_WAIT) {
        if (now < s_scan_at) {
            return;
        }
        scan_stop_listening();
        for (unsigned i = 0; i < 2; i++) {
            if ((s_scan_subs & (1u << i)) && esp_mqtt_client_unsubscribe(s_cl, s_t_scan[i]) < 0) {
                ESP_LOGW(TAG, "scan unsubscribe %u failed", i);   /* its messages are ignored from now on */
            }
        }
        if (!s_disc_ctx) {   /* no list of medicines yet: nothing can be judged, the scan runs again */
            ESP_LOGW(TAG, "cleanup scan postponed: no medicine list yet");
            s_scan_ph = SC_IDLE;
            s_scan_at = now + MQ_JOB_RETRY_MS;
            return;
        }
        s_scan_cur = 0;
        s_scan_ph = SC_DEL;
    }
    /* SC_DEL: the stale topics, paced like the configs. The listening has ended, so the arena is read without a lock. */
    const char *pfx = s_scan_old ? s_old_pfx : s_pfx;
    bool ha = s_scan_old ? false : s_ha;
    while (s_scan_cur < s_arena_len) {
        size_t n = s_arena[s_scan_cur];
        const char *t = (const char *)s_arena + s_scan_cur + 1;
        if (!scan_stale(t, n, ha, pfx)) {
            s_scan_cur += n + 1;
            continue;
        }
        if (now < s_disc_at || fl_free_slot() < 0) {
            return;   /* the pace timer or an acknowledgement wakes the task */
        }
        memcpy(s_t_cfg, t, n);
        s_t_cfg[n] = '\0';
        if (pub1(s_t_cfg, "", 0, true, MQF_CLEAR, 0, 0) < 0) {
            s_disc_at = now + MQ_HOLD_MS;
            return;
        }
        s_scan_cleared++;
        s_disc_at = now + MQ_PACE_MS;
        s_scan_cur += n + 1;
        return;   /* one per pace */
    }
    if (fl_has(MQF_CLEAR)) {
        return;   /* a lost clear is only known when its acknowledgement is missing */
    }
    bool over = atomic_load(&s_scan_over);
    bool again = over || s_scan_redo;
    s_scan_redo = false;
    s_scan_ph = SC_IDLE;
    if (again && s_scan_rounds + 1 < MQ_SCAN_ROUNDS) {
        s_scan_rounds++;   /* the request stays: another scan follows, with fewer stale topics left */
        s_scan_at = now + MQ_PACE_MS;
        ESP_LOGI(TAG, "cleanup scan %s: %u topics cleared, scanning again", over ? "incomplete" : "had a lost clear",
                 s_scan_cleared);
        return;
    }
    if (again) {
        ESP_LOGW(TAG, "cleanup scan incomplete after %d rounds: stale topics may remain until the next connect",
                 MQ_SCAN_ROUNDS);
    } else if (s_scan_cleared) {
        ESP_LOGI(TAG, "cleanup scan: %u topics cleared", s_scan_cleared);
    }
    if (s_scan_old) {
        s_req_old = false;
    } else {
        s_req_new = false;
    }
    s_scan_rounds = 0;
}

static void flight_lost(const mq_flight_t *f)
{
    switch (f->kind) {
    case MQF_ONLINE:
        s_todo |= TODO_ONLINE;
        s_todo_at = 0;
        break;
    case MQF_STATE:
        state_lost(f->seq);
        break;
    case MQF_DISC:
        disc_lost(f);
        break;
    case MQF_CLEAR:
        s_scan_redo = true;
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- adherence */

typedef struct {
    uint32_t id;
    hy_log_t *rows;
    bool ok;
    hy_adherence_t ad;
} mq_adh_ctx_t;

static HY_EXT_BSS hy_log_t s_adh_rows[HY_SCHED_ADH_ROWS];   /* scratch of the adherence jobs, owned by the delivery task */
static uint32_t s_adh_ids[HY_MAX_MEDS];
static size_t s_adh_n, s_adh_i;
static uint32_t s_adh_taken, s_adh_resolved;
static bool s_adh_busy;                         /* a pass is running: one medicine per step */
static bool s_adh_dirty;                        /* an event may have changed it */
static int64_t s_adh_at;                        /* next step of the pass, 0 none */
static int64_t s_adh_last;                      /* monotonic end of the last pass on this connection, 0 none */

static void job_adh(void *arg)
{
    mq_adh_ctx_t *c = arg;
    c->ok = hy_sched_med_adherence(c->id, c->rows, HY_SCHED_ADH_ROWS, &c->ad);
}

/* Delivery task, connected: computed once after a connect, then at most every MQ_ADH_MS after a taken, skipped,
 * missed or undone event. One job per active medicine, one per step. */
static void adh_pass(const hy_mq_ctx_t *c)
{
    if (c->adh_req) {
        s_adh_dirty = true;
    }
    int64_t now = mono_ms();
    if (!s_adh_busy) {
        bool due = s_adh_last == 0 ? s_have_ctx : (s_adh_dirty && now - s_adh_last >= MQ_ADH_MS);
        if (!due) {
            return;
        }
        s_adh_n = 0;
        for (size_t i = 0; i < s_sctx.n; i++) {
            s_adh_ids[s_adh_n++] = s_sctx.tiles[i].id;
        }
        s_adh_i = 0;
        s_adh_taken = s_adh_resolved = 0;
        s_adh_busy = true;
        s_adh_dirty = false;
        s_adh_at = 0;
    }
    if (now < s_adh_at) {
        return;
    }
    if (s_adh_i < s_adh_n) {
        hy_job_t *j = hy_job_new(job_adh, sizeof(mq_adh_ctx_t));
        bool ok = false;
        if (j) {
            mq_adh_ctx_t *x = hy_job_ctx(j);
            x->id = s_adh_ids[s_adh_i];
            x->rows = s_adh_rows;
            if (hy_job_run(j, HY_JOB_TIMEOUT_MS) == ESP_OK) {
                ok = true;
                if (x->ok) {
                    s_adh_taken += x->ad.taken;
                    s_adh_resolved += x->ad.resolved;
                }
            }
            hy_job_put(j);   /* a job that timed out may still use the scratch: the retry waits MQ_JOB_RETRY_MS */
        }
        s_adh_at = now + (ok ? MQ_PACE_MS : MQ_JOB_RETRY_MS);
        if (ok) {
            s_adh_i++;
        }
        return;
    }
    s_adh_pct = hy_mqtt_adherence_pct(s_adh_taken, s_adh_resolved);
    s_adh_busy = false;
    s_adh_at = 0;
    s_adh_last = now;
    s_st_pend = true;   /* the device state carries it */
    s_pend_at = 0;
}

static void disc_state_reset(void)
{
    disc_reset();
    scan_reset();
    s_adh_busy = false;
    s_adh_dirty = false;
    s_adh_at = 0;
    s_adh_last = 0;
}

/* "online" and the three command subscriptions. A refused one is logged once and retried every MQ_HOLD_MS until it
 * went out; a full in-flight table waits for the next acknowledgement. */
static void conn_todo(void)
{
    if (!s_todo) {
        return;
    }
    int64_t now = mono_ms();
    if (s_todo_at && now < s_todo_at) {
        return;
    }
    s_todo_at = 0;
    bool refused = false;
    if ((s_todo & TODO_ONLINE) && fl_free_slot() >= 0) {
        if (pub1(s_t_status, "online", 6, true, MQF_ONLINE, 0, 0) >= 0) {
            s_todo &= ~TODO_ONLINE;
        } else {
            refused = true;
            if (!s_todo_logged) {
                ESP_LOGW(TAG, "online not published, retrying every %d ms", MQ_HOLD_MS);
            }
        }
    }
    static const char *const leaves[] = {"med/+/log", "med/+/skip", "med/+/snooze"};
    for (unsigned i = 0; i < 3; i++) {
        unsigned bit = TODO_SUB0 << i;
        if (!(s_todo & bit)) {
            continue;
        }
        if (hy_mqtt_topic(s_run.base, leaves[i], s_t_sub, sizeof s_t_sub) > 0 &&
            esp_mqtt_client_subscribe_single(s_cl, s_t_sub, 1) >= 0) {
            s_todo &= ~bit;
        } else {
            refused = true;
            if (!s_todo_logged) {
                ESP_LOGW(TAG, "subscribe %u failed, retrying every %d ms", i, MQ_HOLD_MS);
            }
        }
    }
    if (refused) {
        s_todo_logged = true;
        s_todo_at = now + MQ_HOLD_MS;
    } else if (!s_todo) {
        s_todo_logged = false;
    }
}

static void mq_connected(void)
{
    s_todo = TODO_ONLINE | TODO_SUBS;
    s_todo_at = 0;
    s_todo_logged = false;
    state_reset();
    s_state_at = mono_ms();   /* the first state job runs at once */
    s_req_new = true;         /* the cleanup scan of this connection */
    s_ev_at = 0;
    conn_todo();
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
    if (!s_up) {
        s_todo = 0;
        s_todo_at = 0;
        state_reset();
    }
    if (s_up) {
        mq_connected();
    }
}

static void timeouts(void)
{
    int64_t now = mono_ms();
    for (int i = 0; i < MQ_INFLIGHT; i++) {
        if (s_fl[i].kind != MQF_FREE && now - s_fl[i].sent_ms >= MQ_ACK_MS) {
            flight_lost(&s_fl[i]);
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
    s_dbg.ha = s_ha;
    s_dbg.scan_ph = s_scan_ph;
    s_dbg.scan_rounds = s_scan_rounds;
    s_dbg.arena_len = (unsigned)s_arena_len;
    s_dbg.scan_cleared = s_scan_cleared;
    s_dbg.adh = s_adh_pct;
    s_dbg.cfg_sent = (unsigned)__builtin_popcount(s_ddev_sent);
    s_dbg.cfg_wanted = s_ha ? HY_DO_DEV_COUNT : 0;
    for (size_t i = 0; i < s_n_dmed; i++) {
        s_dbg.cfg_sent += (unsigned)__builtin_popcount(s_dmed[i].sent);
        s_dbg.cfg_wanted += (unsigned)__builtin_popcount(s_dmed[i].want);
    }
    portEXIT_CRITICAL(&s_dbg_mux);
}
#endif

/* ---------------------------------------------------------------- API (delivery task) */

void hy_mq_pump(hy_mq_ctx_t *c)
{
    if (s_cl) {
        drain(c);
    }
}

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
            ha_check(c);
            timeouts();
            conn_todo();
            send_events(c);   /* before the state: a freed slot goes to the event first */
            state_pass(c);
            disc_pass();
            scan_pass();
            adh_pass(c);
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
    if (s_todo && s_todo_at && s_todo_at < next) {
        next = s_todo_at;
    }
    if (s_state_at && s_state_at < next) {
        next = s_state_at;
    }
    if (s_st_pend && s_pend_at && s_pend_at < next) {
        next = s_pend_at;
    }
    int64_t now = mono_ms();
    disc_act_t a;
    if (((s_disc_go && disc_find(&a)) || s_scan_ph == SC_DEL) && s_disc_at > now && s_disc_at < next) {
        next = s_disc_at;   /* the next discovery or clear publish */
    }
    if ((s_scan_ph != SC_IDLE || s_req_new || s_req_old) && s_scan_at > now && s_scan_at < next) {
        next = s_scan_at;   /* end of the listening, a retry, or the earliest start */
    }
    if (s_adh_busy && s_adh_at && s_adh_at < next) {
        next = s_adh_at;
    } else if (!s_adh_busy && s_adh_last && s_adh_dirty && s_adh_last + MQ_ADH_MS < next) {
        next = s_adh_last + MQ_ADH_MS;
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
    static const char *const phases[] = {"idle", "subscribing", "listening", "clearing"};
    printf("integ: discovery %s, configs sent %u of %u wanted; cleanup scan %s, arena %u of %d bytes%s, cleared %u, "
           "round %u; adherence 30 days %d\n",
           d.ha ? "on" : "off", d.cfg_sent, d.cfg_wanted, d.scan_ph < 4 ? phases[d.scan_ph] : "?", d.arena_len,
           MQ_SCAN_ARENA, atomic_load(&s_scan_over) ? " (full)" : "", d.scan_cleared, (unsigned)d.scan_rounds + 1u,
           (int)d.adh);
}
#endif
