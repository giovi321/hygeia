#include "hy/net.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "hy/power_run.h"
#include "hy/sched_run.h"
#include "lvgl.h"
#include "mdns.h"
#include "net_priv.h"

#define TASK_STACK  4096
#define TASK_PRIO   3          /* below the LVGL task (6) */
#define QUEUE_LEN   16
#define TICK_MS     1000
#define SLOT_S      15         /* per network in a pass */
#define RETRY_S     3          /* a failed attempt inside a slot is retried after this */
#define RSSI_EVERY_S 5
#define USB_WAIT_S  5          /* the first radio decision waits this long for a PMU reading */
#define NTP_SERVER  "pool.ntp.org"
#define START_RETRY_S 30       /* a radio that failed to start is tried again after this */
#define DISC_SWALLOW_MS 1500   /* a disconnect event this soon after our own call is ours */

static const char *TAG = "net";
static const char *TAG_NTP = "ntp";
/* DHCP option 114: esp_netif keeps the pointer, so the string is static */
static const char k_portal_uri[] = "http://192.168.4.1/";

typedef enum {
    M_MODE, M_LIST, M_AP_RESTART, M_HOST, M_NTP,
    M_STA_START, M_STA_CONN, M_STA_DISC, M_GOT_IP, M_LOST_IP, M_AP_STA,
} msg_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t reason;   /* M_STA_DISC */
    int64_t val;      /* M_GOT_IP: ip (network order); M_NTP: utc */
} msg_t;

/* ---- shared state ---- */
static SemaphoreHandle_t s_mu;   /* s_cfg, s_pin, s_st, s_subs; never held across an NVS write */
static SemaphoreHandle_t s_save_mu;   /* orders NVS writes: taken before s_mu, held across the write */
static net_cfg_t s_cfg;
static char s_pin[33];           /* the SSID hy_net_set_mode asked for first, "" none */
static hy_net_status_t s_st;
static struct {
    void (*cb)(const hy_net_status_t *st, void *ctx);
    void *ctx;
} s_subs[HY_NET_MAX_SUBS];
static int s_nsubs;
static QueueHandle_t s_q;
static TaskHandle_t s_task;
static bool s_inited;
static esp_netif_t *s_sta, *s_ap;
static char s_ap_ssid[12];

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   /* the fields below */
static int64_t s_activity_s = INT64_MIN / 2;
static bool s_keep_batt;
static bool s_start_req;          /* hy_net_set_mode ran: counts as started on battery */
static int64_t s_ntp_utc;         /* the latest SNTP time, read by the LVGL job */
static int64_t s_ntp_mono_us;     /* esp_timer time when s_ntp_utc was received */

/* ---- net task state ---- */
static struct {
    hy_net_mode_t applied;        /* what the radio runs */
    bool held;                    /* hy_power_net_hold(true) is ours */
    bool wifi_init;
    hy_radio_in_t in;
    bool decided;                 /* the first radio decision was taken */
    int64_t t_start;
    /* client */
    bool sta_started, got_ip, waiting, link_down, expect_disc;
    int64_t disc_ms;              /* when expect_disc was set (esp_timer ms) */
    uint8_t order[HY_NET_MAX_KNOWN], order_n, idx;
    int64_t slot_t0, last_connect, wait_until, stretch_t0, last_rssi;
    uint32_t passes;
    uint32_t ip;
    int8_t rssi;
    char cur[33];                 /* SSID of the slot or of the connection */
    uint8_t last_reason;
    bool sntp_on, mdns_on;
    bool ntp_pending;             /* an SNTP time waits for the LVGL job to be queued */
    int64_t retry_at;             /* a failed radio start is retried from this time (mono s); 0 = none */
    bool start_failed;            /* the last radio start failed; logged once until a start works */
    /* access point */
    uint8_t ap_clients;
    hy_net_state_t last_state;
    bool last_state_set;
} r;

static int64_t mono_s(void)
{
    return esp_timer_get_time() / 1000000;
}

static void lock(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mu);
}

static bool post_msg(msg_kind_t kind, uint8_t reason, int64_t val)
{
    if (!s_q) {
        return false;
    }
    msg_t m = {.kind = (uint8_t)kind, .reason = reason, .val = val};
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, message %u dropped", (unsigned)kind);
        return false;
    }
    return true;
}

static int64_t mono_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Our own esp_wifi_disconnect: the DISCONNECTED event it causes is not a failure */
static void own_disconnect(void)
{
    r.expect_disc = true;
    r.disc_ms = mono_ms();
    esp_wifi_disconnect();
}

static const char *mode_name(hy_net_mode_t m)
{
    return m == HY_NET_CLIENT ? "client" : m == HY_NET_AP ? "ap" : "off";
}

static const char *state_name(hy_net_state_t s)
{
    static const char *const n[] = {"off", "paused", "connecting", "connected", "failing", "access point"};
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "?";
}

/* ---- events (system event task): only queue them ---- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START: post_msg(M_STA_START, 0, 0); break;
        case WIFI_EVENT_STA_CONNECTED: post_msg(M_STA_CONN, 0, 0); break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *d = data;
            post_msg(M_STA_DISC, d ? d->reason : 0, 0);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
        case WIFI_EVENT_AP_STADISCONNECTED: post_msg(M_AP_STA, 0, 0); break;
        default: break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            const ip_event_got_ip_t *g = data;
            post_msg(M_GOT_IP, 0, g ? (int64_t)g->ip_info.ip.addr : 0);
        } else if (id == IP_EVENT_STA_LOST_IP) {
            post_msg(M_LOST_IP, 0, 0);
        }
    }
}

/* ---- NTP ---- */

/* Replaces lwip's weak default, which would call settimeofday behind hy_clock's back: the time goes through
 * hy_clock_set in the LVGL task instead (RTC write, sync sequence, scheduler). Runs in the tcpip task. */
void sntp_sync_time(struct timeval *tv)
{
    if (tv && post_msg(M_NTP, 0, (int64_t)tv->tv_sec)) {
        sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
    }
}

/* LVGL task (or the net task when there is no display). The time elapsed since the SNTP reply is added. */
static void ntp_apply(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    int64_t utc = s_ntp_utc, at = s_ntp_mono_us;
    portEXIT_CRITICAL(&s_mux);
    utc += (esp_timer_get_time() - at) / 1000000;
    esp_err_t err = hy_clock_set(utc, NULL, "ntp");
    if (err == ESP_OK) {
        hy_sched_run_invalidate();
    } else {
        ESP_LOGE(TAG_NTP, "clock set: %s", esp_err_to_name(err));
    }
}

/* Queues the clock set to the LVGL task, the only writer of the clock and the scheduler. Without a display there is
 * no LVGL task and the net task sets it. false: not queued, the next tick tries again. */
static bool ntp_post(void)
{
    if (!hy_board_display()) {
        ntp_apply(NULL);
        return true;
    }
    if (!hy_board_lock(1000)) {
        return false;
    }
    lv_result_t res = lv_async_call(ntp_apply, NULL);
    hy_board_unlock();
    return res == LV_RESULT_OK;
}

static void ntp_received(int64_t utc)
{
    ESP_LOGI(TAG_NTP, "synced, utc %lld", (long long)utc);
    portENTER_CRITICAL(&s_mux);
    s_ntp_utc = utc;
    s_ntp_mono_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);
    r.ntp_pending = !ntp_post();
    if (r.ntp_pending) {
        ESP_LOGW(TAG_NTP, "ui busy, clock set retried on the next tick");
    }
}

static void sntp_on(void)
{
    if (r.sntp_on) {
        return;
    }
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    c.wait_for_sync = false;
    esp_err_t err = esp_netif_sntp_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NTP, "init: %s", esp_err_to_name(err));
        return;
    }
    r.sntp_on = true;
}

static void sntp_off(void)
{
    if (r.sntp_on) {
        esp_netif_sntp_deinit();
        r.sntp_on = false;
    }
}

/* ---- mDNS (client mode only) ---- */

static void mdns_on(void)
{
    if (r.mdns_on) {
        return;
    }
    char host[33];
    lock();
    strlcpy(host, s_cfg.host, sizeof host);
    unlock();
    esp_err_t err = mdns_init();
    if (err == ESP_OK) err = mdns_hostname_set(host);
    if (err == ESP_OK) err = mdns_instance_name_set("Hygeia");
    if (err == ESP_OK) err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns: %s", esp_err_to_name(err));
        mdns_free();
        return;
    }
    r.mdns_on = true;
    ESP_LOGI(TAG, "mdns: %s.local", host);
}

static void mdns_off(void)
{
    if (r.mdns_on) {
        mdns_free();
        r.mdns_on = false;
    }
}

/* ---- radio ---- */

static void hold(bool on)
{
    if (r.held != on) {
        hy_power_net_hold(on);
        r.held = on;
    }
}

static esp_err_t wifi_up(wifi_mode_t mode)
{
    esp_err_t err = ESP_OK;
    if (!r.wifi_init) {
        wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
        c.nvs_enable = 0;   /* NVS namespace "wifi" is the only copy of the credentials */
        err = esp_wifi_init(&c);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi init: %s", esp_err_to_name(err));
            return err;
        }
        r.wifi_init = true;
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi mode: %s", esp_err_to_name(err));
    }
    return err;
}

/* Everything off: services, the radio, the driver (its buffers go back to the heap). */
static void radio_down(void)
{
    sntp_off();
    mdns_off();
    net_captive_stop();
    if (r.wifi_init) {
        esp_wifi_stop();
        esp_wifi_deinit();
        r.wifi_init = false;
    }
    r.sta_started = r.got_ip = r.waiting = r.link_down = r.expect_disc = false;
    r.ip = 0;
    r.rssi = 0;
    r.cur[0] = 0;
    r.ap_clients = 0;
}

static void set_ps(void)
{
    if (r.applied == HY_NET_CLIENT && r.wifi_init) {
        esp_wifi_set_ps(r.in.usb ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    }
}

/* ---- client ---- */

static void connect_slot(int64_t now)
{
    wifi_config_t c;
    memset(&c, 0, sizeof c);
    bool found = false;
    lock();
    uint8_t i = r.order[r.idx];
    if (i < s_cfg.n) {
        memcpy(c.sta.ssid, s_cfg.e[i].ssid, strnlen(s_cfg.e[i].ssid, sizeof c.sta.ssid));
        memcpy(c.sta.password, s_cfg.e[i].pass, strnlen(s_cfg.e[i].pass, sizeof c.sta.password));
        strlcpy(r.cur, s_cfg.e[i].ssid, sizeof r.cur);
        c.sta.threshold.authmode = s_cfg.e[i].pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        found = true;
    }
    unlock();
    r.slot_t0 = r.last_connect = now;
    r.link_down = false;
    if (!found) {
        memset(&c, 0, sizeof c);
        return;   /* the list changed: M_LIST restarts the pass */
    }
    c.sta.scan_method = WIFI_FAST_SCAN;
    c.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &c);
    memset(&c, 0, sizeof c);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    ESP_LOGI(TAG, "trying %s (%u of %u): %s", r.cur, (unsigned)r.idx + 1, (unsigned)r.order_n, esp_err_to_name(err));
}

/* A pass over the known networks in try order, the asked-for SSID first. */
static void begin_pass(int64_t now)
{
    hy_known_net_t k[HY_NET_MAX_KNOWN];
    size_t n;
    char pin[33];
    lock();
    n = s_cfg.n;
    for (size_t i = 0; i < n; i++) {
        strlcpy(k[i].ssid, s_cfg.e[i].ssid, sizeof k[i].ssid);
        k[i].last_ok = s_cfg.e[i].ok;
    }
    strlcpy(pin, s_pin, sizeof pin);
    unlock();
    r.order_n = (uint8_t)hy_net_try_order(k, n, r.order);
    if (pin[0]) {
        for (uint8_t j = 0; j < r.order_n; j++) {
            if (strcmp(k[r.order[j]].ssid, pin) == 0) {
                uint8_t first = r.order[j];
                memmove(&r.order[1], &r.order[0], j);
                r.order[0] = first;
                break;
            }
        }
    }
    r.idx = 0;
    r.waiting = false;
    if (r.order_n == 0) {
        r.waiting = true;
        r.wait_until = INT64_MAX;   /* nothing to try until a network is added */
        r.cur[0] = 0;
        ESP_LOGW(TAG, "client mode with no known network");
        return;
    }
    connect_slot(now);
}

static void next_slot(int64_t now)
{
    r.idx++;
    if (r.idx < r.order_n) {
        connect_slot(now);
        return;
    }
    r.passes++;
    uint32_t wait = hy_net_backoff_s(r.passes);
    r.waiting = true;
    r.wait_until = now + wait;
    r.cur[0] = 0;
    ESP_LOGI(TAG, "no network connected (last reason %u), next pass in %" PRIu32 " s", (unsigned)r.last_reason,
             wait);
}

static void restart_client(int64_t now)
{
    if (r.got_ip) {
        sntp_off();
        r.got_ip = false;
        r.ip = 0;
    }
    if (r.sta_started) {
        own_disconnect();
        r.passes = 0;
        r.stretch_t0 = now;
        begin_pass(now);
    }
}

static void client_tick(int64_t now)
{
    if (!r.sta_started) {
        return;
    }
    if (r.got_ip) {
        if (now - r.last_rssi >= RSSI_EVERY_S) {
            r.last_rssi = now;
            int rssi = 0;
            if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
                r.rssi = (int8_t)rssi;
            }
        }
        return;
    }
    if (r.waiting) {
        if (now >= r.wait_until) {
            begin_pass(now);
        }
        return;
    }
    if (now - r.slot_t0 >= SLOT_S) {
        own_disconnect();
        next_slot(now);
        return;
    }
    if (r.link_down && now - r.last_connect >= RETRY_S && now - r.slot_t0 <= SLOT_S - RETRY_S) {
        r.link_down = false;
        r.last_connect = now;
        esp_wifi_connect();
    }
}

static void on_got_ip(uint32_t ip, int64_t now)
{
    r.got_ip = true;
    r.waiting = false;
    r.expect_disc = false;
    r.ip = ip;
    r.passes = 0;
    r.last_rssi = now - RSSI_EVERY_S;
    int idx = -1;
    uint32_t ok = 0, seq = 0;
    xSemaphoreTake(s_save_mu, portMAX_DELAY);
    lock();
    for (uint8_t i = 0; i < s_cfg.n; i++) {
        if (strcmp(s_cfg.e[i].ssid, r.cur) == 0) {
            s_cfg.e[i].ok = ++s_cfg.okseq;
            idx = i;
            ok = s_cfg.e[i].ok;
            seq = s_cfg.okseq;
            break;
        }
    }
    unlock();
    if (idx >= 0) {
        net_nvs_save_ok((uint8_t)idx, ok, seq);
    }
    xSemaphoreGive(s_save_mu);
    sntp_on();
    set_ps();
}

static void on_disconnect(uint8_t reason, int64_t now)
{
    r.last_reason = reason;
    if (r.expect_disc) {
        r.expect_disc = false;
        if (mono_ms() - r.disc_ms <= DISC_SWALLOW_MS) {
            return;
        }
    }
    if (r.got_ip) {
        ESP_LOGW(TAG, "disconnected from %s, reason %u", r.cur, (unsigned)reason);
        sntp_off();
        r.got_ip = false;
        r.ip = 0;
        r.rssi = 0;
        r.passes = 0;
        r.stretch_t0 = now;
        begin_pass(now);
        return;
    }
    r.link_down = true;
}

/* ---- access point ---- */

static bool ap_start(void)
{
    if (wifi_up(WIFI_MODE_AP) != ESP_OK) {
        return false;
    }
    wifi_config_t c;
    memset(&c, 0, sizeof c);
    lock();
    memcpy(c.ap.password, s_cfg.ap_pass, strnlen(s_cfg.ap_pass, sizeof c.ap.password));
    unlock();
    size_t sl = strlen(s_ap_ssid);
    memcpy(c.ap.ssid, s_ap_ssid, sl);
    c.ap.ssid_len = (uint8_t)sl;
    c.ap.channel = 1;
    c.ap.authmode = WIFI_AUTH_WPA2_PSK;
    c.ap.max_connection = 4;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &c);
    memset(&c, 0, sizeof c);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "access point start: %s", esp_err_to_name(err));
        return false;
    }
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(s_ap, &ip);
    /* DHCP: the device is the DNS server (captive DNS) and the portal URI (option 114) */
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    uint8_t offer = OFFER_DNS;
    esp_netif_dhcps_stop(s_ap);
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof offer);
    esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns);
    err = esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, (void *)k_portal_uri,
                                 strlen(k_portal_uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dhcp portal uri: %s", esp_err_to_name(err));
    }
    esp_netif_dhcps_start(s_ap);
    r.ip = ip.ip.addr;
    if (net_captive_start(ip.ip.addr) != ESP_OK) {
        ESP_LOGE(TAG, "captive dns not started");
    }
    return true;
}

static void ap_count(void)
{
    wifi_sta_list_t l;
    if (esp_wifi_ap_get_sta_list(&l) == ESP_OK) {
        r.ap_clients = (uint8_t)l.num;
    }
}

/* ---- mode changes ---- */

static bool client_start(int64_t now)
{
    r.stretch_t0 = now;
    r.passes = 0;
    if (wifi_up(WIFI_MODE_STA) != ESP_OK) {
        return false;
    }
    set_ps();
    esp_err_t err = esp_wifi_start();   /* WIFI_EVENT_STA_START begins the first pass */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client start: %s", esp_err_to_name(err));
        return false;
    }
    mdns_on();   /* a failure here leaves the client without mDNS, not without a network */
    return true;
}

/* A start failed: everything down, the hold released, another try after START_RETRY_S */
static void start_failed(hy_net_mode_t want, int64_t now)
{
    radio_down();
    r.applied = HY_NET_OFF;
    hold(false);
    r.retry_at = now + START_RETRY_S;
    if (!r.start_failed) {
        ESP_LOGE(TAG, "%s did not start, retry every %d s", mode_name(want), START_RETRY_S);
    }
    r.start_failed = true;
}

static void apply(hy_net_mode_t want, int64_t now)
{
    if (want == r.applied) {
        return;
    }
    if (want == HY_NET_OFF) {
        radio_down();
        r.applied = HY_NET_OFF;
        hold(false);
        r.retry_at = 0;
        r.start_failed = false;
        return;
    }
    if (r.applied == HY_NET_OFF && r.retry_at && now < r.retry_at) {
        return;
    }
    radio_down();
    hold(true);   /* before the radio starts: the sleep mode ends and is not entered while the radio runs */
    r.applied = want;
    bool ok = want == HY_NET_CLIENT ? client_start(now) : ap_start();
    if (!ok) {
        start_failed(want, now);
        return;
    }
    r.retry_at = 0;
    r.start_failed = false;
}

/* ---- status ---- */

static hy_net_state_t state_now(hy_net_mode_t saved, int64_t now)
{
    switch (r.applied) {
    case HY_NET_AP: return HY_NS_AP;
    case HY_NET_CLIENT:
        if (r.got_ip) return HY_NS_CONNECTED;
        return now - r.stretch_t0 >= HY_NET_HINT_S ? HY_NS_FAILING : HY_NS_CONNECTING;
    default:
        if (saved == HY_NET_OFF) return HY_NS_OFF;
        return r.start_failed ? HY_NS_FAILING : HY_NS_PAUSED;
    }
}

static void publish(int64_t now)
{
    hy_net_status_t st;
    memset(&st, 0, sizeof st);
    lock();
    st.saved = s_cfg.mode;
    strlcpy(st.host, s_cfg.host, sizeof st.host);
    unlock();
    st.state = state_now(st.saved, now);
    if (st.state == HY_NS_CONNECTED) {
        strlcpy(st.ssid, r.cur, sizeof st.ssid);
        st.rssi = r.rssi;
        st.ip = r.ip;
    } else if (st.state == HY_NS_AP) {
        st.ip = r.ip;
        st.ap_clients = r.ap_clients;
    }
    strlcpy(st.ap_ssid, s_ap_ssid, sizeof st.ap_ssid);

    if (!r.last_state_set || st.state != r.last_state) {
        r.last_state_set = true;
        r.last_state = st.state;
        esp_ip4_addr_t a = {.addr = st.ip};
        switch (st.state) {
        case HY_NS_CONNECTED: ESP_LOGI(TAG, "connected to %s, ip " IPSTR, st.ssid, IP2STR(&a)); break;
        case HY_NS_AP: ESP_LOGI(TAG, "access point %s, ip " IPSTR, st.ap_ssid, IP2STR(&a)); break;
        case HY_NS_FAILING:
            if (r.start_failed) {
                ESP_LOGW(TAG, "failing: the radio did not start");
            } else {
                ESP_LOGW(TAG, "failing: no connection for %d s, mode stays client", HY_NET_HINT_S);
            }
            break;
        default: ESP_LOGI(TAG, "%s", state_name(st.state)); break;
        }
    }

    bool changed;
    void (*cbs[HY_NET_MAX_SUBS])(const hy_net_status_t *, void *);
    void *ctxs[HY_NET_MAX_SUBS];
    int nsubs;
    lock();
    hy_net_status_t old = s_st;
    old.rssi = st.rssi;   /* rssi alone is not a change */
    changed = memcmp(&old, &st, sizeof st) != 0;
    s_st = st;
    nsubs = s_nsubs;
    for (int i = 0; i < nsubs; i++) {
        cbs[i] = s_subs[i].cb;
        ctxs[i] = s_subs[i].ctx;
    }
    unlock();
    if (changed) {
        for (int i = 0; i < nsubs; i++) {
            cbs[i](&st, ctxs[i]);
        }
    }
}

/* ---- the 1 s evaluation ---- */

static void tick(int64_t now)
{
    hy_power_state_t ps;
    hy_power_state(&ps);
    bool usb = ps.pmu_ok ? ps.batt.raw.vbus : true;
    if (!r.decided) {
        if (!ps.pmu_ok && now - r.t_start < USB_WAIT_S) {
            return;   /* the first PMU reading decides between USB and battery */
        }
        r.decided = true;
        r.in.usb = usb;
    }
    r.in.now = now;
    if (usb != r.in.usb) {
        bool radio_runs = r.applied != HY_NET_OFF;
        r.in.usb = usb;
        if (usb || radio_runs) {
            hy_net_on_usb_change(&r.in, usb);
        }
        ESP_LOGI(TAG, "usb %s", usb ? "in" : "out");
        set_ps();
    }
    portENTER_CRITICAL(&s_mux);
    bool start = s_start_req;
    s_start_req = false;
    int64_t act = s_activity_s;
    r.in.keep_on_battery = s_keep_batt;
    portEXIT_CRITICAL(&s_mux);
    if (start && !usb) {
        r.in.started_on_battery = true;
    }
    if (act > r.in.last_activity) {
        r.in.last_activity = act;
    }
    lock();
    r.in.saved = s_cfg.mode;
    unlock();
    r.in.idle_s = HY_NET_BATT_IDLE_S;

    hy_net_mode_t want = hy_net_radio(&r.in);
    if (want == HY_NET_OFF && !usb && r.in.started_on_battery) {
        /* The idle rule ended the battery run: a later touch or web request does not bring the radio back, only
         * hy_net_set_mode or USB does */
        r.in.started_on_battery = false;
    }
    apply(want, now);
    if (r.applied == HY_NET_CLIENT) {
        client_tick(now);
    }
    if (r.ntp_pending) {
        r.ntp_pending = !ntp_post();
    }
    publish(now);
}

static void handle(const msg_t *m, int64_t now)
{
    switch (m->kind) {
    case M_MODE:
        if (r.applied == HY_NET_CLIENT && r.decided) {
            char pin[33];
            lock();
            strlcpy(pin, s_pin, sizeof pin);
            unlock();
            if (!r.got_ip || (pin[0] && strcmp(pin, r.cur) != 0)) {
                restart_client(now);   /* asked again: no waiting out the backoff */
            }
        }
        break;
    case M_LIST:
        if (r.applied == HY_NET_CLIENT) {
            bool known = false;
            lock();
            for (uint8_t i = 0; i < s_cfg.n; i++) {
                known = known || strcmp(s_cfg.e[i].ssid, r.cur) == 0;
            }
            unlock();
            if (!r.got_ip || !known) {
                restart_client(now);
            }
        }
        break;
    case M_AP_RESTART:
        if (r.applied == HY_NET_AP) {
            radio_down();
            if (!ap_start()) {
                start_failed(HY_NET_AP, now);
            }
        }
        break;
    case M_HOST: {
        char host[33];
        lock();
        strlcpy(host, s_cfg.host, sizeof host);
        unlock();
        esp_netif_set_hostname(s_sta, host);
        esp_netif_set_hostname(s_ap, host);
        if (r.mdns_on) {
            mdns_hostname_set(host);
        }
        break;
    }
    case M_NTP: ntp_received(m->val); break;
    case M_STA_START:
        if (r.applied == HY_NET_CLIENT) {
            r.sta_started = true;
            begin_pass(now);
        }
        break;
    case M_STA_CONN:
        if (r.applied == HY_NET_CLIENT) {
            r.link_down = false;
            r.expect_disc = false;
        }
        break;
    case M_STA_DISC:
        if (r.applied == HY_NET_CLIENT) {
            on_disconnect(m->reason, now);
        }
        break;
    case M_GOT_IP:
        if (r.applied == HY_NET_CLIENT) {
            on_got_ip((uint32_t)m->val, now);
        }
        break;
    case M_LOST_IP:
        if (r.applied == HY_NET_CLIENT && r.got_ip) {
            ESP_LOGW(TAG, "ip lost");
            r.stretch_t0 = now;
            restart_client(now);   /* still associated: disconnect first, then a new pass */
        }
        break;
    case M_AP_STA:
        if (r.applied == HY_NET_AP) {
            ap_count();
        }
        break;
    default: break;
    }
}

static void net_task(void *arg)
{
    (void)arg;
    r.t_start = mono_s();
    int64_t last_tick = INT64_MIN / 2;
    for (;;) {
        msg_t m;
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t wait = TICK_MS - (now_ms - last_tick);
        if (wait < 0) {
            wait = 0;
        }
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(wait)) == pdTRUE) {
            handle(&m, mono_s());
            if (r.decided) {
                publish(mono_s());
            }
        }
        now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_tick >= TICK_MS) {
            last_tick = now_ms;
            tick(mono_s());
        }
    }
}

/* ---- public ---- */

esp_err_t hy_net_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    /* A retry after a failed call reuses what the first call created */
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
    if (!s_save_mu) s_save_mu = xSemaphoreCreateMutex();
    if (!s_q) s_q = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    if (!s_mu || !s_save_mu || !s_q) {
        return ESP_ERR_NO_MEM;
    }
    static bool loaded, handlers;
    if (!loaded) {
        net_nvs_load(&s_cfg);
        loaded = true;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }
    if (!s_sta) s_sta = esp_netif_create_default_wifi_sta();
    if (!s_ap) s_ap = esp_netif_create_default_wifi_ap();
    if (!s_sta || !s_ap) {
        ESP_LOGE(TAG, "netif create failed");
        return ESP_FAIL;
    }
    esp_netif_set_hostname(s_sta, s_cfg.host);
    esp_netif_set_hostname(s_ap, s_cfg.host);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    hy_net_ap_ssid(mac, s_ap_ssid);
    if (!handlers) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL);
        handlers = true;
    }

    s_st.saved = s_cfg.mode;
    s_st.state = s_cfg.mode == HY_NET_OFF ? HY_NS_OFF : HY_NS_PAUSED;
    strlcpy(s_st.ap_ssid, s_ap_ssid, sizeof s_st.ap_ssid);
    strlcpy(s_st.host, s_cfg.host, sizeof s_st.host);
    r.applied = HY_NET_OFF;
    r.in.idle_s = HY_NET_BATT_IDLE_S;
    r.in.last_activity = INT64_MIN / 2;
    ESP_LOGI(TAG, "init: mode %s, %u known networks, host %s, access point %s", mode_name(s_cfg.mode),
             (unsigned)s_cfg.n, s_cfg.host, s_ap_ssid);
    s_inited = true;
    return ESP_OK;
}

void hy_net_start(void)
{
    if (!s_inited || s_task) {
        return;
    }
    if (xTaskCreate(net_task, "net", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "task create failed");
    }
}

void hy_net_status(hy_net_status_t *out)
{
    if (!s_inited) {
        memset(out, 0, sizeof *out);
        return;
    }
    lock();
    *out = s_st;
    unlock();
}

/* A copy of the configuration for an NVS write outside s_mu; PSRAM when there is some */
static net_cfg_t *snap_alloc(void)
{
    net_cfg_t *c = heap_caps_malloc(sizeof *c, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return c ? c : malloc(sizeof *c);
}

static void snap_free(net_cfg_t *c)
{
    if (c) {
        memset(c, 0, sizeof *c);
        free(c);
    }
}

static int find(const char *ssid)
{
    for (uint8_t i = 0; i < s_cfg.n; i++) {
        if (strcmp(s_cfg.e[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t hy_net_set_mode(hy_net_mode_t m, const char *ssid)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((unsigned)m > HY_NET_AP) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_save_mu, portMAX_DELAY);
    lock();
    if (m == HY_NET_CLIENT && ssid && ssid[0] && find(ssid) < 0) {
        unlock();
        xSemaphoreGive(s_save_mu);
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(s_pin, (m == HY_NET_CLIENT && ssid) ? ssid : "", sizeof s_pin);
    bool changed = s_cfg.mode != m;
    s_cfg.mode = m;
    unlock();
    if (changed) {
        err = net_nvs_save_mode(m);
    }
    xSemaphoreGive(s_save_mu);
    portENTER_CRITICAL(&s_mux);
    s_start_req = true;
    s_activity_s = mono_s();
    portEXIT_CRITICAL(&s_mux);
    bool pinned = m == HY_NET_CLIENT && ssid && ssid[0];
    ESP_LOGI(TAG, "mode %s%s%s", mode_name(m), pinned ? ", first " : "", pinned ? ssid : "");
    post_msg(M_MODE, 0, 0);
    return err;
}

size_t hy_net_known(hy_known_net_t *out)
{
    if (!s_inited) {
        return 0;
    }
    lock();
    size_t n = s_cfg.n;
    for (size_t i = 0; i < n; i++) {
        strlcpy(out[i].ssid, s_cfg.e[i].ssid, sizeof out[i].ssid);
        out[i].last_ok = s_cfg.e[i].ok;
    }
    unlock();
    return n;
}

esp_err_t hy_net_add(const char *ssid, const char *pass)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ssid || !pass || hy_net_check_ssid(ssid) != HY_OK || hy_net_check_pass(pass, false) != HY_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    net_cfg_t *snap = snap_alloc();
    if (!snap) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_save_mu, portMAX_DELAY);
    lock();
    int i = find(ssid);
    if (i < 0) {
        if (s_cfg.n >= HY_NET_MAX_KNOWN) {
            unlock();
            xSemaphoreGive(s_save_mu);
            snap_free(snap);
            return ESP_ERR_NO_MEM;
        }
        i = s_cfg.n++;
        memset(&s_cfg.e[i], 0, sizeof s_cfg.e[i]);
        strlcpy(s_cfg.e[i].ssid, ssid, sizeof s_cfg.e[i].ssid);
    }
    strlcpy(s_cfg.e[i].pass, pass, sizeof s_cfg.e[i].pass);
    *snap = s_cfg;
    unlock();
    esp_err_t err = net_nvs_save_list(snap);
    xSemaphoreGive(s_save_mu);
    snap_free(snap);
    ESP_LOGI(TAG, "network %s saved", ssid);
    post_msg(M_LIST, 0, 0);
    return err;
}

esp_err_t hy_net_forget(const char *ssid)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    net_cfg_t *snap = snap_alloc();
    if (!snap) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_save_mu, portMAX_DELAY);
    lock();
    int i = find(ssid);
    if (i < 0) {
        unlock();
        xSemaphoreGive(s_save_mu);
        snap_free(snap);
        return ESP_ERR_NOT_FOUND;
    }
    memmove(&s_cfg.e[i], &s_cfg.e[i + 1], (size_t)(s_cfg.n - i - 1) * sizeof s_cfg.e[0]);
    s_cfg.n--;
    memset(&s_cfg.e[s_cfg.n], 0, sizeof s_cfg.e[0]);
    if (strcmp(s_pin, ssid) == 0) {
        s_pin[0] = 0;
    }
    *snap = s_cfg;
    unlock();
    esp_err_t err = net_nvs_save_list(snap);
    xSemaphoreGive(s_save_mu);
    snap_free(snap);
    ESP_LOGI(TAG, "network %s forgotten", ssid);
    post_msg(M_LIST, 0, 0);
    return err;
}

esp_err_t hy_net_ap_pass_get(char out[64])
{
    if (!s_inited) {
        out[0] = 0;
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    strlcpy(out, s_cfg.ap_pass, 64);
    unlock();
    return ESP_OK;
}

esp_err_t hy_net_ap_pass_set(const char *pass)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!pass || hy_net_check_pass(pass, true) != HY_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_save_mu, portMAX_DELAY);
    lock();
    strlcpy(s_cfg.ap_pass, pass, sizeof s_cfg.ap_pass);
    unlock();
    esp_err_t err = net_nvs_save_str("ap_pass", pass);
    xSemaphoreGive(s_save_mu);
    ESP_LOGI(TAG, "access point password changed");
    post_msg(M_AP_RESTART, 0, 0);
    return err;
}

esp_err_t hy_net_host_set(const char *host)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!net_host_valid(host)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_save_mu, portMAX_DELAY);
    lock();
    strlcpy(s_cfg.host, host, sizeof s_cfg.host);
    unlock();
    esp_err_t err = net_nvs_save_str("host", host);
    xSemaphoreGive(s_save_mu);
    ESP_LOGI(TAG, "host %s", host);
    post_msg(M_HOST, 0, 0);
    return err;
}

void hy_net_activity(void)
{
    int64_t now = mono_s();
    portENTER_CRITICAL(&s_mux);
    s_activity_s = now;
    portEXIT_CRITICAL(&s_mux);
}

void hy_net_set_keep_batt(bool keep)
{
    portENTER_CRITICAL(&s_mux);
    s_keep_batt = keep;
    portEXIT_CRITICAL(&s_mux);
}

int hy_net_subscribe(void (*cb)(const hy_net_status_t *st, void *ctx), void *ctx)
{
    if (!s_inited || !cb) {
        return -1;
    }
    lock();
    int slot = -1;
    if (s_nsubs < HY_NET_MAX_SUBS) {
        slot = s_nsubs++;
        s_subs[slot].cb = cb;
        s_subs[slot].ctx = ctx;
    }
    unlock();
    return slot;
}

/* ---- debug ---- */

#if CONFIG_HY_DEBUG_CONSOLE
static void debug_status(void)
{
    hy_net_status_t st;
    hy_net_status(&st);
    esp_ip4_addr_t a = {.addr = st.ip};
    printf("wifi: mode %s, state %s, ssid '%s', rssi %d, ip " IPSTR ", ap %s (%u clients), host %s.local\n",
           mode_name(st.saved), state_name(st.state), st.ssid, st.rssi, IP2STR(&a), st.ap_ssid,
           (unsigned)st.ap_clients, st.host);
    hy_known_net_t k[HY_NET_MAX_KNOWN];
    size_t n = hy_net_known(k);
    for (size_t i = 0; i < n; i++) {
        printf("wifi: known %u '%s' last_ok %" PRIu32 "\n", (unsigned)i, k[i].ssid, k[i].last_ok);
    }
}

/* Read without the task's cooperation: a snapshot for the console, fields may be one tick apart. */
static void debug_batt(void)
{
    hy_radio_in_t in = r.in;
    int64_t now = mono_s();
    in.now = now;
    portENTER_CRITICAL(&s_mux);
    in.keep_on_battery = s_keep_batt;
    portEXIT_CRITICAL(&s_mux);
    int64_t idle_left = in.started_on_battery ? (int64_t)in.idle_s - (now - in.last_activity) : 0;
    printf("wifi batt: saved %s, usb %d, keep_on_battery %d, started_on_battery %d, idle left %lld s, "
           "radio %s, applied %s, decided %d\n",
           mode_name(in.saved), in.usb, in.keep_on_battery, in.started_on_battery,
           (long long)(idle_left > 0 ? idle_left : 0), mode_name(hy_net_radio(&in)), mode_name(r.applied),
           r.decided);
}

void hy_net_debug(const char *args)
{
    char buf[112];
    strlcpy(buf, args ? args : "", sizeof buf);
    char *rest = buf;
    char *cmd = rest;
    while (*rest && *rest != ' ') rest++;
    if (*rest) {
        *rest++ = 0;
        while (*rest == ' ') rest++;
    }
    esp_err_t err;
    if (!cmd[0]) {
        debug_status();
        return;
    }
    if (strcmp(cmd, "mode") == 0) {
        char *ssid = rest;
        char *m = ssid;
        while (*ssid && *ssid != ' ') ssid++;
        if (*ssid) {
            *ssid++ = 0;
            while (*ssid == ' ') ssid++;
        }
        hy_net_mode_t mode;
        if (strcmp(m, "off") == 0) {
            mode = HY_NET_OFF;
        } else if (strcmp(m, "client") == 0) {
            mode = HY_NET_CLIENT;
        } else if (strcmp(m, "ap") == 0) {
            mode = HY_NET_AP;
        } else {
            printf("usage: wifi mode off|client|ap [ssid]\n");
            return;
        }
        err = hy_net_set_mode(mode, ssid[0] ? ssid : NULL);
        printf("wifi mode %s: %s\n", m, esp_err_to_name(err));
    } else if (strcmp(cmd, "add") == 0) {
        /* the SSID is one word; the password is the rest of the line ("" for an open network) */
        char *pass = rest;
        char *ssid = rest;
        while (*pass && *pass != ' ') pass++;
        if (*pass) {
            *pass++ = 0;
        }
        if (!ssid[0]) {
            printf("usage: wifi add <ssid> [pass]\n");
            return;
        }
        err = hy_net_add(ssid, pass);
        memset(buf, 0, sizeof buf);
        printf("wifi add: %s\n", esp_err_to_name(err));
    } else if (strcmp(cmd, "forget") == 0) {
        err = hy_net_forget(rest);
        printf("wifi forget: %s\n", esp_err_to_name(err));
    } else if (strcmp(cmd, "appass") == 0) {
        char p[64];
        hy_net_ap_pass_get(p);
        printf("wifi appass: %s %s\n", s_ap_ssid, p);   /* console only, never the log */
        memset(p, 0, sizeof p);
    } else if (strcmp(cmd, "batt") == 0) {
        debug_batt();
    } else {
        printf("usage: wifi [mode off|client|ap [ssid] | add <ssid> [pass] | forget <ssid> | appass | batt]\n");
    }
}
#endif
