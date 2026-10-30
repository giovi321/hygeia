#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { bool sound; bool dnd; uint8_t volume; uint8_t brightness; uint16_t snooze_min;
                 uint16_t dim_s; uint16_t off_bat_s; uint32_t off_usb_s; } hy_settings_t;

void hy_settings_defaults(hy_settings_t *s);   /* sound true, dnd false, volume 60, brightness 80, snooze 15, dim_s 30, off_bat_s 120, off_usb_s 0 (never) */
void hy_settings_clamp(hy_settings_t *s);      /* volume 0..100, brightness 10..100, snooze 1..120, dim_s 10..600,
                                                  off_bat_s 30..3600 and >= dim_s + 10, off_usb_s 0 or 60..86400 */

#ifdef __cplusplus
}
#endif
