/* Pairing overlay entry points and the rest of the web server's screen callbacks. The overlay itself (code, seconds
 * left, Cancel) is drawn here later; for now show wakes the screen and both ends log. */

#include "esp_log.h"
#include "hy/web.h"
#include "ui_internal.h"

static const char *TAG = "pair";

/* hy_web_ui_t.pair_show: LVGL task. The code is never logged. */
static void pair_show(const char *code, int64_t expires_mono)
{
    (void)code;
    (void)expires_mono;
    screen_wake("pair");
    ESP_LOGI(TAG, "pairing overlay shown");
}

/* hy_web_ui_t.pair_hide: LVGL task; reason is a hy_web_pair_end_t */
static void pair_hide(int reason)
{
    ESP_LOGI(TAG, "pairing overlay hidden (reason %d)", reason);
}

void pair_register(void)
{
    static const hy_web_ui_t web_ui = {.pair_show = pair_show,
                                       .pair_hide = pair_hide,
                                       .toast_ok = toast_show_ok,
                                       .toast_info = toast_show_info};
    hy_web_set_ui(&web_ui);
}
