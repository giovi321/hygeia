#include "esp_app_desc.h"
#include "esp_log.h"
#include "hy/board.h"

#include "console.h"

static const char *TAG = "hygeia";

static void cmd_boardtest(const char *args)
{
    (void)args;
    if (!hy_board_lock(1000)) {
        ESP_LOGE(TAG, "boardtest: lvgl lock timeout");
        return;
    }
    hy_board_selftest();
    hy_board_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "hygeia %s", esp_app_get_description()->version);

    esp_err_t err = hy_board_init();
    ESP_LOGI(TAG, "hy_board_init: %s", esp_err_to_name(err));
    if (err == ESP_OK && hy_board_lock(1000)) {
        lv_obj_t *l = lv_label_create(lv_screen_active());
        lv_label_set_text(l, "hygeia");
        lv_obj_center(l);
        hy_board_unlock();
    }

    console_register("boardtest", cmd_boardtest);
    console_start();
}
