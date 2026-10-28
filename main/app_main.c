#include "esp_app_desc.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "nvs_flash.h"

#include "console.h"

static const char *TAG = "hygeia";

static void cmd_boardtest(const char *args)
{
    (void)args;
    if (!hy_board_display()) {
        ESP_LOGE(TAG, "boardtest: board not ready");
        return;
    }
    if (!hy_board_lock(1000)) {
        ESP_LOGE(TAG, "boardtest: lvgl lock timeout");
        return;
    }
    hy_board_selftest();
    hy_board_unlock();
}

static void cmd_powerstat(const char *args)
{
    (void)args;
    hy_board_power_report();
}

static void cmd_time(const char *args)
{
    char buf[96];
    strlcpy(buf, args, sizeof buf);
    size_t n = strlen(buf);
    while (n > 0 && isspace((unsigned char)buf[n - 1])) {
        buf[--n] = 0;
    }
    char *end;
    long long epoch = strtoll(buf, &end, 10);
    if (end == buf || (*end != 0 && *end != ' ')) {
        ESP_LOGE(TAG, "usage: time <epoch> [posix_tz]");
        return;
    }
    while (*end == ' ') {
        end++;
    }
    if (*end && strchr(end, ' ')) {
        ESP_LOGE(TAG, "time: posix_tz must not contain spaces");
        return;
    }
    esp_err_t err = hy_clock_set(epoch, *end ? end : NULL, "serial");
    ESP_LOGI(TAG, "time: %s", esp_err_to_name(err));
}

static void cmd_clock(const char *args)
{
    (void)args;
    hy_clock_report();
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs: %s, erasing the nvs partition", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase()); /* the nvs partition only, never storage */
        err = nvs_flash_init();
    }
    ESP_LOGI(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
}

void app_main(void)
{
    ESP_LOGI(TAG, "hygeia %s", esp_app_get_description()->version);

    esp_err_t err = hy_board_init();
    ESP_LOGI(TAG, "hy_board_init: %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        if (hy_board_lock(1000)) {
            lv_obj_t *l = lv_label_create(lv_screen_active());
            lv_label_set_text(l, "hygeia");
            lv_obj_center(l);
            hy_board_unlock();
        } else {
            ESP_LOGE(TAG, "boot label: lvgl lock timeout");
        }
    }

    init_nvs();
    if (err == ESP_OK) {
        esp_err_t cerr = hy_clock_init(hy_board_i2c());
        ESP_LOGI(TAG, "hy_clock_init: %s, clock valid: %s", esp_err_to_name(cerr), hy_clock_valid() ? "yes" : "no");
    }

    console_register("boardtest", cmd_boardtest);
    console_register("powerstat", cmd_powerstat);
    console_register("time", cmd_time);
    console_register("clock", cmd_clock);
    console_start();
}
