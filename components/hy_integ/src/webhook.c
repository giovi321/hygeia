/* Webhook transport: one HTTP(S) POST per call, the HMAC-SHA256 signature header, the heap gate. The retry state lives
 * in integ_run.c. Nothing here logs a URL path or query, a secret or a payload. */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls_errors.h"
#include "integ_int.h"
#include "psa/crypto.h"

static const char *TAG = "integ_wh";

#define WH_TIMEOUT_MS 10000
#define WH_BODY_MAX   1024   /* response bytes read and discarded */

/* The URL handed to the client, scheme and host lower-cased; and a small sink for the response body. PSRAM: the
 * delivery task's stack holds neither. */
static HY_EXT_BSS char s_url[HY_INTEG_URL_MAX + 1];
static HY_EXT_BSS char s_sink[256];
static char s_ua[48];

bool hy_wh_target(const char *url, bool *tls, char host[65])
{
    hy_url_t u;
    if (!url || hy_url_parse(url, false, &u) != HY_OK) {
        return false;
    }
    if (tls) {
        *tls = u.tls;
    }
    if (host) {
        memcpy(host, u.host, sizeof u.host);
    }
    return true;
}

bool hy_wh_heap_ok(bool tls)
{
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return free_int >= (tls ? HY_WH_HEAP_HTTPS : HY_WH_HEAP_HTTP);
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) {
        *v++ = 0;
    }
}

bool hy_wh_sign(const char *secret, const char *body, size_t len, char out[72])
{
    static bool s_psa_ok;
    if (!s_psa_ok) {
        if (psa_crypto_init() != PSA_SUCCESS) {
            ESP_LOGW(TAG, "psa_crypto_init failed");
            return false;
        }
        s_psa_ok = true;
    }
    size_t klen = strlen(secret);
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
    psa_key_id_t key = 0;
    if (psa_import_key(&attr, (const uint8_t *)secret, klen, &key) != PSA_SUCCESS) {
        psa_reset_key_attributes(&attr);
        ESP_LOGW(TAG, "signature key import failed");
        return false;
    }
    psa_reset_key_attributes(&attr);
    uint8_t mac[32];
    size_t mlen = 0;
    psa_status_t st = psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), (const uint8_t *)body, len, mac, sizeof mac,
                                      &mlen);
    psa_destroy_key(key);   /* wipes the key copy PSA holds */
    bool ok = st == PSA_SUCCESS && mlen == sizeof mac;
    if (ok) {
        hy_hook_sig_header(mac, out);
    } else {
        ESP_LOGW(TAG, "signature failed (%d)", (int)st);
    }
    wipe(mac, sizeof mac);
    return ok;
}

/* The URL with scheme and host lower-cased (the path keeps its case). The validated URL has no userinfo, so the
 * authority runs from "://" to the first '/'. */
static void canonical_url(const char *url)
{
    size_t n = strlen(url);
    if (n >= sizeof s_url) {
        n = sizeof s_url - 1;
    }
    memcpy(s_url, url, n);
    s_url[n] = '\0';
    char *p = strstr(s_url, "://");
    char *end = p ? strchr(p + 3, '/') : NULL;
    if (!end) {
        end = s_url + n;
    }
    for (char *q = s_url; q < end; q++) {
        *q = (char)tolower((unsigned char)*q);
    }
}

static hy_integ_err_t transport_err(esp_http_client_handle_t c, esp_err_t err)
{
    int code = 0, flags = 0;
    esp_err_t tls = esp_http_client_get_and_clear_last_tls_error(c, &code, &flags);
    if (err == ESP_ERR_HTTP_EAGAIN || tls == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT ||
        tls == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT) {
        return HY_HE_TIMEOUT;
    }
    if (tls >= ESP_ERR_MBEDTLS_CERT_PARTLY_OK && tls < ESP_ERR_ESP_TLS_BASE + 0x30) {
        return HY_HE_TLS;
    }
    return HY_HE_CONNECT;
}

int hy_wh_post(const char *url, const char *secret, const char *body, size_t len, hy_integ_err_t *err)
{
    *err = HY_HE_NONE;
    bool tls = false;
    char host[65];
    if (!hy_wh_target(url, &tls, host)) {
        *err = HY_HE_CONNECT;
        return -1;
    }
    if (!s_ua[0]) {
        snprintf(s_ua, sizeof s_ua, "hygeia/%s", esp_app_get_description()->version);
    }
    char sig[72];
    bool signed_body = secret && secret[0];
    if (signed_body && !hy_wh_sign(secret, body, len, sig)) {
        *err = HY_HE_MEMORY;
        return -1;
    }
    canonical_url(url);
    const esp_http_client_config_t cfg = {
        .url = s_url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
        .timeout_ms = WH_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
        .disable_auto_redirect = true,
        .max_redirection_count = 0,
        .user_agent = s_ua,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGW(TAG, "%s://%s: no memory for the client", tls ? "https" : "http", host);
        *err = HY_HE_MEMORY;
        return -1;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    if (signed_body) {
        esp_http_client_set_header(c, "X-Hygeia-Signature", sig);
    }
    int status = -1;
    esp_err_t e = esp_http_client_open(c, (int)len);
    if (e == ESP_OK) {
        int w = esp_http_client_write(c, body, (int)len);
        if (w != (int)len) {
            e = ESP_FAIL;
        }
    }
    if (e == ESP_OK) {
        int64_t h = esp_http_client_fetch_headers(c);
        if (h < 0) {
            e = h == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_HTTP_EAGAIN : ESP_ERR_HTTP_FETCH_HEADER;
        }
    }
    if (e == ESP_OK) {
        status = esp_http_client_get_status_code(c);
        for (int got = 0; got < WH_BODY_MAX;) {
            int r = esp_http_client_read(c, s_sink, sizeof s_sink);
            if (r <= 0) {
                break;
            }
            got += r;
        }
    } else {
        *err = transport_err(c, e);
        ESP_LOGW(TAG, "%s://%s: %s (%s)", tls ? "https" : "http", host, hy_integ_err_name(*err), esp_err_to_name(e));
    }
    esp_http_client_cleanup(c);
    wipe(sig, sizeof sig);
    return status;
}
