#include "hy/audio.h"

#include <math.h>
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
static size_t s_total;     /* leading silence, chime and trailing silence */
static int16_t *s_tone[HY_TONE_N];     /* rendered cues, same rate as the chime, with the tail silence */
static size_t s_tone_total[HY_TONE_N];
static TaskHandle_t s_task;
static atomic_bool s_ok;
static atomic_int s_volume = 60;

/* Play requests: a ring of HY_AUDIO_QUEUE_N, so two cues in a row both play. The chime queues behind the cues (a due
 * chime must not be lost during a teach or the counting test); it is dropped only while another chime plays or
 * waits. */
#define HY_AUDIO_QUEUE_N 4
typedef struct { const int16_t *buf; size_t bytes; bool chime; } play_req_t;
static portMUX_TYPE s_qmux = portMUX_INITIALIZER_UNLOCKED;
static play_req_t s_q[HY_AUDIO_QUEUE_N];
static uint8_t s_q_head, s_q_n;
static bool s_chime_playing;     /* the request being played is the chime (under s_qmux), set in q_pop */

/* false when the ring is full, or for the chime when another chime plays or waits */
static bool q_push(play_req_t r, bool chime)
{
    bool ok = false;
    portENTER_CRITICAL(&s_qmux);
    bool chime_busy = false;
    if (chime) {
        chime_busy = s_chime_playing;
        for (uint8_t i = 0; i < s_q_n && !chime_busy; i++) {
            chime_busy = s_q[(s_q_head + i) % HY_AUDIO_QUEUE_N].chime;
        }
    }
    if (!chime_busy && s_q_n < HY_AUDIO_QUEUE_N) {
        r.chime = chime;
        s_q[(s_q_head + s_q_n) % HY_AUDIO_QUEUE_N] = r;
        s_q_n++;
        ok = true;
    }
    portEXIT_CRITICAL(&s_qmux);
    return ok;
}

/* The next request, or false when none is left (and then nothing is playing). The chime playing flag follows the
 * popped request and is cleared here when the queue is empty. */
static bool q_pop(play_req_t *r)
{
    bool have = false;
    portENTER_CRITICAL(&s_qmux);
    if (s_q_n > 0) {
        *r = s_q[s_q_head];
        s_q_head = (uint8_t)((s_q_head + 1) % HY_AUDIO_QUEUE_N);
        s_q_n--;
        s_chime_playing = r->chime;
        have = true;
    } else {
        s_chime_playing = false;
    }
    portEXIT_CRITICAL(&s_qmux);
    return have;
}

#define TAIL_SILENCE_MS 100   /* lets the DMA queue drain before close so the decay is not cut */
#define HEAD_SILENCE_MS 200   /* covers the codec and amplifier start after each open, which ate the first note of a cue */

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

static void play_one(const play_req_t *r)
{
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
        err = esp_codec_dev_write(s_dev, (void *)r->buf, (int)r->bytes);
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
        ESP_LOGI(TAG, "sound played in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
        atomic_store(&s_ok, true);
    } else {
        ESP_LOGE(TAG, "sound failed: %d", err);
        atomic_store(&s_ok, false);
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        play_req_t r;
        while (q_pop(&r)) {
            play_one(&r);
        }
    }
}

/* A cue: bursts of a sine, 5 ms ramps at both ends against clicks, then the tail silence */
#define TONE_RAMP_MS 5
#define TONE_AMP     18000.0f

static size_t tone_burst(int16_t *out, unsigned freq, unsigned ms)
{
    size_t n = (size_t)HY_CHIME_RATE * ms / 1000, ramp = (size_t)HY_CHIME_RATE * TONE_RAMP_MS / 1000;
    for (size_t i = 0; i < n; i++) {
        float env = 1.0f;
        if (i < ramp) {
            env = (float)i / (float)ramp;
        } else if (i + ramp > n) {
            env = (float)(n - i) / (float)ramp;
        }
        out[i] = (int16_t)(TONE_AMP * env * sinf(6.2831853f * (float)freq * (float)i / (float)HY_CHIME_RATE));
    }
    return n;
}

/* The note list of each cue; the gap follows the note and is dropped after the last one */
typedef struct { uint16_t freq, ms, gap_ms; } note_t;
static const note_t N_GO[] = {{1319, 250, 0}};
static const note_t N_TAKE[] = {{880, 120, 120}, {880, 120, 0}};
static const note_t N_REDO[] = {{440, 400, 0}};
static const note_t N_DONE[] = {{659, 120, 40}, {880, 120, 40}, {1319, 120, 0}};
static const note_t N_FAIL[] = {{659, 250, 60}, {440, 250, 0}};
#define NOTES(a) {a, sizeof a / sizeof a[0]}
static const struct { const note_t *n; size_t count; } k_tones[HY_TONE_N] = {
    [HY_TONE_GO] = NOTES(N_GO),
    [HY_TONE_TAKE] = NOTES(N_TAKE),
    [HY_TONE_REDO] = NOTES(N_REDO),
    [HY_TONE_DONE] = NOTES(N_DONE),
    [HY_TONE_FAIL] = NOTES(N_FAIL),
};

static bool tones_render(void)
{
    size_t silence = (size_t)HY_CHIME_RATE * TAIL_SILENCE_MS / 1000;
    size_t head = (size_t)HY_CHIME_RATE * HEAD_SILENCE_MS / 1000;
    for (int t = 0; t < HY_TONE_N; t++) {
        size_t total = head + silence;
        for (size_t i = 0; i < k_tones[t].count; i++) {
            const note_t *n = &k_tones[t].n[i];
            total += (size_t)HY_CHIME_RATE * n->ms / 1000;
            if (i + 1 < k_tones[t].count) {
                total += (size_t)HY_CHIME_RATE * n->gap_ms / 1000;
            }
        }
        s_tone[t] = heap_caps_calloc(total, sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s_tone[t]) {
            return false;
        }
        size_t o = head;   /* calloc left the head silent */
        for (size_t i = 0; i < k_tones[t].count; i++) {
            const note_t *n = &k_tones[t].n[i];
            o += tone_burst(s_tone[t] + o, n->freq, n->ms);
            if (i + 1 < k_tones[t].count) {
                o += (size_t)HY_CHIME_RATE * n->gap_ms / 1000;
            }
        }
        s_tone_total[t] = total;
    }
    return true;
}

/* Above the LVGL task (6): building a screen holds that task for up to 170 ms, longer than the I2S ring plays,
 * so a lower priority starves the feeder and the chime stutters. The task sleeps in the I2S write between refills. */
#define AUDIO_TASK_PRIO 7

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
    size_t head = (size_t)HY_CHIME_RATE * HEAD_SILENCE_MS / 1000;   /* as the cues: the codec start eats the first ~140 ms */
    size_t alloc = head + n + silence;
    s_buf = heap_caps_malloc(alloc * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "no PSRAM for the chime (%u bytes)", (unsigned)(alloc * sizeof(int16_t)));
        goto fail;
    }
    memset(s_buf, 0, head * sizeof(int16_t));
    s_samples = hy_chime_render(s_buf + head, n, HY_CHIME_RATE);
    if (s_samples == 0) {
        goto fail;
    }
    memset(s_buf + head + s_samples, 0, silence * sizeof(int16_t));
    s_total = head + s_samples + silence;
    if (!tones_render()) {
        ESP_LOGW(TAG, "no PSRAM for the cabinet cues, they stay silent");
        for (int t = 0; t < HY_TONE_N; t++) {
            heap_caps_free(s_tone[t]);
            s_tone[t] = NULL;
        }
    }
    if (xTaskCreate(audio_task, "hy_audio", 4096, NULL, AUDIO_TASK_PRIO, &s_task) != pdPASS) {
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
    for (int t = 0; t < HY_TONE_N; t++) {
        heap_caps_free(s_tone[t]);
        s_tone[t] = NULL;
    }
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
    if (!q_push((play_req_t){s_buf, s_total * sizeof(int16_t), true}, true)) {
        ESP_LOGW(TAG, "chime dropped: a chime is already playing or waiting");
        return;
    }
    xTaskNotifyGive(s_task);
}

void hy_audio_tone(hy_tone_t t)
{
    if (!s_task || (unsigned)t >= HY_TONE_N || !s_tone[t]) {
        return;
    }
    if (!q_push((play_req_t){s_tone[t], s_tone_total[t] * sizeof(int16_t), false}, false)) {
        ESP_LOGW(TAG, "tone dropped: %d sounds are already waiting", HY_AUDIO_QUEUE_N);
        return;
    }
    xTaskNotifyGive(s_task);
}
