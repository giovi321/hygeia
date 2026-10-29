#include "hy/clock.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "hy/rtc_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hy/tzlock.h"
#include "nvs.h"
#include "pcf85063.h"

#ifndef HY_BUILD_EPOCH
#error "HY_BUILD_EPOCH must be defined by the component CMakeLists"
#endif

#define NVS_NS "clock"
#define NVS_KEY_TZ "tz"
#define NVS_KEY_ZONE "zone"
#define NVS_KEY_CHG_AT "chg_at"
#define NVS_KEY_CHG_OLD "chg_old"
#define NVS_KEY_CHG_NEW "chg_new"
#define SRC_LEN 16
#define RTC_INT_GPIO 13

static const char *TAG = "clock";

/*
 * Shared between the console task (writer) and others (readers such as the port functions called
 * from the UI task). All state below is read and written inside s_lock. The TZ is double
 * buffered: the writer fills the inactive buffer and swaps the index in one critical section, so
 * the pointer returned by hy_clock_tz() / port tz() always holds a complete string. It stays
 * intact until the second following TZ change; hy_clock_tz_copy() has no such limit.
 *
 * The process TZ environment (setenv/tzset/localtime_r/mktime) is guarded separately by the
 * recursive s_tz_mutex, registered with hy_tz_set_lock so hy_core takes the same lock.
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_valid;
static char s_tzbuf[2][HY_TZ_LEN] = { HY_CLOCK_DEFAULT_TZ, "" };
static int s_tz_idx;
static char s_zone[HY_ZONE_LEN] = HY_CLOCK_DEFAULT_ZONE;
static char s_src[SRC_LEN];
static int64_t s_sync_at;
static uint32_t s_sync_seq;
static int64_t s_sync_jump;
static bool s_have_chg;
static hy_tzchange_t s_chg;

/* Alarm: s_alarm_utc is the programmed value (0 = none, ALARM_UNKNOWN after an I2C error), guarded by s_alarm_mutex; the ISR only sets s_alarm_flag */
static StaticSemaphore_t s_alarm_mutex_buf;
static SemaphoreHandle_t s_alarm_mutex;
#define ALARM_UNKNOWN INT64_MIN /* chip alarm state not known: the next set always writes */
static int64_t s_alarm_utc = ALARM_UNKNOWN;
static volatile bool s_alarm_flag;

static StaticSemaphore_t s_tz_mutex_buf;
static SemaphoreHandle_t s_tz_mutex;

static void tz_mutex_take(void *ctx)
{
    xSemaphoreTakeRecursive((SemaphoreHandle_t)ctx, portMAX_DELAY);
}

static void tz_mutex_give(void *ctx)
{
    xSemaphoreGiveRecursive((SemaphoreHandle_t)ctx);
}

static void set_valid(bool v)
{
    portENTER_CRITICAL(&s_lock);
    s_valid = v;
    portEXIT_CRITICAL(&s_lock);
}

static const char *tz_current(void)
{
    portENTER_CRITICAL(&s_lock);
    const char *p = s_tzbuf[s_tz_idx];
    portEXIT_CRITICAL(&s_lock);
    return p;
}

static void tz_publish(const char *tz)
{
    portENTER_CRITICAL(&s_lock);
    int next = 1 - s_tz_idx;
    strlcpy(s_tzbuf[next], tz, HY_TZ_LEN);
    s_tz_idx = next;
    portEXIT_CRITICAL(&s_lock);
}

static void zone_publish(const char *zone)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_zone, zone, sizeof s_zone);
    portEXIT_CRITICAL(&s_lock);
}

static void chg_publish(const hy_tzchange_t *ch)
{
    portENTER_CRITICAL(&s_lock);
    s_chg = *ch;
    s_have_chg = true;
    portEXIT_CRITICAL(&s_lock);
}

static void tz_apply(void)
{
    char tz[HY_TZ_LEN];
    hy_clock_tz_copy(tz, sizeof tz);
    hy_tz_lock();
    setenv("TZ", tz, 1);
    tzset();
    hy_tz_unlock();
}

/* Reads a string key into out; an absent key leaves out untouched (ESP_ERR_NVS_NOT_FOUND) */
static esp_err_t load_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    char buf[HY_ZONE_LEN > HY_TZ_LEN ? HY_ZONE_LEN : HY_TZ_LEN];
    size_t n = sizeof buf;
    esp_err_t err = nvs_get_str(h, key, buf, &n);
    if (err == ESP_OK) {
        strlcpy(out, buf, len);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "reading %s from NVS: %s", key, esp_err_to_name(err));
    }
    return err;
}

static void load_tz(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return; /* namespace absent: keep the defaults */
    }
    char tz[HY_TZ_LEN] = "";
    if (load_str(h, NVS_KEY_TZ, tz, sizeof tz) == ESP_OK && tz[0] != '\0') {
        tz_publish(tz);
    }
    char zone[HY_ZONE_LEN];
    if (load_str(h, NVS_KEY_ZONE, zone, sizeof zone) == ESP_OK) {
        zone_publish(zone);
    } else if (tz[0] != '\0' && strcmp(tz, HY_CLOCK_DEFAULT_TZ) != 0) {
        zone_publish(""); /* a raw POSIX zone stored before zone names existed */
    }
    hy_tzchange_t ch;
    memset(&ch, 0, sizeof ch);
    if (nvs_get_i64(h, NVS_KEY_CHG_AT, &ch.at) == ESP_OK &&
        load_str(h, NVS_KEY_CHG_OLD, ch.old_tz, sizeof ch.old_tz) == ESP_OK &&
        load_str(h, NVS_KEY_CHG_NEW, ch.new_tz, sizeof ch.new_tz) == ESP_OK) {
        chg_publish(&ch);
    }
    nvs_close(h);
}

/* Persists tz and zone, plus the change record when ch is not NULL, with one commit */
static esp_err_t save_zone(const char *tz, const char *zone, const hy_tzchange_t *ch)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_TZ, tz);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_ZONE, zone);
    }
    if (err == ESP_OK && ch) {
        err = nvs_set_i64(h, NVS_KEY_CHG_AT, ch->at);
        if (err == ESP_OK) {
            err = nvs_set_str(h, NVS_KEY_CHG_OLD, ch->old_tz);
        }
        if (err == ESP_OK) {
            err = nvs_set_str(h, NVS_KEY_CHG_NEW, ch->new_tz);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/*
 * Puts tz in force (RAM and the process TZ) inside the TZ lock, so no reader sees the zone half
 * switched. When tz differs from the current POSIX string the change {at, old, tz} is recorded
 * in RAM and copied to *ch. The zone name becomes `zone`; zone NULL keeps the current name when
 * the POSIX string is unchanged and clears it when it changed. The resulting name is copied to
 * zone_out. Returns true when a change was recorded.
 */
static bool zone_switch(const char *tz, const char *zone, int64_t at, hy_tzchange_t *ch,
                        char *zone_out, size_t zone_out_len)
{
    hy_tz_lock();
    char old[HY_TZ_LEN];
    hy_clock_tz_copy(old, sizeof old);
    bool changed = strcmp(old, tz) != 0;
    if (changed) {
        memset(ch, 0, sizeof *ch);
        ch->at = at;
        strlcpy(ch->old_tz, old, sizeof ch->old_tz);
        strlcpy(ch->new_tz, tz, sizeof ch->new_tz);
        chg_publish(ch);
    }
    if (zone) {
        zone_publish(zone);
    } else if (changed) {
        zone_publish("");
    }
    tz_publish(tz);
    tz_apply();
    hy_clock_zone_copy(zone_out, zone_out_len);
    hy_tz_unlock();
    return changed;
}

static void set_system_time(int64_t utc)
{
    struct timeval tv = { .tv_sec = (time_t)utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static void IRAM_ATTR rtc_int_isr(void *arg)
{
    (void)arg;
    s_alarm_flag = true;
}

/* INT is open-drain active low: input, pull-up, falling edge */
static esp_err_t alarm_gpio_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << RTC_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    /* The service may already be installed elsewhere; the driver then logs an error line for
     * ESP_ERR_INVALID_STATE, which is expected here, so silence its tag around the call. */
    esp_log_level_t old = esp_log_level_get("gpio");
    esp_log_level_set("gpio", ESP_LOG_NONE);
    err = gpio_install_isr_service(0);
    esp_log_level_set("gpio", old);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return gpio_isr_handler_add(RTC_INT_GPIO, rtc_int_isr, NULL);
}

esp_err_t hy_clock_init(i2c_master_bus_handle_t bus)
{
    if (!s_alarm_mutex) {
        s_alarm_mutex = xSemaphoreCreateMutexStatic(&s_alarm_mutex_buf);
    }
    if (!s_tz_mutex) {
        s_tz_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_tz_mutex_buf);
        hy_tz_set_lock(tz_mutex_take, tz_mutex_give, s_tz_mutex);
    }
    load_tz();
    tz_apply();

    esp_err_t err = pcf85063_init(bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC add device: %s", esp_err_to_name(err));
        return err;
    }
    pcf85063_time_t t;
    err = pcf85063_read(&t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC read: %s", esp_err_to_name(err));
        return err;
    }
    /* The chip is battery backed and may still hold an armed alarm: disarm it and clear AF once */
    esp_err_t aerr = pcf85063_alarm_set(NULL);
    if (aerr == ESP_OK) {
        s_alarm_utc = 0;
    } else {
        ESP_LOGE(TAG, "RTC alarm disarm: %s", esp_err_to_name(aerr));
    }
    esp_err_t gerr = alarm_gpio_init();
    if (gerr != ESP_OK) {
        ESP_LOGE(TAG, "RTC INT gpio %d: %s", RTC_INT_GPIO, esp_err_to_name(gerr));
    }
    bool ok = t.decoded && hy_clock_is_plausible(t.epoch, t.osc_stopped, HY_BUILD_EPOCH);
    if (ok) {
        set_system_time(t.epoch);
        set_valid(true);
        ESP_LOGI(TAG, "clock valid: rtc utc %lld, tz %s", (long long)t.epoch, tz_current());
    } else {
        set_valid(false);
        ESP_LOGW(TAG, "clock invalid: osc_stopped=%d decoded=%d rtc_epoch=%lld build_epoch=%lld "
                 "raw=%02x %02x %02x %02x %02x %02x %02x ctrl1=%02x",
                 t.osc_stopped, t.decoded, (long long)t.epoch, (long long)HY_BUILD_EPOCH,
                 t.raw[0], t.raw[1], t.raw[2], t.raw[3], t.raw[4], t.raw[5], t.raw[6], t.ctrl1);
    }
    return ESP_OK;
}

bool hy_clock_valid(void)
{
    portENTER_CRITICAL(&s_lock);
    bool v = s_valid;
    portEXIT_CRITICAL(&s_lock);
    return v;
}

esp_err_t hy_clock_set(int64_t utc, const char *posix_tz, const char *source)
{
    char tzcopy[HY_TZ_LEN];
    if (posix_tz) {
        size_t n = strlen(posix_tz);
        if (n == 0 || n >= HY_TZ_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(tzcopy, posix_tz, n + 1);
    } else {
        hy_clock_tz_copy(tzcopy, sizeof tzcopy);
    }
    if (!hy_clock_is_plausible(utc, false, HY_BUILD_EPOCH)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = pcf85063_write(utc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC write: %s", esp_err_to_name(err));
        return err;
    }
    int64_t jump = utc - (int64_t)time(NULL);
    set_system_time(utc);
    /* The zone is in force even if persisting it fails. A differing POSIX string records a
     * change and clears the IANA name; an unchanged one keeps the name. */
    hy_tzchange_t ch;
    char zone[HY_ZONE_LEN];
    bool changed = zone_switch(tzcopy, NULL, utc, &ch, zone, sizeof zone);
    esp_err_t nvs_err = save_zone(tzcopy, zone, changed ? &ch : NULL);
    portENTER_CRITICAL(&s_lock);
    s_valid = true;
    strlcpy(s_src, source ? source : "", sizeof s_src);
    s_sync_at = utc;
    s_sync_jump = jump;
    s_sync_seq++;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "clock set via %s: utc %lld, jump %lld s, tz %s%s", source ? source : "",
             (long long)utc, (long long)jump, tzcopy, changed ? ", zone change recorded" : "");
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "tz in force but not persisted, NVS: %s", esp_err_to_name(nvs_err));
    }
    return nvs_err;
}

esp_err_t hy_clock_set_zone(const char *iana, const char *posix_tz)
{
    if (!posix_tz) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(posix_tz);
    if (n == 0 || n >= HY_TZ_LEN || (iana && strlen(iana) >= HY_ZONE_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!hy_clock_valid()) {
        return ESP_ERR_INVALID_STATE;
    }
    char tzcopy[HY_TZ_LEN];
    memcpy(tzcopy, posix_tz, n + 1);
    hy_tzchange_t ch;
    char zone[HY_ZONE_LEN];
    bool changed = zone_switch(tzcopy, iana ? iana : "", (int64_t)time(NULL), &ch, zone, sizeof zone);
    esp_err_t nvs_err = save_zone(tzcopy, zone, changed ? &ch : NULL);
    ESP_LOGI(TAG, "zone set: %s, tz %s%s", zone[0] ? zone : "(none)", tzcopy,
             changed ? ", zone change recorded" : "");
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "zone in force but not persisted, NVS: %s", esp_err_to_name(nvs_err));
    }
    return nvs_err;
}

esp_err_t hy_clock_alarm_set(int64_t utc)
{
    uint8_t raw[5];
    if (utc != 0 && !hy_rtc_alarm_encode(utc, raw)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (utc != 0 && !hy_clock_valid()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_alarm_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A target at or before now + 2 s cannot be armed: the chip matches day of month, so it would
     * fire about a month late. Disarm and raise the software flag so the next take returns true. */
    bool due = utc != 0 && utc <= (int64_t)time(NULL) + 2;
    int64_t want = due ? 0 : utc;
    xSemaphoreTake(s_alarm_mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (want != s_alarm_utc) {
        s_alarm_flag = false; /* a stale pending interrupt must not disarm the new alarm */
        err = pcf85063_alarm_set(want ? raw : NULL);
        if (err == ESP_OK) {
            s_alarm_utc = want;
        } else {
            s_alarm_utc = ALARM_UNKNOWN;
            ESP_LOGE(TAG, "RTC alarm write: %s", esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        if (due) {
            s_alarm_flag = true;
        }
        ESP_LOGI(TAG, "rtc alarm %s", due ? "due now" : (want ? "set" : "disabled"));
    }
    xSemaphoreGive(s_alarm_mutex);
    return err;
}

bool hy_clock_alarm_take(void)
{
    if (!s_alarm_flag || !s_alarm_mutex) {
        return false;
    }
    xSemaphoreTake(s_alarm_mutex, portMAX_DELAY);
    s_alarm_flag = false;
    /* One shot: clears AIE and AF so the day-of-month match does not repeat */
    esp_err_t err = pcf85063_alarm_set(NULL);
    if (err == ESP_OK) {
        s_alarm_utc = 0;
    } else {
        s_alarm_utc = ALARM_UNKNOWN;
        ESP_LOGE(TAG, "RTC alarm clear: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_alarm_mutex);
    return true;
}

const char *hy_clock_tz(void)
{
    return tz_current();
}

int32_t hy_clock_today_ymd(void)
{
    if (!hy_clock_valid()) return 0;
    time_t now = time(NULL);
    struct tm l;
    hy_tz_lock();
    localtime_r(&now, &l);
    hy_tz_unlock();
    return (int32_t)((l.tm_year + 1900) * 10000 + (l.tm_mon + 1) * 100 + l.tm_mday);
}

void hy_clock_tz_copy(char *out, size_t len)
{
    if (!out || len == 0) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(out, s_tzbuf[s_tz_idx], len);
    portEXIT_CRITICAL(&s_lock);
}

void hy_clock_zone_copy(char *out, size_t len)
{
    if (!out || len == 0) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(out, s_zone, len);
    portEXIT_CRITICAL(&s_lock);
}

bool hy_clock_last_tzchange(hy_tzchange_t *out)
{
    portENTER_CRITICAL(&s_lock);
    bool have = s_have_chg;
    if (have && out) {
        *out = s_chg;
    }
    portEXIT_CRITICAL(&s_lock);
    return have;
}

uint32_t hy_clock_sync_seq(int64_t *jump_s)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t seq = s_sync_seq;
    if (jump_s) {
        *jump_s = s_sync_jump;
    }
    portEXIT_CRITICAL(&s_lock);
    return seq;
}

void hy_clock_last_sync(char *src, size_t src_len, int64_t *at)
{
    portENTER_CRITICAL(&s_lock);
    if (src && src_len) {
        strlcpy(src, s_src, src_len);
    }
    if (at) {
        *at = s_sync_at;
    }
    portEXIT_CRITICAL(&s_lock);
}

void hy_clock_report(void)
{
    pcf85063_time_t t;
    esp_err_t err = pcf85063_read(&t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC read: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "rtc raw=%02x %02x %02x %02x %02x %02x %02x ctrl1=%02x osc_stopped=%d decoded=%d utc=%lld",
                 t.raw[0], t.raw[1], t.raw[2], t.raw[3], t.raw[4], t.raw[5], t.raw[6], t.ctrl1,
                 t.osc_stopped, t.decoded, (long long)t.epoch);
    }
    time_t now = time(NULL);
    struct tm u, l;
    char us[24], ls[24];
    gmtime_r(&now, &u);
    hy_tz_lock();
    localtime_r(&now, &l);
    hy_tz_unlock();
    strftime(us, sizeof us, "%Y-%m-%d %H:%M:%S", &u);
    strftime(ls, sizeof ls, "%Y-%m-%d %H:%M:%S", &l);
    char tz[HY_TZ_LEN], zone[HY_ZONE_LEN];
    hy_clock_tz_copy(tz, sizeof tz);
    hy_clock_zone_copy(zone, sizeof zone);
    ESP_LOGI(TAG, "system utc=%lld (%s) local=%s tz=%s zone=%s", (long long)now, us, ls, tz,
             zone[0] ? zone : "(none)");
    int64_t at, jump;
    char src[SRC_LEN];
    hy_clock_last_sync(src, sizeof src, &at);
    uint32_t seq = hy_clock_sync_seq(&jump);
    ESP_LOGI(TAG, "valid=%d last_sync_source=%s last_sync_utc=%lld sync_seq=%u jump=%lld",
             hy_clock_valid(), src[0] ? src : "none", (long long)at, (unsigned)seq, (long long)jump);
    hy_tzchange_t ch;
    if (hy_clock_last_tzchange(&ch)) {
        ESP_LOGI(TAG, "tz change at=%lld old=%s new=%s", (long long)ch.at, ch.old_tz, ch.new_tz);
    } else {
        ESP_LOGI(TAG, "tz change none");
    }
}

static int64_t port_now(void *ctx)
{
    (void)ctx;
    return (int64_t)time(NULL);
}

static const char *port_tz(void *ctx)
{
    (void)ctx;
    return tz_current();
}

static bool port_valid(void *ctx)
{
    (void)ctx;
    return hy_clock_valid();
}

static bool port_last_change(void *ctx, hy_tzchange_t *out)
{
    (void)ctx;
    return hy_clock_last_tzchange(out);
}

const hy_clock_port_t *hy_clock_port(void)
{
    static const hy_clock_port_t port = {
        .now = port_now, .tz = port_tz, .valid = port_valid, .last_change = port_last_change,
    };
    return &port;
}
