#include "hy/audio.h"

#include <stdatomic.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
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

static atomic_bool s_pending = true;

/* The BSP's I2S pins and format (waveshare esp32_s3_touch_amoled_2_16 2.0.1, BSP_I2S_DUPLEX_MONO_CFG(22050) and
 * BSP_I2S_GPIO_CFG in its .c file), TX only. The BSP's bsp_audio_init creates a TX + RX pair and enables both, and
 * an enabled I2S channel holds an APB_FREQ_MAX PM lock, which keeps the chip out of automatic light sleep for good.
 * This channel is left disabled: esp_codec_dev_open enables it and esp_codec_dev_close disables it, so the lock is
 * held only while a chime plays. */
#define SPK_RATE 22050

/* The ES8311 speaker device as bsp_audio_codec_speaker_init builds it (same address, PA pin, gain), on a TX-only
 * data interface. Nothing is released on a failure here or later (the device has no deinit path). */
static esp_err_t speaker_init(void)
{
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c init: %s", esp_err_to_name(err));
        return err;
    }
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   /* as the BSP: stale DMA data is cleared */
    err = i2s_new_channel(&chan_cfg, &tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s channel: %s", esp_err_to_name(err));
        return err;
    }
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,   /* no RX channel */
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    err = i2s_channel_init_std_mode(tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s std mode: %s", esp_err_to_name(err));
        i2s_del_channel(tx);
        return err;
    }
    audio_codec_i2s_cfg_t i2s_cfg = {.port = CONFIG_BSP_I2S_NUM, .rx_handle = NULL, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_cfg = {.port = BSP_I2C_NUM, .addr = ES8311_CODEC_DEFAULT_ADDR,
                                     .bus_handle = bsp_i2c_get_handle()};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!data_if || !gpio_if || !ctrl_if) {
        ESP_LOGE(TAG, "speaker codec init failed");
        return ESP_FAIL;
    }
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *es = es8311_codec_new(&es_cfg);
    if (!es) {
        ESP_LOGE(TAG, "speaker codec init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es, .data_if = data_if};
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) {
        ESP_LOGE(TAG, "speaker codec init failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool hy_audio_pending(void)
{
    return atomic_load(&s_pending);
}

static esp_err_t audio_init_impl(void)
{
    if (s_task) {
        return ESP_OK;
    }
    esp_err_t err = speaker_init();
    if (err != ESP_OK) {
        return err;
    }
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

esp_err_t hy_audio_init(void)
{
    esp_err_t err = audio_init_impl();
    atomic_store(&s_pending, false);
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
