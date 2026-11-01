/* Firmware update from GitHub releases: the release check and the download engine (see hy/update.h).
 *
 * One task per run, created by a user's press and deleted at its end. It runs below the LVGL task's priority and
 * calls nothing that writes the store, the settings or the scheduler. Status subscribers run in this task between I/O
 * steps and must not block; the screen's subscriber takes the board lock for at most 50 ms around one lv_async_call.
 * publish() is never called while a socket read or a flash write is in progress. Every
 * byte that reaches flash goes through hy_fwimg, which checks the image head (a hygeia image for this chip whose
 * version equals the checked tag) before the slot is touched; the SHA-256 of the downloaded bytes is compared with
 * the SHA256SUMS line before esp_ota_end, and only then does the boot partition change.
 *
 * HTTP: one esp_http_client per request (no keep-alive), the certificate bundle for the TLS roots, redirects followed
 * by hand so that every hop is checked against hy_fw_redirect_ok. The check reads only the redirect of
 * /releases/latest: no GitHub API call, no JSON. mbedTLS allocates in PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC); the
 * Location, URL, SHA256SUMS and download buffers are one PSRAM block. Certificate dates are not checked
 * (CONFIG_MBEDTLS_HAVE_TIME_DATE off), so a device whose clock is wrong can still update. */

#include "hy/update.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hy/fwimg.h"
#include "hy/net.h"
#include "hy/power_run.h"

static const char *TAG = "update";

#define UPD_STACK            8192                 /* internal: flash writes run from it */
#define UPD_PRIO             4                    /* the backup worker's priority (WORKER_PRIO in hy_web h_backup.c):
                                                     below the LVGL task (6) and the httpd task */
#define HY_UPD_MIN_INTERNAL  52224                /* 35 KB floor + 16 KB budget of a run + 1 KB rounding */
#define UPD_MIN_BLOCK        12288                /* largest internal block: the 8 KB stack plus the client buffers */
#define UPD_STALE_S          600                  /* an install needs a check younger than this */
#define UPD_MIN_BATT_PCT     30
#define UPD_TIMEOUT_MS       10000
#define UPD_DEADLINE_US      (300LL * 1000000)    /* whole install */
#define UPD_MAX_HOPS         3
#define UPD_CHECK_LOC_MAX    512                  /* Location of releases/latest, terminator included */
#define UPD_LOC_MAX          2048                 /* Location of an asset redirect (signed URLs), terminator included */
#define UPD_URL_MAX          2048
#define UPD_BUF              4096
#define UPD_ACTIVITY         (64 * 1024)          /* restart the radio's battery idle timer every 64 KB */
#define UPD_RESTART_MS       2000
#define UPD_GH               "https://github.com/"

typedef enum { RUN_CHECK = 0, RUN_INSTALL } run_kind_t;

typedef struct {
    char *loc;          /* Location of the last response, "" when none */
    size_t loc_max;     /* capacity of loc, terminator included */
    bool loc_long;      /* the Location did not fit */
} hdr_t;

/* One PSRAM block for a run (loc and url are 2 KB each; the block is never on a task stack). */
typedef struct {
    char loc[UPD_LOC_MAX];
    char url[UPD_URL_MAX];
    char sums[HY_FW_SUMS_MAX + 1];
    uint8_t buf[UPD_BUF];
} run_mem_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static hy_fw_status_t s_st;          /* under s_mux */
static atomic_bool s_running;        /* a check or install task exists */
static bool (*s_restart)(void);
static struct {
    void (*cb)(const hy_fw_status_t *st, void *ctx);
    void *ctx;
} s_subs[HY_UPDATE_MAX_SUBS];        /* under s_mux */
static char s_ua[48];
static char s_tag[32];               /* the tag an install downloads; written before its task starts */
static uint32_t s_last_hwm;          /* bytes of stack never used by the last run */
static int64_t s_last_ms;            /* duration of the last run */
#if CONFIG_HY_DEBUG_CONSOLE
static bool s_fakesum;
static int s_fakebatt = -1;          /* >= 0: the install rule sees the device on battery at this percent */
#endif

/* ---------------------------------------------------------------- status */

static int64_t now_s(void)
{
    return esp_timer_get_time() / 1000000;
}

static void publish(void)
{
    hy_fw_status_t st;
    void (*cb[HY_UPDATE_MAX_SUBS])(const hy_fw_status_t *, void *);
    void *ctx[HY_UPDATE_MAX_SUBS];
    portENTER_CRITICAL(&s_mux);
    st = s_st;
    for (int i = 0; i < HY_UPDATE_MAX_SUBS; i++) {
        cb[i] = s_subs[i].cb;
        ctx[i] = s_subs[i].ctx;
    }
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < HY_UPDATE_MAX_SUBS; i++) {
        if (cb[i]) {
            cb[i](&st, ctx[i]);
        }
    }
}

/* Update task: a state change, published. */
static void set_state(hy_fw_state_t s, hy_fw_err_t e, uint8_t pct)
{
    portENTER_CRITICAL(&s_mux);
    s_st.state = s;
    s_st.err = e;
    s_st.pct = pct;
    portEXIT_CRITICAL(&s_mux);
    publish();
}

/* Caller's task: an immediate refusal is stored as FAILED unless a run of ours owns the status. */
static hy_fw_err_t refuse(hy_fw_err_t e)
{
    portENTER_CRITICAL(&s_mux);
    if (!atomic_load(&s_running)) {
        s_st.state = HY_FWS_FAILED;
        s_st.err = e;
        s_st.pct = 0;
    }
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "refused: %s", hy_fw_err_code(e));
    return e;
}

/* ---------------------------------------------------------------- HTTP */

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    hdr_t *h = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && h && evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Location") == 0) {
        size_t n = strlen(evt->header_value);
        if (n >= h->loc_max) {
            h->loc_long = true;
            h->loc[0] = 0;
        } else {
            memcpy(h->loc, evt->header_value, n + 1);
        }
    }
    return ESP_OK;
}

/* The host of an https URL, for logs (signed asset URLs carry tokens in the query). */
static void log_host(const char *what, const char *url)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    int n = (int)strcspn(p, "/?#");
    ESP_LOGI(TAG, "%s %.*s", what, n, p);
}

/* A failed open or head read: no internet unless the TLS layer reports a certificate or handshake failure. */
static hy_fw_err_t net_err(esp_http_client_handle_t c, esp_err_t err)
{
    int code = 0, flags = 0;
    esp_err_t tls = esp_http_client_get_and_clear_last_tls_error(c, &code, &flags);
    ESP_LOGW(TAG, "connection failed: %s, tls %s (-0x%x, flags 0x%x)", esp_err_to_name(err), esp_err_to_name(tls),
             (unsigned)-code, (unsigned)flags);
    if (tls == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT ||
        (tls >= ESP_ERR_MBEDTLS_CERT_PARTLY_OK && tls < ESP_ERR_ESP_TLS_BASE + 0x30)) {
        return HY_FWE_TLS;
    }
    return HY_FWE_NO_INTERNET;   /* ESP_ERR_HTTP_CONNECT, ESP_ERR_HTTP_EAGAIN, cannot resolve, cannot connect */
}

static void close_client(esp_http_client_handle_t *c)
{
    if (*c) {
        esp_http_client_cleanup(*c);   /* closes the connection first */
        *c = NULL;
    }
}

/* Sends GET url and reads the response head. *out: the open client (close_client it); *status: the HTTP status. */
static hy_fw_err_t open_url(const char *url, hdr_t *hdr, esp_http_client_handle_t *out, int *status)
{
    *out = NULL;
    hdr->loc[0] = 0;
    hdr->loc_long = false;
    const esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_event,
        .user_data = hdr,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,          /* the signed asset URL after the redirect is over 512 bytes */
        .timeout_ms = UPD_TIMEOUT_MS,
        .keep_alive_enable = false,
        .user_agent = s_ua,
        .max_redirection_count = 0,
        .disable_auto_redirect = true,   /* redirects are followed by hand, each target checked */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGW(TAG, "http client: no memory");
        return HY_FWE_MEMORY;
    }
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        int64_t n = esp_http_client_fetch_headers(c);
        if (n < 0) {
            err = n == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_HTTP_EAGAIN : ESP_ERR_HTTP_FETCH_HEADER;
        }
    }
    if (err != ESP_OK) {
        hy_fw_err_t e = net_err(c, err);
        close_client(&c);
        return e;
    }
    *status = esp_http_client_get_status_code(c);
    *out = c;
    return HY_FWE_NONE;
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* GET m->url, following at most UPD_MAX_HOPS redirects that pass hy_fw_redirect_ok. *out: open client with status
   200. A 404, another status, a long or refused Location, or a fourth redirect: BAD_RELEASE. */
static hy_fw_err_t get_asset(run_mem_t *m, esp_http_client_handle_t *out)
{
    hdr_t hdr = {.loc = m->loc, .loc_max = sizeof m->loc};
    for (int hop = 0;; hop++) {
        int status = 0;
        hy_fw_err_t e = open_url(m->url, &hdr, out, &status);
        if (e != HY_FWE_NONE) {
            return e;
        }
        if (status == 200) {
            return HY_FWE_NONE;
        }
        close_client(out);
        if (!is_redirect(status)) {
            ESP_LOGW(TAG, "asset answered HTTP %d", status);
            return HY_FWE_BAD_RELEASE;
        }
        if (hop >= UPD_MAX_HOPS) {
            ESP_LOGW(TAG, "more than %d redirects", UPD_MAX_HOPS);
            return HY_FWE_BAD_RELEASE;
        }
        if (hdr.loc_long || !hdr.loc[0] || !hy_fw_redirect_ok(hdr.loc)) {
            ESP_LOGW(TAG, "redirect refused (%s)", hdr.loc_long ? "Location too long" : "target not allowed");
            return HY_FWE_BAD_RELEASE;
        }
        memcpy(m->url, hdr.loc, sizeof m->url);   /* both UPD_LOC_MAX; loc is terminated */
        log_host("redirect to", m->url);
    }
}

/* ---------------------------------------------------------------- check */

static hy_fw_err_t run_check(run_mem_t *m)
{
    esp_http_client_handle_t c = NULL;
    hdr_t hdr = {.loc = m->loc, .loc_max = UPD_CHECK_LOC_MAX};
    int status = 0;
    char tag[32];
    snprintf(m->url, sizeof m->url, UPD_GH "%s/releases/latest", CONFIG_HY_UPDATE_REPO);
    hy_fw_err_t e = open_url(m->url, &hdr, &c, &status);
    if (e != HY_FWE_NONE) {
        return e;
    }
    close_client(&c);   /* the body is not read */
    if (status == 404) {
        ESP_LOGW(TAG, "%s: HTTP 404", CONFIG_HY_UPDATE_REPO);
        return HY_FWE_NO_RELEASE;
    }
    if (status != 301 && status != 302) {
        ESP_LOGW(TAG, "releases/latest answered HTTP %d", status);
        return HY_FWE_BAD_RELEASE;
    }
    if (hdr.loc_long) {
        ESP_LOGW(TAG, "Location too long");
        return HY_FWE_BAD_RELEASE;
    }
    hy_err_t te = hy_fw_tag_from_location(hdr.loc, CONFIG_HY_UPDATE_REPO, tag, sizeof tag);
    if (te == HY_E_NOT_FOUND) {
        ESP_LOGI(TAG, "no release published");
        return HY_FWE_NO_RELEASE;
    }
    if (te != HY_OK) {
        ESP_LOGW(TAG, "unexpected Location %.96s", hdr.loc);
        return HY_FWE_BAD_RELEASE;
    }
    char installed[32];
    portENTER_CRITICAL(&s_mux);
    memcpy(installed, s_st.installed, sizeof installed);
    portEXIT_CRITICAL(&s_mux);
    hy_fw_offer_t offer = hy_fw_compare(installed, tag);
    int64_t t = now_s();
    portENTER_CRITICAL(&s_mux);
    memcpy(s_st.latest, tag, sizeof s_st.latest);
    s_st.checked_s = t > 0 ? t : 1;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "latest release %s, installed %s: %s", tag, installed,
             offer == HY_FW_UP_TO_DATE ? "up to date" : offer == HY_FW_NEWER ? "newer" : "development build");
    set_state(offer == HY_FW_UP_TO_DATE ? HY_FWS_UP_TO_DATE : HY_FWS_AVAILABLE, HY_FWE_NONE, 0);
    return HY_FWE_NONE;
}

/* ---------------------------------------------------------------- install */

static hy_fw_err_t img_err(hy_fwimg_res_t r)
{
    switch (r) {
    case HY_FWIMG_OK:           return HY_FWE_NONE;
    case HY_FWIMG_TOO_LARGE:    return HY_FWE_BAD_RELEASE;
    case HY_FWIMG_NOT_HYGEIA:
    case HY_FWIMG_VERSION:      return HY_FWE_MISMATCH;
    case HY_FWIMG_SELF_PENDING: return HY_FWE_SELF_PENDING;
    case HY_FWIMG_CHECKSUM:     return HY_FWE_CHECKSUM;
    case HY_FWIMG_CORRUPT:      return HY_FWE_CORRUPT;
    case HY_FWIMG_NO_MEM:       return HY_FWE_MEMORY;
    case HY_FWIMG_FLASH:
    default:                    return HY_FWE_FLASH;
    }
}

/* SHA256SUMS of the tag into sha. */
static hy_fw_err_t fetch_sums(run_mem_t *m, const char *tag, uint8_t sha[32])
{
    esp_http_client_handle_t c = NULL;
    snprintf(m->url, sizeof m->url, UPD_GH "%s/releases/download/%s/SHA256SUMS", CONFIG_HY_UPDATE_REPO, tag);
    hy_fw_err_t e = get_asset(m, &c);
    if (e != HY_FWE_NONE) {
        return e;
    }
    size_t got = 0;
    while (got <= HY_FW_SUMS_MAX) {
        int r = esp_http_client_read(c, m->sums + got, (int)(sizeof m->sums - got));
        if (r == 0) {
            break;
        }
        if (r < 0) {
            ESP_LOGW(TAG, "SHA256SUMS read failed: %s",
                     esp_err_to_name(r == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_HTTP_EAGAIN : ESP_FAIL));
            close_client(&c);
            return HY_FWE_NO_INTERNET;
        }
        got += (size_t)r;
    }
    close_client(&c);
    if (got > HY_FW_SUMS_MAX) {
        ESP_LOGW(TAG, "SHA256SUMS is over %d bytes", HY_FW_SUMS_MAX);
        return HY_FWE_BAD_RELEASE;
    }
    hy_err_t se = hy_fw_sums_find(m->sums, got, "hygeia.bin", sha);
    if (se != HY_OK) {
        ESP_LOGW(TAG, "SHA256SUMS: %s", se == HY_E_NOT_FOUND ? "no line for hygeia.bin" : "malformed");
        return HY_FWE_BAD_RELEASE;
    }
    return HY_FWE_NONE;
}

static hy_fw_err_t run_install(run_mem_t *m, int64_t t0)
{
    esp_http_client_handle_t c = NULL;
    hy_fwimg_t *w = NULL;
    uint8_t sha[32];
    hy_fw_err_t e;
    const char *tag = s_tag;
    int64_t deadline = t0 + UPD_DEADLINE_US;

    e = fetch_sums(m, tag, sha);
    if (e != HY_FWE_NONE) {
        return e;
    }
    snprintf(m->url, sizeof m->url, UPD_GH "%s/releases/download/%s/hygeia.bin", CONFIG_HY_UPDATE_REPO, tag);
    e = get_asset(m, &c);
    if (e != HY_FWE_NONE) {
        return e;
    }
    int64_t clen = esp_http_client_get_content_length(c);
    if (clen <= 0 || esp_http_client_is_chunked_response(c)) {
        ESP_LOGW(TAG, "hygeia.bin has no Content-Length");
        e = HY_FWE_BAD_RELEASE;
        goto fail;
    }
    size_t len = (size_t)clen;
    hy_fwimg_res_t r = hy_fwimg_begin(len, tag, &w);
    if (r != HY_FWIMG_OK) {
        ESP_LOGW(TAG, "image of %u bytes refused (%d)", (unsigned)len, (int)r);
        e = img_err(r);
        goto fail;
    }
    ESP_LOGI(TAG, "downloading %s hygeia.bin, %u bytes", tag, (unsigned)len);
    size_t done = 0, fill = 0, next_act = UPD_ACTIVITY;
    int timeouts = 0;
    uint8_t step = 0;
    while (done < len) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "download deadline passed at %u of %u bytes", (unsigned)(done + fill), (unsigned)len);
            e = HY_FWE_TIMEOUT;
            goto fail;
        }
        size_t want = UPD_BUF - fill;
        if (want > len - done - fill) {
            want = len - done - fill;
        }
        int n = esp_http_client_read(c, (char *)m->buf + fill, (int)want);
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            if (++timeouts >= 2) {
                ESP_LOGW(TAG, "two read timeouts in a row at %u bytes", (unsigned)(done + fill));
                e = HY_FWE_TIMEOUT;
                goto fail;
            }
            continue;
        }
        if (n < 0) {
            ESP_LOGW(TAG, "read failed at %u bytes: %s", (unsigned)(done + fill), esp_err_to_name(n));
            e = HY_FWE_NO_INTERNET;
            goto fail;
        }
        if (n == 0) {
            ESP_LOGW(TAG, "connection ended at %u of %u bytes", (unsigned)(done + fill), (unsigned)len);
            e = HY_FWE_CORRUPT;
            goto fail;
        }
        timeouts = 0;
        fill += (size_t)n;
        if (fill < UPD_BUF && done + fill < len) {
            continue;   /* full buffers only: the first write carries the whole image head */
        }
        r = hy_fwimg_write(w, m->buf, fill);
        if (r != HY_FWIMG_OK) {
            w = NULL;   /* the writer freed itself */
            e = img_err(r);
            goto fail;
        }
        done += fill;
        fill = 0;
        if (done >= next_act) {
            hy_net_activity();
            next_act += UPD_ACTIVITY;
        }
        uint8_t pct = (uint8_t)((uint64_t)done * 100 / len);
        if (pct / 5 != step) {
            step = pct / 5;
            set_state(HY_FWS_DOWNLOADING, HY_FWE_NONE, pct);
        }
    }
    close_client(&c);   /* frees the TLS session before the image check */
    set_state(HY_FWS_VERIFYING, HY_FWE_NONE, 100);
#if CONFIG_HY_DEBUG_CONSOLE
    if (s_fakesum) {
        sha[0] ^= 0x01;
        ESP_LOGW(TAG, "fakesum: expected hash altered");
    }
#endif
    r = hy_fwimg_finish(w, sha, NULL);
    w = NULL;
    if (r != HY_FWIMG_OK) {
        return img_err(r);
    }
    return HY_FWE_NONE;
fail:
    close_client(&c);
    hy_fwimg_abort(w);
    return e;
}

/* ---------------------------------------------------------------- task */

static void upd_task(void *arg)
{
    run_kind_t kind = (run_kind_t)(uintptr_t)arg;
    int64_t t0 = esp_timer_get_time();
    hy_fw_err_t e;
    publish();   /* CHECKING or DOWNLOADING, set by the caller */
    hy_net_activity();
    run_mem_t *m = heap_caps_malloc(sizeof *m, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) {
        e = HY_FWE_MEMORY;
    } else if (kind == RUN_CHECK) {
        e = run_check(m);
    } else {
        e = run_install(m, t0);
    }
    heap_caps_free(m);
    s_last_ms = (esp_timer_get_time() - t0) / 1000;
    s_last_hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    if (e != HY_FWE_NONE) {
        ESP_LOGW(TAG, "%s failed: %s", kind == RUN_CHECK ? "check" : "install", hy_fw_err_code(e));
        set_state(HY_FWS_FAILED, e, 0);
    } else if (kind == RUN_INSTALL) {
        /* the heavy slot stays held: nothing else may start before the restart */
        ESP_LOGI(TAG, "firmware %s installed, restarting in %d ms", s_tag, UPD_RESTART_MS);
        set_state(HY_FWS_RESTARTING, HY_FWE_NONE, 100);
        vTaskDelay(pdMS_TO_TICKS(UPD_RESTART_MS));
        /* through the LVGL task, the only task that writes the store, so no write is cut halfway */
        if (!s_restart || !s_restart()) {
            esp_restart();
        }
        vTaskDelete(NULL);   /* the restart runs in the LVGL task; this task is not needed any more */
        return;
    }
    if (kind == RUN_INSTALL) {
        hy_heavy_release();
    }
    atomic_store(&s_running, false);
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------- start */

static bool net_connected(void)
{
    hy_net_status_t ns;
    hy_net_status(&ns);
    return ns.state == HY_NS_CONNECTED;
}

static bool memory_ok(void)
{
    size_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (free < HY_UPD_MIN_INTERNAL || big < UPD_MIN_BLOCK) {
        ESP_LOGW(TAG, "internal heap %u free, largest block %u: below %u / %u", (unsigned)free, (unsigned)big,
                 (unsigned)HY_UPD_MIN_INTERNAL, (unsigned)UPD_MIN_BLOCK);
        return false;
    }
    return true;
}

static bool running_unconfirmed(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

/* The upload's rule: on battery below 30 %, or on battery with an unknown percent (-1), an install is refused. */
static bool battery_low(void)
{
#if CONFIG_HY_DEBUG_CONSOLE
    if (s_fakebatt >= 0) {
        return s_fakebatt < UPD_MIN_BATT_PCT;
    }
#endif
    hy_power_state_t ps;
    hy_power_state(&ps);
    return ps.pmu_ok && !ps.batt.raw.vbus && ps.batt.pct < UPD_MIN_BATT_PCT;
}

static bool start_task(run_kind_t kind)
{
    return xTaskCreatePinnedToCore(upd_task, "hy_update", UPD_STACK, (void *)(uintptr_t)kind, UPD_PRIO, NULL,
                                   tskNO_AFFINITY) == pdPASS;
}

hy_fw_err_t hy_update_check(void)
{
    if (!net_connected()) {
        return refuse(HY_FWE_NO_NETWORK);
    }
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_running, &idle, true)) {
        return refuse(HY_FWE_BUSY);   /* our own run keeps its status */
    }
    if (hy_heavy_owner()) {
        atomic_store(&s_running, false);
        return refuse(HY_FWE_BUSY);
    }
    if (!memory_ok()) {
        atomic_store(&s_running, false);
        return refuse(HY_FWE_MEMORY);
    }
    portENTER_CRITICAL(&s_mux);
    s_st.state = HY_FWS_CHECKING;
    s_st.err = HY_FWE_NONE;
    s_st.pct = 0;
    portEXIT_CRITICAL(&s_mux);
    if (!start_task(RUN_CHECK)) {
        atomic_store(&s_running, false);
        return refuse(HY_FWE_MEMORY);
    }
    ESP_LOGI(TAG, "check of %s started", CONFIG_HY_UPDATE_REPO);
    return HY_FWE_NONE;
}

hy_fw_err_t hy_update_install(const char *version)
{
    if (atomic_load(&s_running)) {
        return refuse(HY_FWE_BUSY);
    }
    bool fresh;
    int64_t t = now_s();
    portENTER_CRITICAL(&s_mux);
    fresh = version && s_st.state == HY_FWS_AVAILABLE && s_st.latest[0] &&
            strncmp(version, s_st.latest, sizeof s_st.latest) == 0 && s_st.checked_s > 0 &&
            t - s_st.checked_s < UPD_STALE_S;
    portEXIT_CRITICAL(&s_mux);
    if (!fresh) {
        return refuse(HY_FWE_STALE);
    }
    if (!net_connected()) {
        return refuse(HY_FWE_NO_NETWORK);
    }
    if (running_unconfirmed()) {
        return refuse(HY_FWE_SELF_PENDING);
    }
    if (battery_low()) {
        return refuse(HY_FWE_BATTERY);
    }
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_running, &idle, true)) {
        return refuse(HY_FWE_BUSY);
    }
    if (!hy_heavy_claim("update")) {
        atomic_store(&s_running, false);
        return refuse(HY_FWE_BUSY);
    }
    if (!memory_ok()) {
        hy_heavy_release();
        atomic_store(&s_running, false);
        return refuse(HY_FWE_MEMORY);
    }
    strncpy(s_tag, version, sizeof s_tag - 1);
    s_tag[sizeof s_tag - 1] = 0;
    portENTER_CRITICAL(&s_mux);
    s_st.state = HY_FWS_DOWNLOADING;
    s_st.err = HY_FWE_NONE;
    s_st.pct = 0;
    portEXIT_CRITICAL(&s_mux);
    if (!start_task(RUN_INSTALL)) {
        hy_heavy_release();
        atomic_store(&s_running, false);
        return refuse(HY_FWE_MEMORY);
    }
    ESP_LOGI(TAG, "install of %s started", s_tag);
    return HY_FWE_NONE;
}

/* ---------------------------------------------------------------- the rest of the API */

esp_err_t hy_update_init(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    portENTER_CRITICAL(&s_mux);
    memset(&s_st, 0, sizeof s_st);
    s_st.state = HY_FWS_IDLE;
    strncpy(s_st.installed, d->version, sizeof s_st.installed - 1);
    s_st.installed_release = hy_fw_parse_release(s_st.installed, NULL);
    portEXIT_CRITICAL(&s_mux);
    snprintf(s_ua, sizeof s_ua, "hygeia/%.31s", d->version);
    ESP_LOGI(TAG, "installed %s (%s), updates from %s", d->version,
             hy_fw_parse_release(d->version, NULL) ? "release" : "development build", CONFIG_HY_UPDATE_REPO);
    return ESP_OK;
}

void hy_update_set_restart(bool (*restart)(void))
{
    s_restart = restart;
}

void hy_update_status(hy_fw_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

int hy_update_subscribe(void (*cb)(const hy_fw_status_t *st, void *ctx), void *ctx)
{
    int slot = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < HY_UPDATE_MAX_SUBS; i++) {
        if (!s_subs[i].cb) {
            s_subs[i].cb = cb;
            s_subs[i].ctx = ctx;
            slot = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return slot;
}

/* ---------------------------------------------------------------- debug */

#if CONFIG_HY_DEBUG_CONSOLE
void hy_update_debug(const char *args)
{
    while (args && *args == ' ') {
        args++;
    }
    if (!args || !*args) {
        hy_fw_status_t st;
        hy_update_status(&st);
        const char *code = hy_fw_err_code(st.err);
        printf("upd: %s%s%s, installed %s (%s), latest %s, %u %%, checked %s%lld s ago\n", hy_fw_state_name(st.state),
               code ? " " : "", code ? code : "", st.installed, st.installed_release ? "release" : "dev",
               st.latest[0] ? st.latest : "-", st.pct, st.checked_s ? "" : "never, ",
               st.checked_s ? (long long)(now_s() - st.checked_s) : 0LL);
        const char *owner = hy_heavy_owner();
        printf("upd: internal free %u, min %u, largest %u; last run stack unused %u of %u, %lld ms; heavy %s; "
               "repo %s; fakesum %s, batt %d\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)s_last_hwm, UPD_STACK,
               (long long)s_last_ms, owner ? owner : "free", CONFIG_HY_UPDATE_REPO, s_fakesum ? "on" : "off",
               s_fakebatt);
        return;
    }
    if (strcmp(args, "check") == 0) {
        hy_fw_err_t e = hy_update_check();
        printf("upd check: %s\n", e == HY_FWE_NONE ? "started" : hy_fw_err_code(e));
        return;
    }
    if (strcmp(args, "install") == 0) {
        hy_fw_status_t st;
        hy_update_status(&st);
        hy_fw_err_t e = hy_update_install(st.latest);
        printf("upd install %s: %s\n", st.latest[0] ? st.latest : "-",
               e == HY_FWE_NONE ? "started" : hy_fw_err_code(e));
        return;
    }
    if (strncmp(args, "fakesum ", 8) == 0) {
        s_fakesum = strcmp(args + 8, "on") == 0;
        printf("upd fakesum %s\n", s_fakesum ? "on" : "off");
        return;
    }
    if (strncmp(args, "batt ", 5) == 0) {
        const char *v = args + 5;
        if (strcmp(v, "off") == 0) {
            s_fakebatt = -1;
            printf("upd batt off: the install rule reads the real battery\n");
        } else {
            int p = atoi(v);
            s_fakebatt = p < 0 ? 0 : p > 100 ? 100 : p;
            printf("upd batt %d: the install rule sees the device on battery at %d %%\n", s_fakebatt, s_fakebatt);
        }
        return;
    }
    printf("upd [check|install|fakesum on|off|batt <pct>|off]\n");
}
#endif
