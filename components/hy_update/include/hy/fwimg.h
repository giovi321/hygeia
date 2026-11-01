/* One firmware image streamed to the inactive slot, and the one-heavy-job slot. Used by the web upload and the GitHub
 * update. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hy_fwimg hy_fwimg_t;    /* opaque, PSRAM */
typedef enum { HY_FWIMG_OK = 0, HY_FWIMG_TOO_LARGE, HY_FWIMG_NOT_HYGEIA, HY_FWIMG_VERSION, HY_FWIMG_SELF_PENDING,
               HY_FWIMG_FLASH, HY_FWIMG_CHECKSUM, HY_FWIMG_CORRUPT, HY_FWIMG_NO_MEM } hy_fwimg_res_t;
#define HY_FWIMG_HEAD_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

/* len: total image bytes; TOO_LARGE when over the next slot. expect_ver: NULL, or the exact app version the image
   must carry (VERSION otherwise). Nothing touches flash yet. SELF_PENDING when the running image is pending verify. */
hy_fwimg_res_t hy_fwimg_begin(size_t len, const char *expect_ver, hy_fwimg_t **out);
/* Bytes in order. The first call must carry min(len, HY_FWIMG_HEAD_LEN) bytes or more: the head is checked
   (magic, chip id, segment count, app description magic, project "hygeia", version) and only then esp_ota_begin
   (OTA_WITH_SEQUENTIAL_WRITES) runs. Every byte also goes into a running SHA-256 (PSA). On any result other than OK
   the writer is aborted and freed. */
hy_fwimg_res_t hy_fwimg_write(hy_fwimg_t *w, const uint8_t *buf, size_t n);
/* After exactly len bytes: when expect_sha is not NULL compares the SHA-256 (CHECKSUM, aborted); then esp_ota_end
   (CORRUPT on ESP_ERR_OTA_VALIDATE_FAILED, FLASH otherwise); then esp_ota_set_boot_partition (FLASH). Frees w in every
   case. version (may be NULL) receives the image's version. */
hy_fwimg_res_t hy_fwimg_finish(hy_fwimg_t *w, const uint8_t expect_sha[32], char version[32]);
void           hy_fwimg_abort(hy_fwimg_t *w);   /* NULL allowed */

/* One heavy job at a time across the device: web upload, export, import, GitHub update. Any task. */
bool        hy_heavy_claim(const char *who);  /* false when held; who is a static string for logs */
void        hy_heavy_release(void);
const char *hy_heavy_owner(void);             /* NULL when free */

#ifdef __cplusplus
}
#endif
