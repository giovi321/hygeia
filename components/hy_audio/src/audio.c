#include "hy/audio.h"

#include <stdatomic.h>
#include <string.h>

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
static size_t s_samples;   /* chime samples */
static size_t s_total;     /* chime plus trailing silence */
static TaskHandle_t s_task;
static atomic_bool s_ok;
static atomic_bool s_busy;
static atomic_int s_volume = 60;

#define TAIL_SILENCE_MS 100   /* lets the DMA queue drain before close so the decay is not cut */

/* esp_codec_dev 1.5.11 disables the I2S TX channel unconditionally in its set_fmt step (open), so every
 * open after a close logs an error from "i2s_common" ("channel has not been enabled yet") and playback
 * still succeeds. Silence these tags around open and close only. */
static const char *const k_quiet_tags[] = {"i2s_common", "I2S_IF", "Adev_Codec"};
#define N_QUIET (sizeof k_quiet_tags / sizeof k_quiet_tags[0])

static void quiet_begin(esp_log_level_t saved[N_QUIET])
{
    for (size_t i = 0; i < N_QUIET; i++) {
        saved[i] = esp_log_level_get(k_quiet_tags[i]);
        esp_log_level_set(k_quiet_tags[i], ESP_LOG_NONE);
    }
}

static void quiet_end(const esp_log_level_t saved[N_QUIET])
{
    for (size_t i = 0; i < N_QUIET; i++) {
        esp_log_level_set(k_quiet_tags[i], saved[i]);
    }
}

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
        esp_log_level_t saved[N_QUIET];
        quiet_begin(saved);
        int err = esp_codec_dev_open(s_dev, &fs);
        quiet_end(saved);
        if (err == ESP_CODEC_DEV_OK) {
            esp_codec_dev_set_out_vol(s_dev, atomic_load(&s_volume));
            err = esp_codec_dev_write(s_dev, s_buf, (int)(s_total * sizeof(int16_t)));
        }
        /* Close also after a failed open: the library marks the output opened before configuring the
         * codec, so without a close the next open would return OK early with the codec unconfigured. */
        quiet_begin(saved);
        int cerr = esp_codec_dev_close(s_dev);
        quiet_end(saved);
        if (err == ESP_CODEC_DEV_OK) {
            err = cerr;
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
    /* The BSP has no deinit: on a failure below the I2S channels (RX stays enabled) and the data
     * interface remain allocated; only the codec device handle is released here. */
    size_t n = hy_chime_len(HY_CHIME_RATE);
    size_t silence = (size_t)HY_CHIME_RATE * TAIL_SILENCE_MS / 1000;
    s_buf = heap_caps_malloc((n + silence) * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "no PSRAM for the chime (%u bytes)", (unsigned)((n + silence) * sizeof(int16_t)));
        goto fail;
    }
    s_samples = hy_chime_render(s_buf, n, HY_CHIME_RATE);
    if (s_samples == 0) {
        goto fail;
    }
    memset(s_buf + s_samples, 0, silence * sizeof(int16_t));
    s_total = s_samples + silence;
    if (xTaskCreate(audio_task, "hy_audio", 4096, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        err = ESP_ERR_NO_MEM;
        goto fail_free;
    }
    atomic_store(&s_ok, true);
    ESP_LOGI(TAG, "ready, chime %u samples", (unsigned)s_samples);
    return ESP_OK;

fail:
    err = ESP_FAIL;
    if (!s_buf) {
        err = ESP_ERR_NO_MEM;
    }
fail_free:
    heap_caps_free(s_buf);
    s_buf = NULL;
    s_samples = s_total = 0;
    esp_codec_dev_delete(s_dev);
    s_dev = NULL;
    return err;
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
        ESP_LOGW(TAG, "chime ignored: audio not available");
        return;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        ESP_LOGW(TAG, "chime dropped: one is already playing");
        return;
    }
    xTaskNotifyGive(s_task);
}
