#include "hy/store.h"

#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "store_mount";

esp_err_t hy_store_mount(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/data",
        .partition_label = "storage",
        .format_if_mount_failed = false,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err == ESP_FAIL) {
        /* only a failed mount (not a missing partition) formats the partition */
        ESP_LOGW(TAG, "!!! littlefs mount failed, FORMATTING partition \"storage\" now: any stored data is erased !!!");
        conf.format_if_mount_failed = true;
        err = esp_vfs_littlefs_register(&conf);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount /data (partition \"storage\"): %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    err = esp_littlefs_info("storage", &total, &used);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_littlefs_info: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "mounted /data: total %u bytes, used %u bytes", (unsigned)total, (unsigned)used);
    return ESP_OK;
}
