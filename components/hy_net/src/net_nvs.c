#include "net_priv.h"

#include <stdio.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#define NS "wifi"

static const char *TAG = "net";

bool net_host_valid(const char *host)
{
    size_t n = host ? strlen(host) : 0;
    if (n < 1 || n > 32 || host[0] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        out[0] = 0;
    }
}

/* First boot: the RF subsystem is off, so the SAR ADC entropy source is switched on around the read. */
static void create_ap_pass(char out[64])
{
    uint8_t rnd[10];
    bootloader_random_enable();
    esp_fill_random(rnd, sizeof rnd);
    bootloader_random_disable();
    char p[11];
    hy_net_ap_pass(rnd, p);
    memset(rnd, 0, sizeof rnd);
    strlcpy(out, p, 64);
    memset(p, 0, sizeof p);
}

esp_err_t net_nvs_load(net_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->mode = HY_NET_OFF;
    strlcpy(c->host, NET_HOST_DEFAULT, sizeof c->host);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs open: %s, defaults in use", esp_err_to_name(err));
        create_ap_pass(c->ap_pass);
        return err;
    }
    uint8_t u8;
    if (nvs_get_u8(h, "mode", &u8) == ESP_OK && u8 <= HY_NET_AP) {
        c->mode = (hy_net_mode_t)u8;
    }
    uint8_t n = 0;
    if (nvs_get_u8(h, "n", &u8) == ESP_OK) {
        n = u8 > HY_NET_MAX_KNOWN ? HY_NET_MAX_KNOWN : u8;
    }
    uint32_t seq = 0;
    nvs_get_u32(h, "okseq", &seq);
    bool dropped = false;
    for (uint8_t i = 0; i < n; i++) {
        char key[8];
        net_entry_t *e = &c->e[c->n];
        snprintf(key, sizeof key, "s%u", (unsigned)i);
        get_str(h, key, e->ssid, sizeof e->ssid);
        snprintf(key, sizeof key, "p%u", (unsigned)i);
        get_str(h, key, e->pass, sizeof e->pass);
        snprintf(key, sizeof key, "ok%u", (unsigned)i);
        if (nvs_get_u32(h, key, &e->ok) != ESP_OK) {
            e->ok = 0;
        }
        if (hy_net_check_ssid(e->ssid) != HY_OK || hy_net_check_pass(e->pass, false) != HY_OK) {
            memset(e, 0, sizeof *e);
            dropped = true;
            continue;
        }
        if (e->ok > seq) {
            seq = e->ok;
        }
        c->n++;
    }
    c->okseq = seq;

    get_str(h, "host", c->host, sizeof c->host);
    if (!net_host_valid(c->host)) {
        strlcpy(c->host, NET_HOST_DEFAULT, sizeof c->host);
    }

    get_str(h, "ap_pass", c->ap_pass, sizeof c->ap_pass);
    if (hy_net_check_pass(c->ap_pass, true) != HY_OK) {
        create_ap_pass(c->ap_pass);
        esp_err_t werr = nvs_set_str(h, "ap_pass", c->ap_pass);
        if (werr == ESP_OK) {
            werr = nvs_commit(h);
        }
        ESP_LOGI(TAG, "access point password created (save: %s)", esp_err_to_name(werr));
    }
    nvs_close(h);
    if (dropped) {
        ESP_LOGW(TAG, "invalid saved network entries dropped");
        net_nvs_save_list(c);
    }
    return ESP_OK;
}

static esp_err_t open_rw(nvs_handle_t *h)
{
    esp_err_t err = nvs_open(NS, NVS_READWRITE, h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs open: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t finish(nvs_handle_t h, esp_err_t err, const char *what)
{
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs save %s: %s", what, esp_err_to_name(err));
    }
    return err;
}

esp_err_t net_nvs_save_mode(hy_net_mode_t m)
{
    nvs_handle_t h;
    esp_err_t err = open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }
    return finish(h, nvs_set_u8(h, "mode", (uint8_t)m), "mode");
}

esp_err_t net_nvs_save_list(const net_cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t err = open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "n", c->n);
    for (uint8_t i = 0; i < HY_NET_MAX_KNOWN && err == ESP_OK; i++) {
        char ks[8], kp[8], ko[8];
        snprintf(ks, sizeof ks, "s%u", (unsigned)i);
        snprintf(kp, sizeof kp, "p%u", (unsigned)i);
        snprintf(ko, sizeof ko, "ok%u", (unsigned)i);
        if (i < c->n) {
            err = nvs_set_str(h, ks, c->e[i].ssid);
            if (err == ESP_OK) err = nvs_set_str(h, kp, c->e[i].pass);
            if (err == ESP_OK) err = nvs_set_u32(h, ko, c->e[i].ok);
        } else {
            const char *keys[3] = {ks, kp, ko};
            for (int k = 0; k < 3 && err == ESP_OK; k++) {
                esp_err_t e = nvs_erase_key(h, keys[k]);
                if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
                    err = e;
                }
            }
        }
    }
    return finish(h, err, "networks");
}

esp_err_t net_nvs_save_ok(uint8_t idx, uint32_t ok, uint32_t okseq)
{
    nvs_handle_t h;
    esp_err_t err = open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }
    char ko[8];
    snprintf(ko, sizeof ko, "ok%u", (unsigned)idx);
    err = nvs_set_u32(h, ko, ok);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "okseq", okseq);
    }
    return finish(h, err, "okseq");
}

esp_err_t net_nvs_save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }
    return finish(h, nvs_set_str(h, key, val), key);
}
