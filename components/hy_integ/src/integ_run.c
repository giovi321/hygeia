/* Delivery task, event capture, outbox ownership and the debug overrides. The MQTT client (mqtt.c) and the
 * webhooks (webhook.c) are driven from this task.
 *
 * Threads: the capture runs in the LVGL task (bus callback) and only fills the ring; the delivery task owns the
 * outbox, the active config and the retry state; hy_integ_apply, hy_integ_status, hy_integ_test and the debug command
 * only touch the pending slots and the published copies under s_mx (nothing slow happens under it). Nothing here
 * takes hy_board_lock during network or flash I/O, touches the radio or calls hy_net_activity. */

#include "hy/integ_run.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hy/actions.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "hy/events.h"
#include "hy/fwimg.h"
#include "hy/job.h"
#include "hy/net.h"
#include "hy/outbox.h"
#include "hy/power_run.h"
#include "hy/store.h"
#include "integ_int.h"

#if CONFIG_HY_DEBUG_CONSOLE
#include <fcntl.h>
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#endif

static const char *TAG = "integ";

#define TASK_STACK      8192
#define TASK_PRIO       4          /* the web backup worker's priority, below the LVGL task */
#define HOOK_BATCH      20         /* records per hook per pass */
#define HOOK_BUDGET_MS  2000       /* time per hook per pass: then MQTT and the other hooks get their turn */
#define WAIT_MEMORY_MS  60000      /* heap under the gate: not a failure */
#define WAIT_BUSY_MS    10000      /* heavy-job slot held: not a failure */
#define WAIT_IO_MS      10000      /* a transient outbox read failure */
#define FLUSH_MARGIN_MS 1000
#define MAX_SUBS        2

/* ---------------------------------------------------------------- shared with the capture */

HY_EXT_BSS hy_hook_ev_t g_integ_ring[HY_INTEG_RING];
atomic_uint g_integ_head, g_integ_tail, g_integ_lost;
atomic_uint g_integ_wanted;
static atomic_bool s_any_on;       /* a destination is on: network changes wake the delivery task */
static atomic_bool s_mq_on;        /* MQTT is on: every bus event asks for a state refresh */

static TaskHandle_t s_task;
static char s_device[12];          /* the access point name hygeia-<4 hex>, fixed per device */
static char s_ob_dir[32];          /* "/data/outbox"; "" when the store is not open */

/* ---------------------------------------------------------------- published under s_mx */

static StaticSemaphore_t s_mx_buf;
static SemaphoreHandle_t s_mx;
static HY_EXT_BSS hy_integ_t s_pending;        /* hy_integ_apply */
static bool s_pending_set;
#if CONFIG_HY_DEBUG_CONSOLE
static HY_EXT_BSS hy_integ_t s_ovr_pending;    /* debug override */
static atomic_bool s_ovr_active;               /* fire, fill and burst are allowed */
#endif
enum { OVR_NONE = 0, OVR_ON, OVR_OFF };
static int s_ovr_cmd;
#if CONFIG_HY_DEBUG_CONSOLE
enum { HA_NONE = 0, HA_ON, HA_OFF };
static int s_ha_cmd;                           /* integ ha: discovery on or off in the active override */
#endif
static HY_EXT_BSS hy_integ_status_t s_status;
static int64_t s_retry_at_ms[HY_HOOK_MAX];     /* monotonic; 0 none */
static bool s_pub_connected;
static bool s_pub_tls[HY_HOOK_MAX];
static bool s_pub_free[HY_HOOK_MAX];   /* the slot holds no webhook (an empty URL) */
static struct { void (*cb)(void *ctx); void *ctx; } s_subs[MAX_SUBS];
static atomic_int s_test_req = -1;

#if CONFIG_HY_DEBUG_CONSOLE
typedef struct {
    bool on, secret_set, paused;
    uint16_t events;
    uint32_t failures;
    char host[65];
} dbg_hook_t;
static HY_EXT_BSS struct {
    uint8_t n_hooks;
    dbg_hook_t hook[HY_HOOK_MAX];
    bool ob_open, ob_ram;
    hy_ob_stats_t st;
} s_dbg;
#endif

/* ---------------------------------------------------------------- delivery task state */

typedef struct {
    uint32_t failures;
    int64_t deadline_ms;   /* monotonic; no attempt before it */
    bool paused;           /* 401 or 403: waits for a save, a reconnect, a successful test or a kick */
    int64_t at;            /* last attempt, epoch s (0 unknown) */
    int16_t http;
    uint8_t err;
} hook_rt_t;

static HY_EXT_BSS hy_integ_t s_saved;    /* the saved settings */
static HY_EXT_BSS hy_integ_t s_cfg;      /* in force: s_saved, or the override */
static HY_EXT_BSS char s_payload[HY_HOOK_PAYLOAD_MAX + 1];
static HY_EXT_BSS hy_ob_rec_t s_rec;
static hook_rt_t s_hk[HY_HOOK_MAX];
static hy_ob_t *s_ob;
static uint32_t s_ob_epoch;              /* counts outbox opens (an MQTT acknowledgement names the open it belongs to) */
static bool s_ob_ram;
static bool s_storage_err;               /* RAM fallback for the saved settings */
static bool s_override;
static bool s_connected;
static bool s_ob_dirty;                  /* the outbox owes a state write (hy_ob_dirty after each flush) */
static bool s_ack_dirty;                 /* an ack or drop of ours not yet written: forces the write before blocking offline */
static int64_t s_ob_fail_ms = -(HY_OB_FLUSH_S * 1000);   /* last failed state write */
static int64_t s_ob_write_ms;            /* last state write seen */
static hy_ob_stats_t s_st;               /* the last stats (kept while the outbox is closed) */
static uint8_t s_test_err;
static int16_t s_test_http;

static int64_t mono_ms(void) { return esp_timer_get_time() / 1000; }
static int64_t mono_s(void) { return esp_timer_get_time() / 1000000; }

static int64_t wall_now(void)
{
    return hy_clock_valid() ? (int64_t)time(NULL) : 0;
}

static void notify(uint32_t bits)
{
    if (s_task) {
        xTaskNotify(s_task, bits, eSetBits);
    }
}

void hy_integ_notify(uint32_t bits)
{
    notify(bits);
}

static int8_t batt_pct(void)
{
    hy_power_state_t st;
    hy_power_state(&st);
    return st.pmu_ok ? st.batt.pct : -1;
}

/* ---------------------------------------------------------------- capture (LVGL task) */

/* The slot at head, zeroed, or NULL (and lost counted) when the ring is full. */
static hy_hook_ev_t *ring_reserve(void)
{
    unsigned h = atomic_load_explicit(&g_integ_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&g_integ_tail, memory_order_acquire);
    if (h - t >= HY_INTEG_RING) {
        atomic_fetch_add(&g_integ_lost, 1);
        return NULL;
    }
    hy_hook_ev_t *e = &g_integ_ring[h % HY_INTEG_RING];
    memset(e, 0, sizeof *e);
    return e;
}

static void ring_commit(void)
{
    atomic_fetch_add_explicit(&g_integ_head, 1, memory_order_release);
    notify(HY_IB_EV);
}

static void fill_common(hy_hook_ev_t *e, hy_iev_t type, int64_t ts)
{
    e->type = type;
    e->ts = ts;
    hy_clock_zone_copy(e->zone, sizeof e->zone);
    memcpy(e->device, s_device, sizeof e->device);
    e->batt_pct = batt_pct();
}

static bool fill_med(hy_hook_ev_t *e, uint32_t id, const char *code)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    for (size_t i = 0; i < n; i++) {
        if ((code && strcmp(ms[i].code, code) == 0) || (!code && ms[i].id == id)) {
            e->has_med = true;
            e->med_id = ms[i].id;
            memcpy(e->code, ms[i].code, sizeof e->code);
            memcpy(e->name, ms[i].name, sizeof e->name);
            memcpy(e->dose, ms[i].dose, sizeof e->dose);
            return true;
        }
    }
    return false;
}

/* Bus callback, LVGL task: no file, network or allocation here. */
static void on_bus(const hy_event_t *ev, void *ctx)
{
    (void)ctx;
    if (atomic_load(&s_mq_on)) {
        /* any event may change a state topic, also the types no destination wants; a dose taken, skipped, missed or
         * undone may change the adherence */
        bool adh = ev->type == HY_EV_TAKEN || ev->type == HY_EV_SKIPPED || ev->type == HY_EV_MISSED ||
                   ev->type == HY_EV_UNDONE;
        notify(adh ? HY_IB_STATE | HY_IB_ADH : HY_IB_STATE);
    }
    hy_iev_t t;
    if (!hy_iev_from_bus(ev->type, &t) || !(atomic_load(&g_integ_wanted) & (1u << t))) {
        return;
    }
    hy_hook_ev_t *e = ring_reserve();
    if (!e) {
        return;
    }
    fill_common(e, t, ev->ts);
    e->slot = ev->slot;
    if (ev->med_id) {
        fill_med(e, ev->med_id, NULL);
    }
    if (t == HY_IE_UNDONE) {
        e->log_id = ev->log_id;   /* the payload sends {"id":n} only, never the restored entry */
    } else if (ev->log_id && hy_act_last_log(ev->log_id, &e->log)) {
        e->has_log = true;
        e->log_id = ev->log_id;
    }
    if (t == HY_IE_BATTERY_LOW || t == HY_IE_BATTERY_CRITICAL) {
        e->value = ev->value;
    }
    ring_commit();
}

static void on_net(const hy_net_status_t *st, void *ctx)
{
    (void)st;
    (void)ctx;
    if (atomic_load(&s_any_on)) {   /* with every destination off no task wakes */
        notify(HY_IB_NET);
    }
}

/* ---------------------------------------------------------------- config (delivery task) */

static bool hook_on(const hy_integ_t *c, unsigned i)
{
    return i < c->n_hooks && c->hook[i].on;
}

static bool any_on(const hy_integ_t *c)
{
    if (c->mqtt.on) {
        return true;
    }
    for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
        if (hook_on(c, i)) {
            return true;
        }
    }
    return false;
}

static uint8_t dest_mask(hy_iev_t t)
{
    uint8_t m = s_cfg.mqtt.on ? 1u : 0u;
    for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
        if (hook_on(&s_cfg, i) && (s_cfg.hook[i].events & (1u << t))) {
            m |= (uint8_t)(1u << (1 + i));
        }
    }
    return m;
}

static void publish_wanted(void)
{
    unsigned w = s_cfg.mqtt.on ? HY_IE_ALL : 0;
    for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
        if (hook_on(&s_cfg, i)) {
            w |= s_cfg.hook[i].events;
        }
    }
    atomic_store(&g_integ_wanted, w & HY_IE_ALL);
    atomic_store(&s_any_on, any_on(&s_cfg));
    atomic_store(&s_mq_on, s_cfg.mqtt.on);
}

static void hook_reset(unsigned i)
{
    s_hk[i].failures = 0;
    s_hk[i].deadline_ms = 0;
    s_hk[i].paused = false;
    if (s_hk[i].err == HY_HE_AUTH) {
        s_hk[i].err = HY_HE_NONE;
    }
}

static void ob_drop(uint8_t dest)
{
    if (!s_ob) {
        return;
    }
    hy_ob_stats_t st;
    hy_ob_stats(s_ob, &st);
    if (st.pending[dest] > 0) {   /* a drop of nothing changes nothing and owes no state write */
        hy_ob_drop_dest(s_ob, dest);
        s_ob_dirty = s_ack_dirty = true;
    }
}

/* Drops the waiting events of every destination that is off in s_cfg. */
static void ob_drop_off(void)
{
    if (!s_cfg.mqtt.on) {
        ob_drop(0);
    }
    for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
        if (!hook_on(&s_cfg, i)) {
            ob_drop((uint8_t)(1 + i));
        }
    }
}

static void ob_close(void)
{
    if (s_ob) {
        hy_ob_stats(s_ob, &s_st);
        hy_ob_close(s_ob);
        s_ob = NULL;
        s_ob_dirty = s_ack_dirty = false;
    }
}

/* Opens the outbox when a destination is on: the file one for the saved settings, the RAM one for an override or
 * when the storage is down or the open fails (status "storage"). */
static bool ob_ensure(void)
{
    if (s_ob) {
        return true;
    }
    hy_err_t r = HY_E_IO;
    s_ob_ram = true;
    if (!s_override) {
        if (s_ob_dir[0]) {
            r = hy_ob_open(s_ob_dir, mono_s, &s_ob);
            if (r == HY_OK) {
                s_ob_ram = false;
                s_storage_err = false;
            } else {
                ESP_LOGW(TAG, "outbox open failed (%s): events kept in RAM until a restart", hy_err_str(r));
            }
        } else {
            ESP_LOGW(TAG, "storage not mounted: events kept in RAM until a restart");
        }
        if (r != HY_OK) {
            s_storage_err = true;
        }
    }
    if (r != HY_OK) {
        r = hy_ob_open(NULL, mono_s, &s_ob);
        if (r != HY_OK) {
            ESP_LOGE(TAG, "RAM outbox failed (%s)", hy_err_str(r));
            s_ob = NULL;
            return false;
        }
    }
    s_ob_epoch++;
    s_ob_write_ms = mono_ms();
    s_ob_dirty = s_ack_dirty = false;
    ob_drop_off();
    hy_ob_stats(s_ob, &s_st);
    return true;
}

/* New settings in force: compare the hook list by slot with the one before. */
static void apply_cfg(const hy_integ_t *nw)
{
    for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
        bool was = hook_on(&s_cfg, i), is = hook_on(nw, i);
        if (was && !is) {
            ob_drop((uint8_t)(1 + i));   /* removed or turned off: its waiting events are discarded */
            hook_reset(i);
            s_hk[i].err = HY_HE_NONE;
        } else if (is && (!was || strcmp(s_cfg.hook[i].url, nw->hook[i].url) != 0)) {
            hook_reset(i);               /* new, or a corrected URL: its waiting events are kept */
        } else if (is) {
            s_hk[i].paused = false;      /* saved again: an auth pause ends */
            if (s_hk[i].err == HY_HE_AUTH) {
                s_hk[i].err = HY_HE_NONE;
            }
        }
    }
    if (s_cfg.mqtt.on && !nw->mqtt.on) {
        ob_drop(0);
    }
    memcpy(&s_cfg, nw, sizeof s_cfg);
    publish_wanted();
    if (any_on(&s_cfg)) {
        ob_ensure();
    } else {
        ob_close();
    }
}

#if CONFIG_HY_DEBUG_CONSOLE
static void override_on(void)
{
    ob_close();   /* forced flush of the file outbox */
    s_override = true;
    for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
        hook_reset(i);
        s_hk[i].err = HY_HE_NONE;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    memcpy(&s_cfg, &s_ovr_pending, sizeof s_cfg);
    memset(&s_ovr_pending, 0, sizeof s_ovr_pending);   /* held a secret */
    xSemaphoreGive(s_mx);
    publish_wanted();
    ob_ensure();
    atomic_store(&s_ovr_active, true);
    ESP_LOGW(TAG, "debug override on: RAM outbox, saved integration settings not used until 'integ off'");
}

static void override_off(void)
{
    atomic_store(&s_ovr_active, false);
    ob_close();   /* the RAM outbox and its events are discarded */
    s_override = false;
    for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
        hook_reset(i);
        s_hk[i].err = HY_HE_NONE;
    }
    memcpy(&s_cfg, &s_saved, sizeof s_cfg);
    publish_wanted();
    if (any_on(&s_cfg)) {
        ob_ensure();
    }
    ESP_LOGW(TAG, "debug override off: saved integration settings back in force");
}
#endif

/* Takes the pending settings (and the debug override commands). While an override is in force, saved settings are
 * kept for when it ends. */
static void take_pending(void)
{
    bool saved = false;
    int ovr;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_pending_set) {
        memcpy(&s_saved, &s_pending, sizeof s_saved);
        memset(&s_pending, 0, sizeof s_pending);   /* held secrets */
        s_pending_set = false;
        saved = true;
    }
    ovr = s_ovr_cmd;
    s_ovr_cmd = OVR_NONE;
#if CONFIG_HY_DEBUG_CONSOLE
    int ha_cmd = s_ha_cmd;
    s_ha_cmd = HA_NONE;
#endif
    xSemaphoreGive(s_mx);
#if CONFIG_HY_DEBUG_CONSOLE
    if (ovr == OVR_OFF && s_override) {
        override_off();
        saved = false;   /* s_saved is in force now */
    }
#endif
    if (saved && !s_override) {
        apply_cfg(&s_saved);
    }
#if CONFIG_HY_DEBUG_CONSOLE
    if (ovr == OVR_ON) {
        override_on();
    }
    if (ha_cmd != HA_NONE && s_override) {
        s_cfg.mqtt.ha = ha_cmd == HA_ON;   /* the MQTT client reads it at its next pass */
    }
#else
    (void)ovr;
#endif
}

/* ---------------------------------------------------------------- outbox writes */

static void drain_ring(void)
{
    unsigned t = atomic_load_explicit(&g_integ_tail, memory_order_relaxed);
    while (t != atomic_load_explicit(&g_integ_head, memory_order_acquire)) {
        const hy_hook_ev_t *e = &g_integ_ring[t % HY_INTEG_RING];
        uint8_t mask = dest_mask(e->type);
        if (mask && !ob_ensure()) {
            atomic_fetch_add(&g_integ_lost, 1);   /* no outbox at all (no memory) */
        } else if (mask) {
            int n = hy_hook_payload(e, hy_ob_next_seq(s_ob), s_payload, sizeof s_payload);
            hy_err_t r = n > 0 ? hy_ob_append(s_ob, wall_now(), mask, s_payload, NULL) : HY_E_INVALID;
            if (r != HY_OK) {
                atomic_fetch_add(&g_integ_lost, 1);
                ESP_LOGW(TAG, "event %s not stored: %s", hy_iev_name(e->type), hy_err_str(r));
            }
        }
        t++;
        atomic_store_explicit(&g_integ_tail, t, memory_order_release);
    }
}

/* State write after a pass. While the outbox owes one (hy_ob_dirty, also after a failed write) it is forced once
 * HY_OB_FLUSH_S has passed since the last attempt, so there is at most one write per HY_OB_FLUSH_S and one at least
 * HY_OB_FLUSH_S after the last ack (wait_ticks wakes for it while connected). Offline, an ack or drop of ours is also
 * forced out before the task blocks with no timeout, at most once per HY_OB_FLUSH_S after a failure; drops when full
 * while offline ride on the rate-limited write and never cost a write per event. A failed write is tried again
 * HY_OB_FLUSH_S later, never in a spin. */
static void ob_flush(void)
{
    if (!s_ob) {
        return;
    }
    int64_t now = mono_ms();
    bool due = hy_ob_dirty(s_ob) && now >= s_ob_write_ms + HY_OB_FLUSH_S * 1000;
    bool offline = !s_connected && s_ack_dirty && now >= s_ob_fail_ms + HY_OB_FLUSH_S * 1000;
    bool force = due || offline;
    if (hy_ob_flush(s_ob, force)) {
        s_ob_write_ms = now;
        s_ack_dirty = false;
    } else if (force && hy_ob_dirty(s_ob)) {
        s_ob_write_ms = now;   /* the write failed: the outbox keeps it owed */
        s_ob_fail_ms = now;
    } else if (force) {
        s_ack_dirty = false;   /* nothing was owed */
    }
    s_ob_dirty = hy_ob_dirty(s_ob);
}

/* ---------------------------------------------------------------- MQTT */

static void mqtt_ctx(hy_mq_ctx_t *c, bool state_req, bool adh_req)
{
    memset(c, 0, sizeof *c);
    c->cfg = &s_cfg.mqtt;
    c->net = s_connected;
    c->ob = s_ob;
    c->ob_epoch = s_ob_epoch;
    c->rec = &s_rec;
    c->client_id = s_device;
    c->state_req = state_req;
    c->adh_req = adh_req;
    c->dry = s_override;
}

static void mqtt_pass(bool state_req, bool adh_req)
{
    hy_mq_ctx_t c;
    mqtt_ctx(&c, state_req, adh_req);
    hy_mq_step(&c);
    if (c.acked) {
        s_ob_dirty = s_ack_dirty = true;
    }
}

/* Between webhook requests: a POST can take about 10 s, so the queue of the MQTT task is emptied after each one
 * (acknowledgements, commands). Nothing else of a step runs here. */
static void mqtt_pump(void)
{
    hy_mq_ctx_t c;
    mqtt_ctx(&c, false, false);
    hy_mq_pump(&c);
    if (c.acked) {
        s_ob_dirty = s_ack_dirty = true;
    }
}

/* ---------------------------------------------------------------- webhooks */

static void net_check(void)
{
    hy_net_status_t st;
    hy_net_status(&st);
    bool c = st.state == HY_NS_CONNECTED;
    if (c && !s_connected) {
        for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
            hook_reset(i);   /* a new connection: queued events go out at once */
        }
    }
    s_connected = c;
}

static void deliver_hook(unsigned i)
{
    hook_rt_t *hk = &s_hk[i];
    const hy_hook_cfg_t *h = &s_cfg.hook[i];
    uint8_t dest = (uint8_t)(1 + i);
    bool tls = false;
    hy_wh_target(h->url, &tls, NULL);
    int64_t start = mono_ms();
    for (int k = 0; k < HOOK_BATCH; k++) {
        int64_t now = mono_ms();
        if (k > 0 && now - start >= HOOK_BUDGET_MS) {
            return;   /* the deadline stays 0: the next loop runs MQTT first, then continues here */
        }
        if (hy_heavy_owner()) {
            hk->err = HY_HE_BUSY;
            hk->deadline_ms = now + WAIT_BUSY_MS;
            return;
        }
        if (!hy_wh_heap_ok(tls)) {
            hk->err = HY_HE_MEMORY;
            hk->deadline_ms = now + WAIT_MEMORY_MS;
            return;
        }
        hy_err_t r = hy_ob_peek(s_ob, dest, &s_rec);
        if (r == HY_E_NOT_FOUND) {
            if (k == 0) {
                hk->deadline_ms = now + WAIT_IO_MS;   /* counted pending but nothing readable: no spin */
            }
            return;
        }
        if (r != HY_OK) {
            hk->deadline_ms = now + WAIT_IO_MS;   /* a transient read failure: the record stays */
            return;
        }
        if (hy_ob_expired(s_rec.at, (int64_t)time(NULL), hy_clock_valid())) {
            hy_ob_done(s_ob, dest, s_rec.seq, HY_OBO_EXPIRED);
            s_ob_dirty = s_ack_dirty = true;
            continue;
        }
        hy_integ_err_t err = HY_HE_NONE;
        int status = hy_wh_post(h->url, h->secret, s_rec.payload, strlen(s_rec.payload), &err);
        mqtt_pump();
        hk->at = wall_now();
        hk->http = status > 0 ? (int16_t)status : 0;
        switch (hy_hook_verdict(status)) {
        case HY_HV_DELIVERED:
            hy_ob_done(s_ob, dest, s_rec.seq, HY_OBO_DELIVERED);
            s_ob_dirty = s_ack_dirty = true;
            hk->failures = 0;
            hk->deadline_ms = 0;
            hk->err = HY_HE_NONE;
            break;
        case HY_HV_REJECT:
            hy_ob_done(s_ob, dest, s_rec.seq, HY_OBO_REJECTED);
            s_ob_dirty = s_ack_dirty = true;
            hk->err = HY_HE_HTTP;
            ESP_LOGW(TAG, "webhook %u rejected event %u (HTTP %d)", i, (unsigned)s_rec.seq, status);
            break;
        case HY_HV_PAUSE:
            hk->paused = true;
            hk->err = HY_HE_AUTH;
            ESP_LOGW(TAG, "webhook %u refused the credentials (HTTP %d): paused", i, status);
            return;
        case HY_HV_RETRY:
        default:
            hk->failures++;
            hk->deadline_ms = mono_ms() + (int64_t)hy_hook_retry_s(hk->failures) * 1000;
            hk->err = status > 0 ? HY_HE_HTTP : err;
            return;
        }
    }
}

/* A paused hook sends nothing, but its records still age: on any wake, expired records at its head are counted as
 * expired (no request). At most HOOK_BATCH per hook per wake; the rest follow at later wakes. */
static void expire_paused(void)
{
    if (!s_ob) {
        return;
    }
    bool valid = hy_clock_valid();
    int64_t now = (int64_t)time(NULL);
    for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
        if (!hook_on(&s_cfg, i) || !s_hk[i].paused) {
            continue;
        }
        uint8_t dest = (uint8_t)(1 + i);
        for (int k = 0; k < HOOK_BATCH; k++) {
            if (hy_ob_peek(s_ob, dest, &s_rec) != HY_OK || !hy_ob_expired(s_rec.at, now, valid)) {
                break;
            }
            hy_ob_done(s_ob, dest, s_rec.seq, HY_OBO_EXPIRED);
            s_ob_dirty = s_ack_dirty = true;
        }
    }
}

static void deliver(void)
{
    if (!s_connected || !s_ob) {
        return;
    }
    hy_ob_stats(s_ob, &s_st);
    int64_t now = mono_ms();
    for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
        if (!hook_on(&s_cfg, i) || s_hk[i].paused || now < s_hk[i].deadline_ms || s_st.pending[1 + i] == 0) {
            continue;
        }
        deliver_hook(i);
    }
}

static void run_test(void)
{
    int idx = atomic_exchange(&s_test_req, -1);
    if (idx < 0) {
        return;
    }
    int status = 0;
    hy_integ_err_t err = HY_HE_NONE;
    bool tls = false;
    if (idx >= s_cfg.n_hooks) {
        err = HY_HE_CONNECT;
    } else if (!s_connected) {
        err = HY_HE_OFFLINE;
    } else if (hy_heavy_owner()) {
        err = HY_HE_BUSY;
    } else if (hy_wh_target(s_cfg.hook[idx].url, &tls, NULL) && !hy_wh_heap_ok(tls)) {
        err = HY_HE_MEMORY;
    } else {
        char zone[48];
        hy_clock_zone_copy(zone, sizeof zone);
        int n = hy_hook_test_payload(wall_now(), zone, s_device, batt_pct(), s_payload, sizeof s_payload);
        if (n > 0) {
            status = hy_wh_post(s_cfg.hook[idx].url, s_cfg.hook[idx].secret, s_payload, (size_t)n, &err);
            if (status > 0) {
                err = hy_hook_verdict(status) == HY_HV_DELIVERED ? HY_HE_NONE : HY_HE_HTTP;
            }
            if (hy_hook_verdict(status) == HY_HV_DELIVERED && s_hk[idx].paused) {
                hook_reset((unsigned)idx);   /* a successful test ends an auth pause */
            }
        }
        ESP_LOGI(TAG, "test of webhook %d: %s %d", idx, err ? hy_integ_err_name(err) : "HTTP", status);
    }
    s_test_http = status > 0 ? (int16_t)status : 0;
    s_test_err = (uint8_t)err;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_status.test_running = false;
    s_status.test_index = (int8_t)idx;
    s_status.test_http = s_test_http;
    s_status.test_err = s_test_err;
    xSemaphoreGive(s_mx);
}

/* ---------------------------------------------------------------- status */

static void publish_status(bool force_notify)
{
    if (s_ob) {
        hy_ob_stats(s_ob, &s_st);
    }
    hy_integ_status_t prev, st;
    memset(&st, 0, sizeof st);
    hy_mq_status(&st.mqtt, &st.mqtt_err, &st.mqtt_since);
    st.ha = s_cfg.mqtt.on && s_cfg.mqtt.ha;
    st.mqtt_pending = s_st.pending[0];
    st.n_hooks = s_cfg.n_hooks;
    int64_t retry_at[HY_HOOK_MAX] = {0};
    bool tls[HY_HOOK_MAX] = {0};
    bool slot_free[HY_HOOK_MAX] = {0};
    for (unsigned i = 0; i < s_cfg.n_hooks && i < HY_HOOK_MAX; i++) {
        hy_hook_status_t *h = &st.hook[i];
        h->at = s_hk[i].at;
        h->http = s_hk[i].http;
        h->err = s_hk[i].err;
        h->pending = s_cfg.hook[i].on ? s_st.pending[1 + i] : 0;
        h->paused = s_hk[i].paused;
        h->retry_in_s = -1;   /* filled in hy_integ_status from retry_at */
        if (s_cfg.hook[i].on && !h->paused && h->pending) {
            retry_at[i] = s_hk[i].deadline_ms > 0 ? s_hk[i].deadline_ms : 1;
        }
        hy_wh_target(s_cfg.hook[i].url, &tls[i], NULL);
        slot_free[i] = s_cfg.hook[i].url[0] == '\0';
    }
    st.pending = s_st.live;
    st.dropped = s_st.dropped;
    st.expired = s_st.expired;
    st.rejected = s_st.rejected;
    st.lost = atomic_load(&g_integ_lost);
    st.outbox_err = s_storage_err && !s_override ? HY_HE_STORAGE : HY_HE_NONE;
    st.override = s_override;

    void (*cbs[MAX_SUBS])(void *) = {0};
    void *ctxs[MAX_SUBS] = {0};
    xSemaphoreTake(s_mx, portMAX_DELAY);
    prev = s_status;
    st.test_running = prev.test_running;
    st.test_index = prev.test_index;
    st.test_http = prev.test_http;
    st.test_err = prev.test_err;
    bool changed = force_notify || memcmp(&prev, &st, sizeof st) != 0 ||
                   memcmp(s_retry_at_ms, retry_at, sizeof retry_at) != 0;
    s_status = st;
    memcpy(s_retry_at_ms, retry_at, sizeof retry_at);
    memcpy(s_pub_tls, tls, sizeof tls);
    memcpy(s_pub_free, slot_free, sizeof slot_free);
    s_pub_connected = s_connected;
#if CONFIG_HY_DEBUG_CONSOLE
    s_dbg.n_hooks = s_cfg.n_hooks;
    for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
        dbg_hook_t *d = &s_dbg.hook[i];
        d->on = s_cfg.hook[i].on;
        d->events = s_cfg.hook[i].events;
        d->secret_set = s_cfg.hook[i].secret[0] != '\0';
        d->paused = s_hk[i].paused;
        d->failures = s_hk[i].failures;
        if (!hy_wh_target(s_cfg.hook[i].url, NULL, d->host)) {
            d->host[0] = '\0';
        }
    }
    s_dbg.ob_open = s_ob != NULL;
    s_dbg.ob_ram = s_ob_ram;
    s_dbg.st = s_st;
#endif
    if (changed) {
        for (unsigned i = 0; i < MAX_SUBS; i++) {
            cbs[i] = s_subs[i].cb;
            ctxs[i] = s_subs[i].ctx;
        }
    }
    xSemaphoreGive(s_mx);
    for (unsigned i = 0; i < MAX_SUBS; i++) {
        if (cbs[i]) {
            cbs[i](ctxs[i]);
        }
    }
}

/* ---------------------------------------------------------------- the task */

/* Ticks until the earliest retry deadline, the next deadline of the MQTT client (an in-flight timeout, a start retry)
 * or the state write that the outbox owes (the 10 s redelivery bound needs a flush at least HY_OB_FLUSH_S after the
 * last write even when nothing else happens). Only while connected as a client; otherwise no timeout, so the task
 * adds no wakeups while offline. */
static TickType_t wait_ticks(void)
{
    if (!s_connected) {
        return portMAX_DELAY;
    }
    int64_t now = mono_ms();
    int64_t next = hy_mq_next_ms();
    if (s_ob) {
        for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
            if (hook_on(&s_cfg, i) && !s_hk[i].paused && s_st.pending[1 + i] && s_hk[i].deadline_ms < next) {
                next = s_hk[i].deadline_ms;
            }
        }
        if (s_ob_dirty) {
            int64_t due = s_ob_write_ms + HY_OB_FLUSH_S * 1000 + FLUSH_MARGIN_MS;
            if (due < next) {
                next = due;
            }
        }
    }
    if (next == INT64_MAX) {
        return portMAX_DELAY;
    }
    if (next <= now) {
        return 0;
    }
    return pdMS_TO_TICKS(next - now) + 1;
}

static void integ_task(void *arg)
{
    (void)arg;
    net_check();
    if (any_on(&s_cfg)) {
        ob_ensure();
    }
    publish_status(true);
    TickType_t wait = 0;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, wait);
        drain_ring();   /* events captured under the settings before a change go out under those settings */
        if (bits & HY_IB_APPLY) {
            take_pending();
        }
        net_check();
        if (bits & HY_IB_KICK) {
            for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
                hook_reset(i);
            }
        }
        drain_ring();
        mqtt_pass((bits & HY_IB_STATE) != 0, (bits & HY_IB_ADH) != 0);   /* before the webhooks: a slow POST never holds back an acknowledgement */
        if (bits & HY_IB_TEST) {
            run_test();
        }
        expire_paused();
        deliver();
        if (s_ob) {
            ob_flush();
        }
        publish_status((bits & HY_IB_TEST) != 0);
        wait = wait_ticks();
    }
}

/* ---------------------------------------------------------------- API */

esp_err_t hy_integ_init(void)
{
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
    hy_net_status_t ns;
    hy_net_status(&ns);
    memcpy(s_device, ns.ap_ssid, sizeof s_device);
    s_device[sizeof s_device - 1] = '\0';
    if (hy_store_path(s_ob_dir, sizeof s_ob_dir, "outbox") != HY_OK) {
        s_ob_dir[0] = '\0';
    }
    memcpy(&s_saved, hy_integ_cfg(), sizeof s_saved);
    memcpy(&s_cfg, &s_saved, sizeof s_cfg);
    publish_wanted();
    s_status.test_index = -1;
    if (!hy_clock_valid() && (atomic_load(&g_integ_wanted) & (1u << HY_IE_CLOCK_INVALID))) {
        /* The task does not run yet and the LVGL task publishes nothing before hy_ui_start: this is the only
         * producer at this point. */
        hy_hook_ev_t *e = ring_reserve();
        if (e) {
            fill_common(e, HY_IE_CLOCK_INVALID, 0);
            atomic_fetch_add_explicit(&g_integ_head, 1, memory_order_release);
        }
    }
    if (xTaskCreate(integ_task, "integ", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "no memory for the delivery task: integrations off");
        return ESP_ERR_NO_MEM;
    }
    notify(HY_IB_EV);
    bool locked = hy_board_display() && hy_board_lock(1000);
    int sub = hy_ev_subscribe(on_bus, NULL);
    if (locked) {
        hy_board_unlock();
    }
    if (sub < 0) {
        ESP_LOGE(TAG, "event subscribe failed: no events for integrations");
    }
    if (hy_net_subscribe(on_net, NULL) < 0) {
        ESP_LOGE(TAG, "network subscribe failed: delivery follows other wakes only");
    }
    return sub < 0 ? ESP_FAIL : ESP_OK;
}

void hy_integ_apply(const hy_integ_t *saved)
{
    if (!s_task || !saved) {
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    memcpy(&s_pending, saved, sizeof s_pending);
    s_pending_set = true;
    xSemaphoreGive(s_mx);
    notify(HY_IB_APPLY);
}

void hy_integ_status(hy_integ_status_t *out)
{
    if (!s_mx) {
        memset(out, 0, sizeof *out);
        out->test_index = -1;
        return;
    }
    int64_t now = mono_ms();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    *out = s_status;
    for (unsigned i = 0; i < HY_HOOK_MAX; i++) {
        if (s_retry_at_ms[i] > 0) {
            int64_t left = (s_retry_at_ms[i] - now + 999) / 1000;
            out->hook[i].retry_in_s = left > 0 ? (int32_t)left : 0;
        }
    }
    xSemaphoreGive(s_mx);
}

esp_err_t hy_integ_test(uint8_t index)
{
    if (!s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Read here: with every destination off the delivery task does not follow network changes */
    hy_net_status_t ns;
    hy_net_status(&ns);
    esp_err_t r = ESP_OK;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (index >= s_status.n_hooks || s_pub_free[index]) {
        r = ESP_ERR_INVALID_ARG;
    } else if (ns.state != HY_NS_CONNECTED) {
        r = ESP_ERR_INVALID_STATE;
    } else if (s_status.test_running || hy_heavy_owner()) {
        r = ESP_ERR_NOT_FINISHED;
    } else if (!hy_wh_heap_ok(s_pub_tls[index])) {
        r = ESP_ERR_NO_MEM;
    } else {
        s_status.test_running = true;
        s_status.test_index = (int8_t)index;
        s_status.test_http = 0;
        s_status.test_err = HY_HE_NONE;
    }
    xSemaphoreGive(s_mx);
    if (r == ESP_OK) {
        atomic_store(&s_test_req, index);
        notify(HY_IB_TEST);
    }
    return r;
}

int hy_integ_subscribe(void (*cb)(void *ctx), void *ctx)
{
    if (!cb || !s_mx) {
        return -1;
    }
    int slot = -1;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!s_subs[i].cb) {
            s_subs[i].cb = cb;
            s_subs[i].ctx = ctx;
            slot = i;
            break;
        }
    }
    xSemaphoreGive(s_mx);
    return slot;
}

/* ---------------------------------------------------------------- debug console */

#if CONFIG_HY_DEBUG_CONSOLE

static const char *next_word(char **p)
{
    char *s = *p;
    while (*s == ' ') s++;
    char *w = s;
    while (*s && *s != ' ') s++;
    if (*s) *s++ = '\0';
    while (*s == ' ') s++;
    *p = s;
    return w;
}

static void dbg_wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) {
        *v++ = 0;
    }
}

static void dbg_print(void)
{
    hy_integ_status_t st;
    hy_integ_status(&st);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    __typeof__(s_dbg) d = s_dbg;
    xSemaphoreGive(s_mx);
    hy_mq_debug_print();
    printf("integ: override %s; connected %s\n", st.override ? "yes" : "no", s_pub_connected ? "yes" : "no");
    for (unsigned i = 0; i < d.n_hooks && i < HY_HOOK_MAX; i++) {
        const dbg_hook_t *h = &d.hook[i];
        printf("integ: hook %u host %s, on %s, events 0x%03x, secret set %s, failures %u, retry in %d s, paused %s, "
               "pending %u, last %s %d\n",
               i, h->host, h->on ? "yes" : "no", (unsigned)h->events, h->secret_set ? "yes" : "no",
               (unsigned)h->failures, (int)st.hook[i].retry_in_s, h->paused ? "yes" : "no",
               (unsigned)st.hook[i].pending, hy_integ_err_name((hy_integ_err_t)st.hook[i].err), (int)st.hook[i].http);
    }
    printf("integ: outbox %s%s, live %u, pending mqtt %u, dropped %u, expired %u, rejected %u, corrupt %u, "
           "appends %u, state_writes %u, segments %u; lost %u\n",
           d.ob_open ? (d.ob_ram ? "RAM" : "file") : "closed", st.outbox_err ? " (storage error)" : "",
           (unsigned)d.st.live, (unsigned)d.st.pending[0], (unsigned)d.st.dropped, (unsigned)d.st.expired,
           (unsigned)d.st.rejected, (unsigned)d.st.corrupt, (unsigned)d.st.appends, (unsigned)d.st.state_writes,
           (unsigned)d.st.segments, (unsigned)st.lost);
    printf("integ: internal heap free %u, min %u; task stack free min %u bytes; test %s index %d status %d %s\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           s_task ? (unsigned)uxTaskGetStackHighWaterMark(s_task) : 0u, st.test_running ? "running" : "idle",
           (int)st.test_index, (int)st.test_http, hy_integ_err_name((hy_integ_err_t)st.test_err));
}

typedef struct {
    hy_iev_t type;
    char code[3];
    bool with_code;
    uint16_t n;
    uint16_t pushed;
} synth_ctx_t;

/* LVGL task (inside a job): synthetic captures, medicine copied read-only, no action runs. */
static void job_synth(void *arg)
{
    synth_ctx_t *c = arg;
    if (!atomic_load(&s_ovr_active)) {
        return;
    }
    int64_t ts = wall_now();
    for (unsigned k = 0; k < c->n; k++) {
        hy_hook_ev_t *e = ring_reserve();
        if (!e) {
            continue;
        }
        fill_common(e, c->type, ts);
        if (c->with_code) {
            fill_med(e, 0, c->code);
        }
        if (c->type == HY_IE_BATTERY_LOW || c->type == HY_IE_BATTERY_CRITICAL) {
            e->value = e->batt_pct;
        }
        ring_commit();
        c->pushed++;
    }
}

static unsigned synth_push(hy_iev_t type, const char *code, unsigned n)
{
    hy_job_t *j = hy_job_new(job_synth, sizeof(synth_ctx_t));
    if (!j) {
        return 0;
    }
    synth_ctx_t *c = hy_job_ctx(j);
    c->type = type;
    c->n = (uint16_t)n;
    if (code && code[0]) {
        strlcpy(c->code, code, sizeof c->code);
        c->with_code = true;
    }
    unsigned pushed = 0;
    if (hy_job_run(j, HY_JOB_TIMEOUT_MS) == ESP_OK) {
        pushed = c->pushed;
    }
    hy_job_put(j);
    return pushed;
}

static bool need_override(void)
{
    if (!atomic_load(&s_ovr_active)) {
        printf("integ: override first (integ hook <url> [secret] or integ mqtt <url> [base])\n");
        return false;
    }
    return true;
}

static void dbg_tcp(const char *host, const char *port)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    int g = getaddrinfo(host, port, &hints, &res);
    if (g != 0 || !res) {
        printf("integ: tcp %s: cannot resolve (%d)\n", host, g);
        return;
    }
    int s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0) {
        printf("integ: tcp: socket errno %d\n", errno);
        freeaddrinfo(res);
        return;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    int r = connect(s, res->ai_addr, res->ai_addrlen);
    int e = r == 0 ? 0 : errno;
    if (r != 0 && e == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval tv = {.tv_sec = 5};
        r = select(s + 1, NULL, &w, NULL, &tv);
        if (r > 0) {
            socklen_t l = sizeof e;
            getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &l);
        } else {
            e = r == 0 ? ETIMEDOUT : errno;
        }
    }
    if (e == 0) {
        printf("integ: tcp %s:%s ok\n", host, port);
    } else {
        printf("integ: tcp %s:%s errno %d\n", host, port, e);
    }
    close(s);
    freeaddrinfo(res);
}

void hy_integ_debug(const char *args)
{
    char buf[128];
    strlcpy(buf, args ? args : "", sizeof buf);
    char *rest = buf;
    const char *cmd = next_word(&rest);
    if (!s_task) {
        printf("integ: not running\n");
    } else if (!cmd[0]) {
        dbg_print();
    } else if (strcmp(cmd, "hook") == 0) {
        const char *url = next_word(&rest);
        const char *secret = next_word(&rest);
        bool tls;
        char host[65];
        if (!hy_wh_target(url, &tls, host) || hy_integ_check_cred(secret) != HY_OK) {
            printf("integ: usage: integ hook <http(s)://host[:port]/path> [secret]\n");
        } else {
            xSemaphoreTake(s_mx, portMAX_DELAY);
            hy_integ_defaults(&s_ovr_pending);
            s_ovr_pending.n_hooks = 1;
            s_ovr_pending.hook[0].on = true;
            strlcpy(s_ovr_pending.hook[0].url, url, sizeof s_ovr_pending.hook[0].url);
            s_ovr_pending.hook[0].events = HY_IE_ALL;
            strlcpy(s_ovr_pending.hook[0].secret, secret, sizeof s_ovr_pending.hook[0].secret);
            s_ovr_cmd = OVR_ON;
            xSemaphoreGive(s_mx);
            notify(HY_IB_APPLY);
            printf("integ: override hook %s://%s%s\n", tls ? "https" : "http", host, secret[0] ? ", secret set" : "");
        }
    } else if (strcmp(cmd, "mqtt") == 0) {
        const char *url = next_word(&rest);
        const char *base = next_word(&rest);
        if (!base[0]) {
            base = "hygeia-test";
        }
        hy_url_t u;
        if (hy_url_parse(url, true, &u) != HY_OK || hy_integ_check_topic(base) != HY_OK) {
            printf("integ: usage: integ mqtt <mqtt(s)://host[:port]> [base]\n");
        } else {
            xSemaphoreTake(s_mx, portMAX_DELAY);
            hy_integ_defaults(&s_ovr_pending);
            s_ovr_pending.mqtt.on = true;
            strlcpy(s_ovr_pending.mqtt.url, url, sizeof s_ovr_pending.mqtt.url);
            strlcpy(s_ovr_pending.mqtt.base, base, sizeof s_ovr_pending.mqtt.base);
            s_ovr_pending.mqtt.ha = false;
            s_ovr_cmd = OVR_ON;
            xSemaphoreGive(s_mx);
            notify(HY_IB_APPLY);
            printf("integ: override mqtt %s://%s:%u base %s, discovery off\n", u.tls ? "mqtts" : "mqtt", u.host,
                   (unsigned)u.port, base);
        }
    } else if (strcmp(cmd, "ha") == 0) {
        const char *arg = next_word(&rest);
        bool on = strcmp(arg, "on") == 0;
        if (!on && strcmp(arg, "off") != 0) {
            printf("integ: usage: integ ha <on|off>\n");
        } else if (need_override()) {
            xSemaphoreTake(s_mx, portMAX_DELAY);
            s_ha_cmd = on ? HA_ON : HA_OFF;
            xSemaphoreGive(s_mx);
            notify(HY_IB_APPLY);
            printf("integ: override discovery %s\n", on ? "on" : "off");
        }
    } else if (strcmp(cmd, "off") == 0) {
        atomic_store(&s_ovr_active, false);
        xSemaphoreTake(s_mx, portMAX_DELAY);
        if (s_ovr_cmd == OVR_ON) {
            dbg_wipe(&s_ovr_pending, sizeof s_ovr_pending);   /* a hook not yet taken: its secret goes */
        }
        s_ovr_cmd = OVR_OFF;
        xSemaphoreGive(s_mx);
        notify(HY_IB_APPLY);
        printf("integ: override off\n");
    } else if (strcmp(cmd, "fire") == 0) {
        const char *name = next_word(&rest);
        const char *code = next_word(&rest);
        hy_iev_t t;
        if (!hy_iev_from_name(name, &t)) {
            printf("integ: usage: integ fire <taken|skipped|missed|due|snoozed|undone|battery_low|battery_critical|"
                   "clock_invalid> [code]\n");
        } else if (need_override()) {
            printf("integ: fired %u\n", synth_push(t, code, 1));
        }
    } else if (strcmp(cmd, "fill") == 0 || strcmp(cmd, "burst") == 0) {
        bool fill = cmd[1] == 'i';
        long n = strtol(next_word(&rest), NULL, 10);
        long max = fill ? 1500 : HY_INTEG_RING;
        if (n < 1 || n > max) {
            printf("integ: usage: integ %s <1..%ld>\n", cmd, max);
        } else if (need_override()) {
            unsigned pushed = 0;
            if (fill) {
                for (long left = n; left > 0;) {
                    unsigned batch = left > 64 ? 64 : (unsigned)left;
                    /* wait for room, so the ring never overflows */
                    for (int w = 0; w < 500 && atomic_load(&g_integ_head) - atomic_load(&g_integ_tail) >
                                                   HY_INTEG_RING - batch; w++) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    pushed += synth_push(HY_IE_DUE, NULL, batch);
                    left -= batch;
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            } else {
                pushed = synth_push(HY_IE_DUE, NULL, (unsigned)n);
            }
            printf("integ: pushed %u, lost %u\n", pushed, (unsigned)atomic_load(&g_integ_lost));
        }
    } else if (strcmp(cmd, "kick") == 0) {
        notify(HY_IB_KICK);
        printf("integ: kick\n");
    } else if (strcmp(cmd, "sign") == 0) {
        const char *secret = next_word(&rest);
        char sig[72];
        if (!secret[0]) {
            printf("integ: usage: integ sign <secret> <text>\n");
        } else if (hy_wh_sign(secret, rest, strlen(rest), sig)) {
            printf("integ: X-Hygeia-Signature: %s\n", sig);
        } else {
            printf("integ: signature failed\n");
        }
    } else if (strcmp(cmd, "tcp") == 0) {
        const char *host = next_word(&rest);
        const char *port = next_word(&rest);
        if (!host[0] || !port[0]) {
            printf("integ: usage: integ tcp <host> <port>\n");
        } else {
            dbg_tcp(host, port);
        }
    } else {
        printf("integ: commands: (status) | hook <url> [secret] | mqtt <url> [base] | fire <event> [code] | fill <n> | "
               "burst <n> | kick | sign <secret> <text> | tcp <host> <port> | off\n");
    }
    dbg_wipe(buf, sizeof buf);   /* the line may hold a secret */
}

#endif /* CONFIG_HY_DEBUG_CONSOLE */
