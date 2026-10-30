#include "hy/store.h"

#include "esp_log.h"
#include "nvs.h"

#define NS "settings"

static const char *TAG = "hy_settings";
static hy_settings_t s_ram;
static bool s_ram_init;

static void ram_init(void)
{
    if (!s_ram_init) {
        hy_settings_defaults(&s_ram);
        s_ram_init = true;
    }
}

const hy_settings_t *hy_settings(void)
{
    ram_init();
    return &s_ram;
}

esp_err_t hy_settings_load(hy_settings_t *s)
{
    hy_settings_defaults(s);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        if (nvs_get_u8(h, "sound", &u8) == ESP_OK) s->sound = u8 != 0;
        if (nvs_get_u8(h, "dnd", &u8) == ESP_OK) s->dnd = u8 != 0;
        if (nvs_get_u8(h, "vol", &u8) == ESP_OK) s->volume = u8;
        if (nvs_get_u8(h, "bright", &u8) == ESP_OK) s->brightness = u8;
        if (nvs_get_u16(h, "snooze", &u16) == ESP_OK) s->snooze_min = u16;
        if (nvs_get_u16(h, "dim_s", &u16) == ESP_OK) s->dim_s = u16;
        if (nvs_get_u16(h, "off_bat_s", &u16) == ESP_OK) s->off_bat_s = u16;
        if (nvs_get_u32(h, "off_usb_s", &u32) == ESP_OK) s->off_usb_s = u32;
        if (nvs_get_u8(h, "rot_lock", &u8) == ESP_OK) s->rot_lock = u8 != 0;
        if (nvs_get_u8(h, "rot", &u8) == ESP_OK) s->rot = u8;
        nvs_close(h);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;   /* first boot: defaults */
    } else {
        ESP_LOGW(TAG, "nvs_open: %s, using defaults", esp_err_to_name(err));
    }
    hy_settings_clamp(s);
    s_ram = *s;
    s_ram_init = true;
    return err;
}

esp_err_t hy_settings_save(const hy_settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "sound", s->sound);
    if (err == ESP_OK) err = nvs_set_u8(h, "dnd", s->dnd);
    if (err == ESP_OK) err = nvs_set_u8(h, "vol", s->volume);
    if (err == ESP_OK) err = nvs_set_u8(h, "bright", s->brightness);
    if (err == ESP_OK) err = nvs_set_u16(h, "snooze", s->snooze_min);
    if (err == ESP_OK) err = nvs_set_u16(h, "dim_s", s->dim_s);
    if (err == ESP_OK) err = nvs_set_u16(h, "off_bat_s", s->off_bat_s);
    if (err == ESP_OK) err = nvs_set_u32(h, "off_usb_s", s->off_usb_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "rot_lock", s->rot_lock);
    if (err == ESP_OK) err = nvs_set_u8(h, "rot", s->rot);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t hy_settings_update(const hy_settings_t *s)
{
    hy_settings_t c = *s;
    hy_settings_clamp(&c);
    s_ram = c;
    s_ram_init = true;
    return hy_settings_save(&c);
}
