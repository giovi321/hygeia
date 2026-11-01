/* One firmware image streamed to the inactive slot: head check, running SHA-256, esp_ota_* sequence. The caller owns
 * the transport (web upload body, download) and the answers; this owns what reaches flash.
 *
 * The first bytes must hold the image header, the first segment header and the application description of a hygeia
 * image built for this chip; anything else is refused before the slot is touched. */

#include "hy/fwimg.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

static const char *TAG = "web_ota";

struct hy_fwimg {
    const esp_partition_t *part;
    esp_ota_handle_t h;
    psa_hash_operation_t sha;
    size_t len, done;
    bool started;        /* esp_ota_begin ran */
    bool sha_open;
    bool has_ver;
    char expect_ver[32];
    esp_app_desc_t desc;
};

/* ---------------------------------------------------------------- heavy-job slot */

static _Atomic(const char *) s_heavy;

bool hy_heavy_claim(const char *who)
{
    const char *none = NULL;
    return atomic_compare_exchange_strong(&s_heavy, &none, who ? who : "unknown");
}

void hy_heavy_release(void)
{
    atomic_store(&s_heavy, NULL);
}

const char *hy_heavy_owner(void)
{
    return atomic_load(&s_heavy);
}

/* ---------------------------------------------------------------- image writer */

static bool running_unconfirmed(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

/* NULL when the first bytes are the start of a hygeia application image for this chip; else the reason (log only). */
static const char *check_head(const uint8_t *buf, size_t n, esp_app_desc_t *desc)
{
    if (n < HY_FWIMG_HEAD_LEN) {
        return "shorter than an image header";
    }
    esp_image_header_t h;
    memcpy(&h, buf, sizeof h);
    if (h.magic != ESP_IMAGE_HEADER_MAGIC) {
        return "no image magic";
    }
    if (h.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return "built for another chip";
    }
    if (h.segment_count == 0) {
        return "no segments";
    }
    memcpy(desc, buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof *desc);
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "no application description";
    }
    if (strncmp(desc->project_name, "hygeia", sizeof desc->project_name) != 0) {
        return "another project";
    }
    desc->version[sizeof desc->version - 1] = 0;
    return NULL;
}

static void release(hy_fwimg_t *w)
{
    if (!w) {
        return;
    }
    if (w->started) {
        esp_ota_abort(w->h);
    }
    if (w->sha_open) {
        psa_hash_abort(&w->sha);
    }
    heap_caps_free(w);
}

hy_fwimg_res_t hy_fwimg_begin(size_t len, const char *expect_ver, hy_fwimg_t **out)
{
    *out = NULL;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return HY_FWIMG_FLASH;
    }
    if (len > part->size) {
        return HY_FWIMG_TOO_LARGE;
    }
    if (running_unconfirmed()) {
        return HY_FWIMG_SELF_PENDING;
    }
    hy_fwimg_t *w = heap_caps_calloc(1, sizeof *w, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w) {
        return HY_FWIMG_NO_MEM;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        heap_caps_free(w);
        return HY_FWIMG_NO_MEM;
    }
    w->sha = psa_hash_operation_init();
    if (psa_hash_setup(&w->sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        psa_hash_abort(&w->sha);
        heap_caps_free(w);
        return HY_FWIMG_NO_MEM;
    }
    w->sha_open = true;
    w->part = part;
    w->len = len;
    if (expect_ver) {
        strncpy(w->expect_ver, expect_ver, sizeof w->expect_ver - 1);
        w->has_ver = true;
    }
    *out = w;
    return HY_FWIMG_OK;
}

hy_fwimg_res_t hy_fwimg_write(hy_fwimg_t *w, const uint8_t *buf, size_t n)
{
    hy_fwimg_res_t res;
    esp_err_t err;
    if (!w) {
        return HY_FWIMG_CORRUPT;
    }
    if (n > w->len - w->done) {
        res = HY_FWIMG_CORRUPT;
        goto fail;
    }
    if (!w->started) {
        const char *why = check_head(buf, n, &w->desc);
        if (why) {
            ESP_LOGW(TAG, "upload refused: %s", why);
            res = HY_FWIMG_NOT_HYGEIA;
            goto fail;
        }
        if (w->has_ver && strncmp(w->desc.version, w->expect_ver, sizeof w->desc.version) != 0) {
            ESP_LOGW(TAG, "upload refused: firmware %s, expected %s", w->desc.version, w->expect_ver);
            res = HY_FWIMG_VERSION;
            goto fail;
        }
        ESP_LOGI(TAG, "upload of firmware %s (%u bytes) to %s", w->desc.version, (unsigned)w->len, w->part->label);
        err = esp_ota_begin(w->part, OTA_WITH_SEQUENTIAL_WRITES, &w->h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
            res = err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ? HY_FWIMG_SELF_PENDING : HY_FWIMG_FLASH;
            goto fail;
        }
        w->started = true;
    }
    err = esp_ota_write(w->h, buf, n);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write at %u: %s", (unsigned)w->done, esp_err_to_name(err));
        res = HY_FWIMG_FLASH;
        goto fail;
    }
    if (psa_hash_update(&w->sha, buf, n) != PSA_SUCCESS) {
        res = HY_FWIMG_NO_MEM;
        goto fail;
    }
    w->done += n;
    return HY_FWIMG_OK;
fail:
    release(w);
    return res;
}

hy_fwimg_res_t hy_fwimg_finish(hy_fwimg_t *w, const uint8_t expect_sha[32], char version[32])
{
    hy_fwimg_res_t res = HY_FWIMG_OK;
    esp_err_t err;
    uint8_t sum[32];
    size_t olen = 0;
    if (!w) {
        return HY_FWIMG_CORRUPT;
    }
    if (!w->started || w->done != w->len) {
        res = HY_FWIMG_CORRUPT;
        goto out;
    }
    w->sha_open = false;   /* finish consumes the operation either way */
    if (psa_hash_finish(&w->sha, sum, sizeof sum, &olen) != PSA_SUCCESS || olen != sizeof sum) {
        psa_hash_abort(&w->sha);
        res = HY_FWIMG_NO_MEM;
        goto out;
    }
    if (expect_sha && memcmp(sum, expect_sha, sizeof sum) != 0) {
        ESP_LOGW(TAG, "image checksum differs from the expected one");
        res = HY_FWIMG_CHECKSUM;
        goto out;
    }
    err = esp_ota_end(w->h);   /* frees the handle whatever the result */
    w->started = false;
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        ESP_LOGW(TAG, "image check failed: %s", esp_err_to_name(err));
        res = HY_FWIMG_CORRUPT;
        goto out;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        res = HY_FWIMG_FLASH;
        goto out;
    }
    err = esp_ota_set_boot_partition(w->part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        res = HY_FWIMG_FLASH;
        goto out;
    }
    if (version) {
        memcpy(version, w->desc.version, 32);
        version[31] = 0;
    }
out:
    release(w);
    return res;
}

void hy_fwimg_abort(hy_fwimg_t *w)
{
    release(w);
}
