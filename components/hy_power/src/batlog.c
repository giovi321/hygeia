#include "batlog.h"

#include <inttypes.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BATLOG_FILE  "/data/batlog.csv"
#define BATLOG_OLD   "/data/batlog.1.csv"
#define BATLOG_LIMIT (256 * 1024)
#define BATLOG_HEAD  "ts,mv,pct,raw_pct,vbus,chg,scr\n"

static const char *TAG = "batlog";
static SemaphoreHandle_t s_mu;

void batlog_init(void)
{
    static StaticSemaphore_t buf;
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutexStatic(&buf);
    }
}

static bool lock(void)
{
    return s_mu && xSemaphoreTake(s_mu, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s_mu);
}

void batlog_append(int64_t ts, const hy_batt_raw_t *raw, int8_t pct, uint8_t scr)
{
    if (!lock()) {
        return;
    }
    struct stat st;
    bool exists = stat(BATLOG_FILE, &st) == 0;
    if (exists && st.st_size >= BATLOG_LIMIT) {
        remove(BATLOG_OLD);
        if (rename(BATLOG_FILE, BATLOG_OLD) != 0) {
            ESP_LOGE(TAG, "rotate failed");
            remove(BATLOG_FILE);
        } else {
            ESP_LOGI(TAG, "rotated to %s", BATLOG_OLD);
        }
        exists = false;
    }
    FILE *f = fopen(BATLOG_FILE, "a");
    if (!f) {
        static bool logged;
        if (!logged) {
            ESP_LOGE(TAG, "open %s failed", BATLOG_FILE);
            logged = true;
        }
        unlock();
        return;
    }
    if (!exists) {
        fputs(BATLOG_HEAD, f);
    }
    fprintf(f, "%" PRId64 ",%u,%d,%d,%d,%d,%u\n", ts, (unsigned)raw->mv, pct, raw->pct, raw->vbus ? 1 : 0,
            (int)raw->chg, (unsigned)scr);
    fclose(f);
    unlock();
}

#if CONFIG_HY_DEBUG_CONSOLE
static void print_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("batlog: %s missing\n", path);
        return;
    }
    printf("batlog: %s\n", path);
    char line[96];
    while (fgets(line, sizeof line, f)) {
        fputs(line, stdout);
    }
    fclose(f);
    fflush(stdout);
}

void batlog_print(void)
{
    if (!lock()) {
        return;
    }
    print_file(BATLOG_OLD);
    print_file(BATLOG_FILE);
    unlock();
}

void batlog_clear(void)
{
    if (!lock()) {
        return;
    }
    remove(BATLOG_OLD);
    remove(BATLOG_FILE);
    unlock();
    ESP_LOGI(TAG, "cleared");
}
#endif
