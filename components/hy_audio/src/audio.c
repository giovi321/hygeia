#include "hy/audio.h"

#include <stdatomic.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hy/chime.h"

static const char *TAG = "hy_audio";

static esp_codec_dev_handle_t s_dev;
static int16_t *s_buf;
static size_t s_samples;
static TaskHandle_t s_task;
static atomic_bool s_ok;
static atomic_bool s_busy;
static atomic_int s_volume = 60;

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .sample_rate = HY_CHIME_RATE,
        };
        int err = esp_codec_dev_open(s_dev, &fs);
        if (err == ESP_CODEC_DEV_OK) {
            esp_codec_dev_set_out_vol(s_dev, atomic_load(&s_volume));
            err = esp_codec_dev_write(s_dev, s_buf, (int)(s_samples * sizeof(int16_t)));
            int cerr = esp_codec_dev_close(s_dev);
            if (err == ESP_CODEC_DEV_OK) {
                err = cerr;
            }
        }
        if (err == ESP_CODEC_DEV_OK) {
            ESP_LOGI(TAG, "chime played in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
            atomic_store(&s_ok, true);
        } else {
            ESP_LOGE(TAG, "chime failed: %d", err);
            atomic_store(&s_ok, false);
        }
        atomic_store(&s_busy, false);
    }
}

esp_err_t hy_audio_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    esp_err_t err = bsp_audio_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init: %s", esp_err_to_name(err));
        return err;
    }
    s_dev = bsp_audio_codec_speaker_init();
    if (!s_dev) {
        ESP_LOGE(TAG, "speaker codec init failed");
        return ESP_FAIL;
    }
    size_t n = hy_chime_len(HY_CHIME_RATE);
    s_buf = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "no PSRAM for the chime (%u bytes)", (unsigned)(n * sizeof(int16_t)));
        return ESP_ERR_NO_MEM;
    }
    s_samples = hy_chime_render(s_buf, n, HY_CHIME_RATE);
    if (s_samples == 0) {
        heap_caps_free(s_buf);
        s_buf = NULL;
        return ESP_FAIL;
    }
    if (xTaskCreate(audio_task, "hy_audio", 4096, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        heap_caps_free(s_buf);
        s_buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_ok, true);
    ESP_LOGI(TAG, "ready, chime %u samples", (unsigned)s_samples);
    return ESP_OK;
}

bool hy_audio_ok(void)
{
    return atomic_load(&s_ok);
}

void hy_audio_set_volume(uint8_t pct)
{
    atomic_store(&s_volume, pct > 100 ? 100 : pct);
}

void hy_audio_chime(void)
{
    if (!s_task) {
        return;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        return;   /* one is playing: dropped */
    }
    xTaskNotifyGive(s_task);
}
