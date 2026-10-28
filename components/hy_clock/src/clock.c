#include "hy/clock.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "pcf85063.h"

#ifndef HY_BUILD_EPOCH
#error "HY_BUILD_EPOCH must be defined by the component CMakeLists"
#endif

#define NVS_NS "clock"
#define NVS_KEY_TZ "tz"
#define SRC_LEN 16

static const char *TAG = "clock";

/*
 * Shared between the console task (writer) and others (readers such as the port functions called
 * from the UI task). All state below is read and written inside s_lock. The TZ is double
 * buffered: the writer fills the inactive buffer and swaps the index in one critical section, so
 * the pointer returned by hy_clock_tz() / port tz() always holds a complete string. It stays
 * intact until the second following TZ change.
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_valid;
static char s_tzbuf[2][HY_TZ_LEN] = { HY_CLOCK_DEFAULT_TZ, "" };
static int s_tz_idx;
static char s_src[SRC_LEN];
static int64_t s_sync_at;

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

static void apply_tz(void)
{
    setenv("TZ", tz_current(), 1);
    tzset();
}

static void load_tz(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return; /* namespace absent: keep the default */
    }
    char buf[HY_TZ_LEN];
    size_t len = sizeof buf;
    esp_err_t err = nvs_get_str(h, NVS_KEY_TZ, buf, &len);
    nvs_close(h);
    if (err == ESP_OK && buf[0] != '\0') {
        tz_publish(buf);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "reading tz from NVS: %s", esp_err_to_name(err));
    }
}

static esp_err_t save_tz(const char *tz)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_TZ, tz);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void set_system_time(int64_t utc)
{
    struct timeval tv = { .tv_sec = (time_t)utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

esp_err_t hy_clock_init(i2c_master_bus_handle_t bus)
{
    load_tz();
    apply_tz();

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
    const char *tz = posix_tz ? posix_tz : tz_current();
    size_t n = strlen(tz);
    if (n == 0 || n >= HY_TZ_LEN || !hy_clock_is_plausible(utc, false, HY_BUILD_EPOCH)) {
        return ESP_ERR_INVALID_ARG;
    }
    char tzcopy[HY_TZ_LEN];
    strlcpy(tzcopy, tz, sizeof tzcopy); /* tz may point at the buffer that is about to be replaced */
    esp_err_t err = pcf85063_write(utc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC write: %s", esp_err_to_name(err));
        return err;
    }
    set_system_time(utc);
    /* The zone is in force even if persisting it fails */
    tz_publish(tzcopy);
    apply_tz();
    esp_err_t nvs_err = save_tz(tzcopy);
    portENTER_CRITICAL(&s_lock);
    s_valid = true;
    strlcpy(s_src, source ? source : "", sizeof s_src);
    s_sync_at = utc;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "clock set via %s: utc %lld, tz %s", source ? source : "", (long long)utc, tzcopy);
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "tz in force but not persisted, NVS: %s", esp_err_to_name(nvs_err));
    }
    return nvs_err;
}

const char *hy_clock_tz(void)
{
    return tz_current();
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
    localtime_r(&now, &l);
    strftime(us, sizeof us, "%Y-%m-%d %H:%M:%S", &u);
    strftime(ls, sizeof ls, "%Y-%m-%d %H:%M:%S", &l);
    ESP_LOGI(TAG, "system utc=%lld (%s) local=%s tz=%s", (long long)now, us, ls, tz_current());
    int64_t at;
    char src[SRC_LEN];
    hy_clock_last_sync(src, sizeof src, &at);
    ESP_LOGI(TAG, "valid=%d last_sync_source=%s last_sync_utc=%lld", hy_clock_valid(), src[0] ? src : "none", (long long)at);
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

const hy_clock_port_t *hy_clock_port(void)
{
    static const hy_clock_port_t port = { .now = port_now, .tz = port_tz, .valid = port_valid };
    return &port;
}
