#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the ES8311 speaker device on a TX-only I2S channel, renders the chime into PSRAM, starts the audio task.
 * Nothing else depends on it: when it fails the device keeps working without sound. */
esp_err_t hy_audio_init(void);
/* true from boot until hy_audio_init has returned (either result): a chime requested now cannot be played yet,
 * which is not a speaker failure */
bool hy_audio_pending(void);
/* false when init failed or the last play failed */
bool hy_audio_ok(void);
void hy_audio_set_volume(uint8_t pct);
/* Non-blocking; a request while one plays is dropped. No-op when init failed. */
void hy_audio_chime(void);
/* Short cues of the Cabinet open teach and test views: GO is one high tone (1319 Hz, 120 ms), TAKE two low ones
 * (880 Hz, 80 ms each). Played at the set volume whatever the Sound and Do not disturb switches say. Non-blocking;
 * dropped while another sound plays; no-op when init failed. */
typedef enum { HY_TONE_GO = 0, HY_TONE_TAKE, HY_TONE_N } hy_tone_t;
void hy_audio_tone(hy_tone_t t);

#ifdef __cplusplus
}
#endif
