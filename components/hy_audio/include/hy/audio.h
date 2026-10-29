#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* bsp_audio_init(NULL), speaker codec, renders the chime into PSRAM, starts the audio task.
 * Nothing else depends on it: when it fails the device keeps working without sound. */
esp_err_t hy_audio_init(void);
/* false when init failed or the last play failed */
bool hy_audio_ok(void);
void hy_audio_set_volume(uint8_t pct);
/* Non-blocking; a request while one plays is dropped. No-op when init failed. */
void hy_audio_chime(void);

#ifdef __cplusplus
}
#endif
