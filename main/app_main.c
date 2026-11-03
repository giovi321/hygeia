#include "esp_app_desc.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "hy/audio.h"
#include "hy/board.h"
#include "hy/actions.h"
#include "hy/clock.h"
#include "hy/motion.h"
#include "hy/motion_run.h"
#include "hy/net.h"
#include "hy/power_run.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "hy/ui.h"
#include "hy/update.h"
#include "hy/web.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <sys/stat.h>

#if CONFIG_HY_DEBUG_CONSOLE
#include "console.h"
#endif

static const char *TAG = "hygeia";

#if CONFIG_HY_DEBUG_CONSOLE
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

static void cmd_alarm(const char *args)
{
    char *end;
    long long secs = strtoll(args, &end, 10);
    while (*end == ' ') {
        end++;
    }
    if (end == args || *end != 0 || secs < 0 || secs > 31 * 86400) {
        ESP_LOGE(TAG, "usage: alarm <seconds, at most 31 days> (0 disables)");
        return;
    }
    int64_t at = secs ? (int64_t)time(NULL) + secs : 0;
    esp_err_t err = hy_clock_alarm_set(at);
    ESP_LOGI(TAG, "alarm: %s, utc %lld", esp_err_to_name(err), (long long)at);
}

static void cmd_zone(const char *args)
{
    /* Wider than HY_ZONE_LEN / HY_TZ_LEN so hy_clock_set_zone rejects a too long value
     * instead of sscanf truncating it */
    char iana[64], posix[64], extra[2];
    if (sscanf(args, "%63s %63s %1s", iana, posix, extra) != 2) {
        ESP_LOGE(TAG, "usage: zone <iana> <posix_tz>");
        return;
    }
    esp_err_t err = hy_clock_set_zone(iana, posix);
    ESP_LOGI(TAG, "zone: %s", esp_err_to_name(err));
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
static void cmd_gaps(const char *args) { (void)args; hy_ui_debug_gaps(); }
static void cmd_seed4(const char *args) { hy_ui_debug_seed4(args); }
static void cmd_shot(const char *args) { (void)args; hy_ui_debug_shot(); }
static void cmd_chime(const char *args) { (void)args; hy_audio_chime(); }
static void cmd_remindtest(const char *args) { hy_ui_debug_remindtest(args); }
static void cmd_perf(const char *args) { (void)args; hy_ui_debug_perf(); }
static void cmd_gesture(const char *args) { hy_ui_debug_gesture(args); }
static void cmd_nav(const char *args) { (void)args; hy_ui_debug_nav(); }
static void cmd_storefail(const char *args) { hy_ui_debug_storefail(args); }
/* "poweroff": the raw AXP2101 cut with a clean heartbeat; "poweroff show|cancel|slide|crit": the overlays */
static void cmd_poweroff(const char *args)
{
    if (args[0]) {
        hy_ui_debug_poweroff(args);
    } else {
        hy_power_debug_poweroff(args);
    }
}
static void cmd_idle(const char *args) { hy_ui_debug_idle(args); }
static void cmd_scr(const char *args) { hy_ui_debug_scr(args); }
static void cmd_mf(const char *args) { hy_ui_debug_mf(args); }
static void cmd_archive(const char *args) { hy_ui_debug_archive(args); }
static void cmd_limits(const char *args) { (void)args; hy_ui_debug_limits(); }
static void cmd_madctl(const char *args) { hy_ui_debug_madctl(args); }
static void cmd_rot(const char *args) { hy_ui_debug_rot(args); }
static void cmd_shake(const char *args) { (void)args; hy_ui_debug_shake(); }
static void cmd_sleepstat(const char *args) { (void)args; hy_power_sleep_stats(); }
static void cmd_imu(const char *args)
{
    /* The motion task owns the IMU: it prints the samples from its own reads. `imu regs` is a direct register read;
       wom, cap and pin hold the motion task off the IMU while they run. */
    if (strncmp(args, "wom", 3) == 0 || strncmp(args, "cap", 3) == 0 || strncmp(args, "pin", 3) == 0) {
        hy_ui_debug_imu(args);
        return;
    }
    bool raw = strncmp(args, "raw", 3) == 0;
    int secs = 0;
    bool rec = sscanf(args, "rec %d", &secs) == 1;
    if (strncmp(args, "regs", 4) == 0) {
        uint8_t r[HY_IMU_REGS_N];
        esp_err_t rerr = hy_board_imu_regs(r);
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "imu regs: %s", esp_err_to_name(rerr));
            return;
        }
        printf("imu regs: CTRL1=0x%02x CTRL2=0x%02x CTRL3=0x%02x CTRL5=0x%02x CTRL7=0x%02x CTRL8=0x%02x CTRL9=0x%02x "
               "STATUSINT=0x%02x\n", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
        return;
    }
    if (strncmp(args, "rec", 3) == 0 && (!rec || secs < 1 || secs > 120)) {
        ESP_LOGE(TAG, "usage: imu rec <1..120 s>");
        return;
    }
    hy_motion_imu(raw, rec, rec ? secs * 50 : 10);
}
static void cmd_motion(const char *args) { hy_motion_debug(args); }
static void cmd_tapstroke(const char *args) { hy_ui_debug_tapstroke(args); }
static void cmd_due(const char *args) { hy_ui_debug_due(args); }
static void cmd_history(const char *args) { hy_ui_debug_history(args); }
static void cmd_detail(const char *args) { hy_ui_debug_detail(args); }
static void cmd_note(const char *args) { hy_ui_debug_note(args); }
static void cmd_earlier(const char *args) { hy_ui_debug_earlier(args); }
static void cmd_qs(const char *args) { hy_ui_debug_qs(args); }
static void cmd_settz(const char *args)
{
    char iana[64];
    if (sscanf(args, "%63s", iana) != 1) {
        ESP_LOGE(TAG, "usage: settz <iana>");
        return;
    }
    hy_ui_debug_settz(iana);
}
static void cmd_settings(const char *args) { (void)args; hy_ui_debug_settings(); }
static void cmd_wifi(const char *args) { hy_net_debug(args); }
static void cmd_web(const char *args) { hy_web_debug(args); }
static void cmd_webjob(const char *args) { hy_web_debug_job(args); }
static void cmd_upd(const char *args) { hy_update_debug(args); }
/* "pair": the running pairing code (console only); "pair show|hide": the overlay with an example code */
static void cmd_pair(const char *args)
{
    if (args[0]) {
        hy_ui_debug_pair(args);
        return;
    }
    hy_web_debug_pair();
}
/* "ui net <state>": the screens show that network state (see hy_ui_debug_net) */
static void cmd_ui(const char *args)
{
    if (strncmp(args, "net ", 4) == 0) {
        hy_ui_debug_net(args + 4);
        return;
    }
    if (strncmp(args, "upd ", 4) == 0) {
        hy_ui_debug_upd(args + 4);
        return;
    }
    if (strncmp(args, "hit ", 4) == 0) {
        hy_ui_debug_hit(args + 4);
        return;
    }
    printf("usage: ui net off|paused|connecting|connected|weak|failing|ap|auto | ui upd checking|latest|available|dev|confirm|downloading [pct]|verifying|restarting|failed [code]|auto | ui hit <x> <y>\n");
}
#endif

/* ---- firmware confirmation after an update ---- */

#define OTA_NVS_NS "ota"
#define OTA_NVS_FAIL "fail"          /* 1: abort before confirming, 2: hang before confirming */
#define CONFIRM_WAIT_MS 120000
#define CONFIRM_DEADLINE_MS 150000   /* a new firmware that has not confirmed itself by then is restarted */

static esp_timer_handle_t s_confirm_deadline;

/* The idle tasks the task watchdog watches (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPUn) */
static uint32_t twdt_idle_mask(void)
{
    uint32_t m = 0;
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    m |= 1u << 0;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    m |= 1u << 1;
#endif
    return m;
}

static void twdt_panic(bool on)
{
#if CONFIG_ESP_TASK_WDT_INIT
    const esp_task_wdt_config_t c = {.timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
                                     .idle_core_mask = twdt_idle_mask(), .trigger_panic = on};
    esp_err_t e = esp_task_wdt_reconfigure(&c);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog reconfigure: %s", esp_err_to_name(e));
    }
#else
    (void)on;
#endif
}

static const char *ota_state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

#if CONFIG_HY_DEBUG_CONSOLE
/* The otafail flag, read and cleared once: 0 none, 1 abort, 2 hang. */
static uint8_t ota_fail_take(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return 0;
    }
    if (nvs_get_u8(h, OTA_NVS_FAIL, &v) == ESP_OK) {
        nvs_erase_key(h, OTA_NVS_FAIL);
        nvs_commit(h);
    }
    nvs_close(h);
    return v;
}

/* "otafail": the next boot of an uploaded firmware stops (abort) just before it would confirm itself, so the
 * bootloader goes back to the previous firmware. "otafail hang": it blocks there instead, so the confirmation
 * deadline restarts it. "otafail off" clears the flag. All print the slots. */
static void cmd_otafail(const char *args)
{
    bool off = strcmp(args, "off") == 0;
    bool hang = strcmp(args, "hang") == 0;
    if (args[0] && !off && !hang) {
        printf("usage: otafail [hang|off]\n");
        return;
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open(OTA_NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = off ? nvs_erase_key(h, OTA_NVS_FAIL) : nvs_set_u8(h, OTA_NVS_FAIL, hang ? 2 : 1);
        if (e == ESP_ERR_NVS_NOT_FOUND) {
            e = ESP_OK;
        }
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    printf("otafail %s: %s\n",
           off ? "cleared" : hang ? "set, the next uploaded firmware will hang before it confirms itself"
                                  : "set, the next uploaded firmware will abort before it confirms itself",
           esp_err_to_name(e));
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run) {
        esp_ota_get_state_partition(run, &st);
    }
    printf("running: %s, %s, firmware %s\n", run ? run->label : "-", ota_state_name(st),
           esp_app_get_description()->version);
    esp_app_desc_t d;
    if (next && esp_ota_get_partition_description(next, &d) == ESP_OK) {
        st = ESP_OTA_IMG_UNDEFINED;
        esp_ota_get_state_partition(next, &st);
        printf("other slot: %s, %s, firmware %.32s\n", next->label, ota_state_name(st), d.version);
    } else {
        printf("other slot: %s, no firmware\n", next ? next->label : "-");
    }
}
#endif

static bool running_pending(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

static void deadline_cb(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "firmware did not confirm itself in time, restarting into the previous firmware");
    esp_restart();
}

/* First thing in app_main: a new firmware gets CONFIRM_DEADLINE_MS to confirm itself, and the task watchdog panics
 * (reset) instead of warning until then. A confirmed firmware never arms either, so there is no restart loop. */
static void arm_confirm_deadline(void)
{
    if (!running_pending()) {
        return;
    }
    const esp_timer_create_args_t a = {.callback = deadline_cb, .name = "ota_deadline"};
    if (esp_timer_create(&a, &s_confirm_deadline) != ESP_OK ||
        esp_timer_start_once(s_confirm_deadline, CONFIRM_DEADLINE_MS * 1000ULL) != ESP_OK) {
        ESP_LOGE(TAG, "confirmation deadline not armed");
    }
    twdt_panic(true);
}

static void disarm_confirm_deadline(void)
{
    if (s_confirm_deadline) {
        esp_timer_stop(s_confirm_deadline);
        esp_timer_delete(s_confirm_deadline);
        s_confirm_deadline = NULL;
    }
    twdt_panic(false);
}

/* A firmware written by the web update boots pending verification. It confirms itself only after the store opened,
 * the screen started and the scheduler made its first pass (an evaluation, or the "clock invalid" pass when the
 * clock is not set, so a device without the time still confirms). Until then any reset (crash, watchdog, the
 * confirmation deadline, power) makes the bootloader start the previous firmware; a firmware that cannot confirm
 * restarts at once. */
static void confirm_firmware(bool store_ok, bool ui_ok)
{
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (bad && esp_ota_get_partition_description(bad, &d) == ESP_OK) {
        ESP_LOGW(TAG, "firmware %.32s in %s did not confirm itself and was rolled back", d.version, bad->label);
    }
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "running %s, %s", run ? run->label : "-", ota_state_name(st));
        return;
    }
    ESP_LOGI(TAG, "firmware %s in %s is new: confirming after the first scheduler pass",
             esp_app_get_description()->version, run->label);
    if (!store_ok || !ui_ok) {
        ESP_LOGE(TAG, "firmware not confirmed (store %s, screen %s): restarting into the previous firmware",
                 store_ok ? "ok" : "failed", ui_ok ? "ok" : "failed");
        esp_restart();
    }
    int64_t t0 = esp_timer_get_time();
    while (hy_sched_generation() == 0) {
        if (esp_timer_get_time() - t0 > CONFIRM_WAIT_MS * 1000LL) {
            ESP_LOGE(TAG, "firmware not confirmed: no scheduler pass in %d s; restarting into the previous firmware",
                     CONFIRM_WAIT_MS / 1000);
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#if CONFIG_HY_DEBUG_CONSOLE
    uint8_t fail = ota_fail_take();
    if (fail == 2) {
        ESP_LOGW(TAG, "otafail hang: blocking before the firmware confirms itself");
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
    if (fail) {
        ESP_LOGW(TAG, "otafail: stopping before the firmware confirms itself");
        abort();
    }
#endif
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "firmware confirmed after %lld ms: %s", (long long)((esp_timer_get_time() - t0) / 1000),
             esp_err_to_name(e));
    if (e == ESP_OK) {
        disarm_confirm_deadline();
    }
}

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
        m.active_from = hy_clock_today_ymd();
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

/* JSON trees (store saves, API bodies) live in PSRAM; internal RAM stays free for Wi-Fi and DMA. */
static void *json_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void json_free(void *p)
{
    heap_caps_free(p);
}

/* true when the store opened */
static bool init_storage(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = json_free};
    cJSON_InitHooks(&hooks);
    esp_err_t err = hy_store_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage unavailable, no persistence: %s", esp_err_to_name(err));
        hy_ui_set_storage_ok(false);
        return false;
    }
    hy_err_t oerr = hy_store_open("/data");
    ESP_LOGI(TAG, "hy_store_open: %s", hy_err_str(oerr));
    if (oerr != HY_OK) {
        hy_ui_set_storage_ok(false);
        return false;
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
    return true;
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

/* The orientation before the splash's first frame: the saved one when locked, else one IMU sample, else the saved one */
static void boot_rotation(const hy_settings_t *settings)
{
    hy_rot_t rot = (hy_rot_t)(settings->rot & 3);
    const char *why = "saved";
    char detail[64];
    if (settings->rot_lock) {
        why = "locked";
    } else {
        int16_t mg[3];
        hy_rot_t cand;
        esp_err_t serr = hy_board_imu_sample_once(mg);
        if (serr == ESP_OK && hy_orient_candidate((hy_accel_t){mg[0], mg[1], mg[2]}, &cand)) {
            rot = cand;
            why = "imu";
        } else if (serr == ESP_OK) {
            snprintf(detail, sizeof detail, "saved; imu %d %d %d mg, no candidate", mg[0], mg[1], mg[2]);
            why = detail;
        } else {
            snprintf(detail, sizeof detail, "saved; imu error %s", esp_err_to_name(serr));
            why = detail;
        }
    }
    if (hy_board_lock(1000)) {
        hy_board_set_rotation((uint8_t)rot);
        hy_board_unlock();
    } else {
        ESP_LOGE(TAG, "boot rotation: lvgl lock timeout");
    }
    ESP_LOGI(TAG, "boot rotation: %u (%s)", (unsigned)rot * 90, why);
}

void app_main(void)
{
    ESP_LOGI(TAG, "hygeia %s", esp_app_get_description()->version);
    arm_confirm_deadline();

    esp_err_t err = hy_board_init();
    ESP_LOGI(TAG, "hy_board_init: %s", esp_err_to_name(err));

    int64_t t_pre = esp_timer_get_time();
    init_nvs();
    hy_settings_t settings;
    esp_err_t serr = hy_settings_load(&settings);
    ESP_LOGI(TAG, "settings loaded: %s (sound %d, dnd %d, volume %u, brightness %u, snooze %u min, rot %u, rot lock %d)",
             esp_err_to_name(serr), settings.sound, settings.dnd, (unsigned)settings.volume,
             (unsigned)settings.brightness, (unsigned)settings.snooze_min, (unsigned)settings.rot,
             settings.rot_lock);
    if (err == ESP_OK) {
        boot_rotation(&settings);
    }
    ESP_LOGI(TAG, "boot: nvs, settings and rotation took %lld ms before the splash",
             (long long)((esp_timer_get_time() - t_pre) / 1000));
    if (err == ESP_OK) {
        esp_err_t perr = hy_ui_splash_start();
        ESP_LOGI(TAG, "hy_ui_splash_start: %s", esp_err_to_name(perr));
    }
    if (err == ESP_OK) {
        hy_board_set_brightness(settings.brightness);
        hy_audio_set_volume(settings.volume);   /* hy_audio_init runs after the splash */
    }
    if (err == ESP_OK) {
        esp_err_t cerr = hy_clock_init(hy_board_i2c());
        ESP_LOGI(TAG, "hy_clock_init: %s, clock valid: %s", esp_err_to_name(cerr), hy_clock_valid() ? "yes" : "no");
    }

    bool store_ok = init_storage();
    esp_err_t nerr = hy_net_init();   /* NVS is up: networks, access point password; the radio stays off */
    ESP_LOGI(TAG, "hy_net_init: %s", esp_err_to_name(nerr));
    hy_update_init();   /* the installed version; no connection until a user's check */
    hy_update_set_restart(hy_web_restart);   /* the restart the web upload uses: through the LVGL task */
    hy_net_set_keep_batt(settings.wifi_keep_batt);
    if (err == ESP_OK) {
        esp_err_t perr = hy_power_init();
        ESP_LOGI(TAG, "hy_power_init: %s", esp_err_to_name(perr));
    }
    hy_power_set_next_wake_fn(hy_sched_run_next_wake);   /* the power task wakes for it while asleep (R11) */
    esp_err_t rerr = hy_sched_run_init();
    if (rerr == ESP_OK) {
        ESP_LOGI(TAG, "hy_sched_run_init: %s", esp_err_to_name(rerr));
    } else {
        ESP_LOGE(TAG, "hy_sched_run_init: %s: reminders are OFF", esp_err_to_name(rerr));
    }
    bool ui_ok = false;
    if (err == ESP_OK) {
        esp_err_t uerr = hy_ui_start();
        ESP_LOGI(TAG, "hy_ui_start: %s", esp_err_to_name(uerr));
        ui_ok = uerr == ESP_OK;
        hy_power_start();
        esp_err_t aerr = hy_audio_init();   /* about 0.4 s: not on the splash's path */
        ESP_LOGI(TAG, "hy_audio_init: %s", esp_err_to_name(aerr));
    }
    hy_net_start();   /* after hy_power_start: the radio rule reads USB from the power state */
    esp_err_t werr = hy_web_start();   /* the server follows the network state */
    ESP_LOGI(TAG, "hy_web_start: %s", esp_err_to_name(werr));

#if CONFIG_HY_DEBUG_CONSOLE
    console_register("boardtest", cmd_boardtest);
    console_register("powerstat", cmd_powerstat);
    console_register("time", cmd_time);
    console_register("zone", cmd_zone);
    console_register("alarm", cmd_alarm);
    console_register("clock", cmd_clock);
    console_register("tap", cmd_tap);
    console_register("undo", cmd_undo);
    console_register("logs", cmd_logs);
    console_register("seed4", cmd_seed4);
    console_register("shot", cmd_shot);
    console_register("chime", cmd_chime);
    console_register("remindtest", cmd_remindtest);
    console_register("perf", cmd_perf);
    console_register("gesture", cmd_gesture);
    console_register("nav", cmd_nav);
    console_register("storefail", cmd_storefail);
    console_register("idle", cmd_idle);
    console_register("tapstroke", cmd_tapstroke);
    console_register("due", cmd_due);
    console_register("history", cmd_history);
    console_register("detail", cmd_detail);
    console_register("note", cmd_note);
    console_register("earlier", cmd_earlier);
    console_register("qs", cmd_qs);
    console_register("settz", cmd_settz);
    console_register("settings", cmd_settings);
    console_register("pmu", hy_power_debug_pmu);
    console_register("batt", hy_power_debug_batt);
    console_register("batlog", hy_power_debug_batlog);
    console_register("alive", hy_power_debug_alive);
    console_register("pkey", hy_power_debug_pkey);
    console_register("fakebatt", hy_power_debug_fakebatt);
    console_register("poweroff", cmd_poweroff);
    console_register("gaps", cmd_gaps);
    console_register("scr", cmd_scr);
    console_register("mf", cmd_mf);
    console_register("archive", cmd_archive);
    console_register("limits", cmd_limits);
    console_register("sleepstat", cmd_sleepstat);
    console_register("imu", cmd_imu);
    console_register("motion", cmd_motion);
    console_register("madctl", cmd_madctl);
    console_register("rot", cmd_rot);
    console_register("shake", cmd_shake);
    console_register("wifi", cmd_wifi);
    console_register("web", cmd_web);
    console_register("webjob", cmd_webjob);
    console_register("upd", cmd_upd);
    console_register("pair", cmd_pair);
    console_register("ui", cmd_ui);
    console_register("otafail", cmd_otafail);
    console_start();
#endif
    confirm_firmware(store_ok, ui_ok);
}
