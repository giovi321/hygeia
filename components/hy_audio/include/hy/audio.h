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
/* Non-blocking; starts with 200 ms of silence (the codec and amplifier start). Queues behind the cues and is dropped
 * only while another chime plays or waits. No-op when init failed. */
void hy_audio_chime(void);
/* Short cues of the Cabinet open teach and test views, as note lists: GO one high tone (1319 Hz, 250 ms); TAKE two
 * low tones (880 Hz, 120 ms each, 120 ms gap); REDO one long low tone (440 Hz, 400 ms); DONE rising 659, 880 and
 * 1319 Hz (120 ms each, 40 ms gaps); FAIL falling 659 then 440 Hz (250 ms each, 60 ms gap). Each has 200 ms of lead silence. Played at the set
 * volume whatever the Sound and Do not disturb switches say. Non-blocking; up to 4 requests wait, so two cues in a
 * row both play, and a request beyond that is dropped; no-op when init failed. */
typedef enum { HY_TONE_GO = 0, HY_TONE_TAKE, HY_TONE_REDO, HY_TONE_DONE, HY_TONE_FAIL, HY_TONE_N } hy_tone_t;
void hy_audio_tone(hy_tone_t t);

#ifdef __cplusplus
}
#endif
