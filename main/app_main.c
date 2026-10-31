#include "esp_app_desc.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "hy/audio.h"
#include "hy/board.h"
#include "hy/actions.h"
#include "hy/clock.h"
#include "hy/motion.h"
#include "hy/motion_run.h"
#include "hy/power_run.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "hy/ui.h"
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
    /* The motion task owns the IMU: it prints the samples from its own reads. `imu regs` is a direct register read. */
    bool raw = strncmp(args, "raw", 3) == 0;
    int secs = 0;
    bool rec = sscanf(args, "rec %d", &secs) == 1;
    if (strncmp(args, "regs", 4) == 0) {
        uint8_t r[3];
        esp_err_t rerr = hy_board_imu_regs(r);
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "imu regs: %s", esp_err_to_name(rerr));
            return;
        }
        printf("imu regs: CTRL1=0x%02x CTRL2=0x%02x CTRL7=0x%02x\n", r[0], r[1], r[2]);
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
#endif

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

static void init_storage(void)
{
    esp_err_t err = hy_store_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage unavailable, no persistence: %s", esp_err_to_name(err));
        hy_ui_set_storage_ok(false);
        return;
    }
    hy_err_t oerr = hy_store_open("/data");
    ESP_LOGI(TAG, "hy_store_open: %s", hy_err_str(oerr));
    if (oerr != HY_OK) {
        hy_ui_set_storage_ok(false);
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

/* The orientation before the splash's first frame: the saved one when locked, else one IMU sample, else the saved one */
static void boot_rotation(const hy_settings_t *settings)
{
    hy_rot_t rot = (hy_rot_t)(settings->rot & 3);
    const char *why = "saved";
    if (settings->rot_lock) {
        why = "locked";
    } else {
        int16_t mg[3];
        hy_rot_t cand;
        if (hy_board_imu_sample_once(mg) == ESP_OK && hy_orient_candidate((hy_accel_t){mg[0], mg[1], mg[2]}, &cand)) {
            rot = cand;
            why = "imu";
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

    init_storage();
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
    if (err == ESP_OK) {
        esp_err_t uerr = hy_ui_start();
        ESP_LOGI(TAG, "hy_ui_start: %s", esp_err_to_name(uerr));
        hy_power_start();
        esp_err_t aerr = hy_audio_init();   /* about 0.4 s: not on the splash's path */
        ESP_LOGI(TAG, "hy_audio_init: %s", esp_err_to_name(aerr));
    }

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
    console_start();
#endif
}
