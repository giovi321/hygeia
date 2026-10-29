#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HY_CHIME_RATE 22050

/* Samples of one chime at rate: 30 ms lead silence, three tones of 180 ms (880, 1109, 1319 Hz) with 70 ms
   gaps, 30 ms tail silence. Each tone: 8 ms linear attack, linear decay to 0 over its last 120 ms, peak 16384. */
size_t hy_chime_len(uint32_t rate);
size_t hy_chime_render(int16_t *out, size_t max, uint32_t rate);   /* returns samples written, 0 if max too small */

#ifdef __cplusplus
}
#endif
