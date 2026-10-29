#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { bool sound; bool dnd; uint8_t volume; uint8_t brightness; uint16_t snooze_min; } hy_settings_t;

void hy_settings_defaults(hy_settings_t *s);   /* sound true, dnd false, volume 60, brightness 80, snooze 15 */
void hy_settings_clamp(hy_settings_t *s);      /* volume 0..100, brightness 10..100, snooze 1..120 */

#ifdef __cplusplus
}
#endif
