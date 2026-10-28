#include "esp_app_desc.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "hy/board.h"
#include "hy/actions.h"
#include "hy/clock.h"
#include "hy/store.h"
#include "hy/ui.h"
#include "nvs_flash.h"
#include <sys/stat.h>

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
    /* The test screen cleans the active screen, which would delete the home grid: load a fresh one
     * first. The home screen stays alive but hidden; a reboot returns to it. */
    static lv_obj_t *test_screen;   /* previous test screen, deleted so repeated runs do not leak */
    lv_obj_t *old = test_screen;
    test_screen = lv_obj_create(NULL);
    lv_screen_load(test_screen);
    if (old) {
        lv_obj_delete(old);
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

static void cmd_tap(const char *args)
{
    char code[4];
    strlcpy(code, args, sizeof code);
    if (!code[0]) {
        ESP_LOGE(TAG, "usage: tap <code>");
        return;
    }
    hy_ui_debug_tap(code);
}

static void cmd_undo(const char *args) { (void)args; hy_ui_debug_undo(); }
static void cmd_logs(const char *args) { (void)args; hy_ui_debug_logs(); }
static void cmd_seed4(const char *args) { hy_ui_debug_seed4(args); }
static void cmd_shot(const char *args) { (void)args; hy_ui_debug_shot(); }

#define MEDS_FILE "/data/meds.json"

/* First boot: the three example medicines, added through the actions so validation applies. */
static void seed_example_meds(void)
{
    static const struct {
        const char *code, *name;
        uint8_t color;
        uint16_t times[3];
        uint8_t n_times;
        uint16_t min_interval;
    } seed[] = {
        {"A", "Antibiotic", 0, {6 * 60, 14 * 60, 22 * 60}, 3, 0},
        {"V", "Vitamin D", 1, {8 * 60 + 30}, 1, 0},
        {"M", "Magnesium", 2, {0}, 0, 240},
    };
    for (size_t i = 0; i < sizeof seed / sizeof seed[0]; i++) {
        hy_med_t m;
        memset(&m, 0, sizeof m);
        hy_med_defaults(&m);
        strcpy(m.code, seed[i].code);
        strcpy(m.name, seed[i].name);
        m.color = seed[i].color;
        m.order = (uint16_t)i;
        if (seed[i].n_times) {
            m.type = HY_SCHED_FIXED;
            memcpy(m.times, seed[i].times, sizeof m.times);
            m.n_times = seed[i].n_times;
        } else {
            m.type = HY_SCHED_AS_NEEDED;
            m.min_interval_min = seed[i].min_interval;
        }
        hy_err_t e = hy_act_med_upsert(&m);
        ESP_LOGI(TAG, "seed %s: %s", seed[i].code, hy_err_str(e));
    }
}

static void init_storage(void)
{
    esp_err_t err = hy_store_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage unavailable, no persistence: %s", esp_err_to_name(err));
        return;
    }
    hy_err_t oerr = hy_store_open("/data");
    ESP_LOGI(TAG, "hy_store_open: %s", hy_err_str(oerr));
    if (oerr != HY_OK) {
        return;
    }
    struct stat st;
    bool first_boot = stat(MEDS_FILE, &st) != 0;
    hy_err_t e = hy_actions_init(hy_store_port(), NULL, hy_clock_port(), NULL);
    ESP_LOGI(TAG, "hy_actions_init: %s", hy_err_str(e));
    if (first_boot) {
        ESP_LOGI(TAG, "%s not found: seeding the example medicines", MEDS_FILE);
        seed_example_meds();
    }
    const hy_med_t *ms;
    ESP_LOGI(TAG, "medicines loaded: %u", (unsigned)hy_meds(&ms));
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

    init_nvs();
    if (err == ESP_OK) {
        esp_err_t cerr = hy_clock_init(hy_board_i2c());
        ESP_LOGI(TAG, "hy_clock_init: %s, clock valid: %s", esp_err_to_name(cerr), hy_clock_valid() ? "yes" : "no");
    }

    init_storage();
    if (err == ESP_OK) {
        esp_err_t uerr = hy_ui_start();
        ESP_LOGI(TAG, "hy_ui_start: %s", esp_err_to_name(uerr));
    }

    console_register("boardtest", cmd_boardtest);
    console_register("powerstat", cmd_powerstat);
    console_register("time", cmd_time);
    console_register("clock", cmd_clock);
    console_register("tap", cmd_tap);
    console_register("undo", cmd_undo);
    console_register("logs", cmd_logs);
    console_register("seed4", cmd_seed4);
    console_register("shot", cmd_shot);
    console_start();
}
