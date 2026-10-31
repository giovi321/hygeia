/* Paired browsers: NVS namespace "web", keys "t0".."t3", one blob per browser holding the SHA-256 of its session
 * token. The token itself only ever lives in the browser's cookie. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hy/auth.h"
#include "hy/clock.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "web_internal.h"

static const char *TAG = "web_sess";
static const char *NS = "web";

typedef struct {
    uint8_t sha[32];
    int64_t created;
    char label[WEB_LABEL_LEN];
} sess_blob_t;

static struct {
    sess_blob_t b;
    bool used;
    uint32_t gen;
} s_slot[WEB_SESS_MAX];

static SemaphoreHandle_t s_mx;
static StaticSemaphore_t s_mx_buf;

static void lock(void) { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

esp_err_t web_sha256(const uint8_t *in, size_t len, uint8_t out[32])
{
    size_t olen = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, in, len, out, 32, &olen);
    return (st == PSA_SUCCESS && olen == 32) ? ESP_OK : ESP_FAIL;
}

static void key_name(int slot, char out[8])
{
    snprintf(out, 8, "t%d", (slot & 7));
}

esp_err_t web_sess_init(void)
{
    if (!s_mx) {
        s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
    }
    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init: %d", (int)ps);
        return ESP_FAIL;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    lock();
    memset(s_slot, 0, sizeof s_slot);
    if (err == ESP_OK) {
        for (int i = 0; i < WEB_SESS_MAX; i++) {
            char k[8];
            key_name(i, k);
            size_t len = sizeof(sess_blob_t);
            if (nvs_get_blob(h, k, &s_slot[i].b, &len) == ESP_OK && len == sizeof(sess_blob_t)) {
                s_slot[i].b.label[WEB_LABEL_LEN - 1] = 0;
                s_slot[i].used = true;
            } else {
                memset(&s_slot[i].b, 0, sizeof s_slot[i].b);
            }
        }
        nvs_close(h);
    }
    size_t n = 0;
    for (int i = 0; i < WEB_SESS_MAX; i++) {
        n += s_slot[i].used;
    }
    unlock();
    ESP_LOGI(TAG, "%u paired browser(s)", (unsigned)n);
    return (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

int web_sess_check(const char *cookie_header, uint32_t *gen)
{
    if (!cookie_header) {
        return -1;
    }
    char tok[65];
    if (!hy_cookie_get(cookie_header, "hy_s", tok, sizeof tok) || !hy_token_well_formed(tok)) {
        return -1;
    }
    uint8_t raw[32], sha[32];
    for (int i = 0; i < 32; i++) {
        raw[i] = (uint8_t)(hexval(tok[2 * i]) << 4 | hexval(tok[2 * i + 1]));
    }
    memset(tok, 0, sizeof tok);
    esp_err_t err = web_sha256(raw, sizeof raw, sha);
    memset(raw, 0, sizeof raw);
    if (err != ESP_OK) {
        return -1;
    }
    int found = -1;
    uint32_t g = 0;
    lock();
    for (int i = 0; i < WEB_SESS_MAX; i++) {
        /* every slot is compared, used or not, with no early exit */
        bool eq = hy_ct_equal(sha, s_slot[i].b.sha, 32);
        if (eq && s_slot[i].used && found < 0) {
            found = i;
            g = s_slot[i].gen;
        }
    }
    unlock();
    if (gen) {
        *gen = g;
    }
    return found;
}

/* "Firefox on Windows": browser and system words only, nothing else from the User-Agent */
static void ua_label(const char *ua, char out[WEB_LABEL_LEN])
{
    const char *b = "Browser", *os = NULL;
    if (ua) {
        if (strstr(ua, "Edg/") || strstr(ua, "EdgA/") || strstr(ua, "EdgiOS/")) {
            b = "Edge";
        } else if (strstr(ua, "OPR/") || strstr(ua, "Opera")) {
            b = "Opera";
        } else if (strstr(ua, "Firefox/") || strstr(ua, "FxiOS/")) {
            b = "Firefox";
        } else if (strstr(ua, "SamsungBrowser/")) {
            b = "Samsung Internet";
        } else if (strstr(ua, "Chrome/") || strstr(ua, "CriOS/") || strstr(ua, "Chromium/")) {
            b = "Chrome";
        } else if (strstr(ua, "Safari/")) {
            b = "Safari";
        } else if (strstr(ua, "curl/")) {
            b = "curl";
        }
        if (strstr(ua, "iPhone") || strstr(ua, "iPad")) {
            os = "iOS";
        } else if (strstr(ua, "Android")) {
            os = "Android";
        } else if (strstr(ua, "CrOS")) {
            os = "ChromeOS";
        } else if (strstr(ua, "Windows")) {
            os = "Windows";
        } else if (strstr(ua, "Macintosh") || strstr(ua, "Mac OS X")) {
            os = "macOS";
        } else if (strstr(ua, "Linux")) {
            os = "Linux";
        }
    }
    strlcpy(out, b, WEB_LABEL_LEN);   /* at most "Samsung Internet on ChromeOS" cut to 23 characters */
    if (os) {
        strlcat(out, " on ", WEB_LABEL_LEN);
        strlcat(out, os, WEB_LABEL_LEN);
    }
}

static esp_err_t save_slot(nvs_handle_t h, int i)
{
    char k[8];
    key_name(i, k);
    return s_slot[i].used ? nvs_set_blob(h, k, &s_slot[i].b, sizeof(sess_blob_t)) : nvs_erase_key(h, k);
}

esp_err_t web_sess_add(const uint8_t token[32], const char *user_agent, int *slot)
{
    sess_blob_t nb;
    memset(&nb, 0, sizeof nb);
    esp_err_t err = web_sha256(token, 32, nb.sha);
    if (err != ESP_OK) {
        return err;
    }
    nb.created = hy_clock_valid() ? (int64_t)time(NULL) : 0;
    ua_label(user_agent, nb.label);

    nvs_handle_t h;
    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
    lock();
    int pick = -1;
    for (int i = 0; i < WEB_SESS_MAX && pick < 0; i++) {
        if (!s_slot[i].used) {
            pick = i;
        }
    }
    if (pick < 0) {   /* the oldest goes; an unknown date (0) counts as oldest, ties take the lowest slot */
        pick = 0;
        for (int i = 1; i < WEB_SESS_MAX; i++) {
            if (s_slot[i].b.created < s_slot[pick].b.created) {
                pick = i;
            }
        }
        ESP_LOGI(TAG, "fifth browser: slot %d (%s) replaced", pick, s_slot[pick].b.label);
    }
    sess_blob_t old = s_slot[pick].b;
    bool old_used = s_slot[pick].used;
    s_slot[pick].b = nb;
    s_slot[pick].used = true;
    err = save_slot(h, pick);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        s_slot[pick].gen++;
    } else {
        s_slot[pick].b = old;
        s_slot[pick].used = old_used;
    }
    unlock();
    nvs_close(h);
    memset(&nb, 0, sizeof nb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: %s", esp_err_to_name(err));
        return err;
    }
    if (slot) {
        *slot = pick;
    }
    ESP_LOGI(TAG, "browser paired in slot %d", pick);
    return ESP_OK;
}

size_t web_sess_list(web_sess_info_t *out, size_t max)
{
    size_t n = 0;
    lock();
    for (int i = 0; i < WEB_SESS_MAX && n < max; i++) {
        if (s_slot[i].used) {
            out[n].slot = i;
            out[n].created = s_slot[i].b.created;
            strlcpy(out[n].label, s_slot[i].b.label, sizeof out[n].label);
            n++;
        }
    }
    unlock();
    return n;
}

esp_err_t web_sess_forget(int slot)
{
    if (slot < -1 || slot >= WEB_SESS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    if (slot >= 0 && !s_slot[slot].used) {
        err = ESP_ERR_NOT_FOUND;
    }
    for (int i = 0; i < WEB_SESS_MAX && err == ESP_OK; i++) {
        if ((slot < 0 || i == slot) && s_slot[i].used) {
            /* RAM first: a failed erase still ends the session until the next boot */
            s_slot[i].used = false;
            memset(&s_slot[i].b, 0, sizeof s_slot[i].b);
            s_slot[i].gen++;
            esp_err_t e = save_slot(h, i);
            if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
                err = e;
            }
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    unlock();
    nvs_close(h);
    if (err != ESP_ERR_NOT_FOUND) {
        web_ws_sessions_changed();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "forgot %s", slot < 0 ? "every browser" : "one browser");
    } else if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "forget: %s", esp_err_to_name(err));
    }
    return err;
}

size_t web_sess_count(void)
{
    if (!s_mx) {
        return 0;
    }
    size_t n = 0;
    lock();
    for (int i = 0; i < WEB_SESS_MAX; i++) {
        n += s_slot[i].used;
    }
    unlock();
    return n;
}

bool web_sess_valid(int slot, uint32_t gen)
{
    if (slot < 0 || slot >= WEB_SESS_MAX) {
        return false;
    }
    lock();
    bool ok = s_slot[slot].used && s_slot[slot].gen == gen;
    unlock();
    return ok;
}
