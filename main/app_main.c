#include "esp_app_desc.h"
#include "esp_log.h"

static const char *TAG = "hygeia";

void app_main(void)
{
    ESP_LOGI(TAG, "hygeia %s", esp_app_get_description()->version);
}
