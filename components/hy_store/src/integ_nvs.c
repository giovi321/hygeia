#include "hy/store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hy/integ.h"
#include "nvs.h"

#define NS "integ"

static const char *TAG = "hy_integ_cfg";
static HY_EXT_BSS hy_integ_t s_ram;
static bool s_ram_init;

static void ram_init(void)
{
    if (!s_ram_init) {
        hy_integ_defaults(&s_ram);
        s_ram_init = true;
    }
}

const hy_integ_t *hy_integ_cfg(void)
{
    ram_init();
    return &s_ram;
}

/* A missing key, a wrong type or a value that does not fit leaves dst as it is. */
static void get_str(nvs_handle_t h, const char *key, char *dst, size_t size)
{
    size_t n = size;
    char tmp[HY_INTEG_URL_MAX + 1];
    if (size > sizeof tmp) {
        return;
    }
    if (nvs_get_str(h, key, tmp, &n) == ESP_OK && n <= size) {
        memcpy(dst, tmp, n);
    }
    volatile unsigned char *w = (volatile unsigned char *)tmp;   /* may hold a password or a secret */
    for (size_t i = 0; i < sizeof tmp; i++) w[i] = 0;
}

static void key_of(char *out, size_t n, unsigned i, const char *suffix)
{
    snprintf(out, n, "wh%u_%s", i, suffix);
}

static void read_all(nvs_handle_t h, hy_integ_t *c)
{
    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, "mq_on", &u8) == ESP_OK) c->mqtt.on = u8 != 0;
    get_str(h, "mq_url", c->mqtt.url, sizeof c->mqtt.url);
    get_str(h, "mq_user", c->mqtt.user, sizeof c->mqtt.user);
    get_str(h, "mq_pass", c->mqtt.pass, sizeof c->mqtt.pass);
    get_str(h, "mq_base", c->mqtt.base, sizeof c->mqtt.base);
    if (nvs_get_u8(h, "mq_ha", &u8) == ESP_OK) c->mqtt.ha = u8 != 0;
    get_str(h, "mq_pfx", c->mqtt.ha_prefix, sizeof c->mqtt.ha_prefix);
    if (nvs_get_u8(h, "wh_n", &u8) == ESP_OK) c->n_hooks = u8 > HY_HOOK_MAX ? HY_HOOK_MAX : u8;
    for (unsigned i = 0; i < c->n_hooks; i++) {
        char k[12];
        hy_hook_cfg_t *w = &c->hook[i];
        w->on = true;
        w->events = HY_IE_ALL;
        key_of(k, sizeof k, i, "on");
        if (nvs_get_u8(h, k, &u8) == ESP_OK) w->on = u8 != 0;
        key_of(k, sizeof k, i, "url");
        get_str(h, k, w->url, sizeof w->url);
        key_of(k, sizeof k, i, "ev");
        if (nvs_get_u16(h, k, &u16) == ESP_OK) w->events = u16;
        key_of(k, sizeof k, i, "sec");
        get_str(h, k, w->secret, sizeof w->secret);
    }
}

esp_err_t hy_integ_cfg_load(void)
{
    hy_integ_defaults(&s_ram);
    s_ram_init = true;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   /* first boot, or no integration ever saved: defaults */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s, using defaults", esp_err_to_name(err));
        return err;
    }
    read_all(h, &s_ram);
    nvs_close(h);
    hy_api_err_t ae;
    if (hy_integ_validate(&s_ram, &ae) != HY_OK) {
        ESP_LOGW(TAG, "stored settings invalid (%s), using defaults", ae.field);   /* the field name only, never a value */
        hy_integ_defaults(&s_ram);
    }
    return ESP_OK;
}

static esp_err_t save_all(nvs_handle_t h, const hy_integ_t *c)
{
    esp_err_t err = nvs_set_u8(h, "mq_on", c->mqtt.on);
    if (err == ESP_OK) err = nvs_set_str(h, "mq_url", c->mqtt.url);
    if (err == ESP_OK) err = nvs_set_str(h, "mq_user", c->mqtt.user);
    if (err == ESP_OK) err = nvs_set_str(h, "mq_pass", c->mqtt.pass);
    if (err == ESP_OK) err = nvs_set_str(h, "mq_base", c->mqtt.base);
    if (err == ESP_OK) err = nvs_set_u8(h, "mq_ha", c->mqtt.ha);
    if (err == ESP_OK) err = nvs_set_str(h, "mq_pfx", c->mqtt.ha_prefix);
    if (err == ESP_OK) err = nvs_set_u8(h, "wh_n", c->n_hooks);
    for (unsigned i = 0; err == ESP_OK && i < HY_HOOK_MAX; i++) {
        char k[12];
        if (i < c->n_hooks) {
            const hy_hook_cfg_t *w = &c->hook[i];
            key_of(k, sizeof k, i, "on");
            err = nvs_set_u8(h, k, w->on);
            if (err == ESP_OK) { key_of(k, sizeof k, i, "url"); err = nvs_set_str(h, k, w->url); }
            if (err == ESP_OK) { key_of(k, sizeof k, i, "ev"); err = nvs_set_u16(h, k, w->events); }
            if (err == ESP_OK) { key_of(k, sizeof k, i, "sec"); err = nvs_set_str(h, k, w->secret); }
        } else {
            static const char *const SUF[] = { "on", "url", "ev", "sec" };
            for (unsigned s = 0; err == ESP_OK && s < 4; s++) {
                key_of(k, sizeof k, i, SUF[s]);
                esp_err_t e = nvs_erase_key(h, k);
                if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) err = e;
            }
        }
    }
    if (err == ESP_OK) err = nvs_commit(h);
    return err;
}

esp_err_t hy_integ_cfg_save(const hy_integ_t *c)
{
    ram_init();
    if (c != &s_ram) {
        s_ram = *c;   /* the RAM copy follows even when the save fails */
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = save_all(h, &s_ram);
    nvs_close(h);
    return err;
}
