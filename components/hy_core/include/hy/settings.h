#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { bool sound; bool dnd; uint8_t volume; uint8_t brightness; uint16_t snooze_min;
                 uint16_t dim_s; uint16_t off_bat_s; uint32_t off_usb_s;
                 bool rot_lock; uint8_t rot;
                 uint16_t quiet_from, quiet_to;   /* minutes after local midnight; equal = no quiet hours */
                 bool wifi_keep_batt;             /* keep Wi-Fi on while on battery */
                 bool cab_on;                     /* cabinet open reaction enabled */
                 uint8_t cab_mode;                /* HY_CAB_ANY or HY_CAB_TAUGHT */
                 bool cab_chime, cab_light;       /* reaction: chime and/or light the screen */
                 uint8_t cab_sens;                /* motion sensitivity 1..5 */
                 uint8_t cab_window_min;          /* minutes before a dose is due, 0..120 in steps of 5, 0 = overdue only */
                 bool wake_single_tap;            /* a single tap wakes the dimmed or dark screen; off: a double tap does */ } hy_settings_t;

#define HY_CAB_ANY 0
#define HY_CAB_TAUGHT 1
#define HY_CAB_WINDOW_MAX_MIN 120
#define HY_CAB_WINDOW_STEP_MIN 5

void hy_settings_defaults(hy_settings_t *s);   /* sound true, dnd false, volume 60, brightness 50, snooze 15, dim_s 30, off_bat_s 120, off_usb_s 0 (never), rot_lock false, rot 0, quiet 0..0 (off), wifi_keep_batt false, cab_on false, cab_mode any, cab_chime true, cab_light true, cab_sens 3, cab_window_min 15, wake_single_tap false */
void hy_settings_clamp(hy_settings_t *s);      /* volume 0..100, brightness 10..100, snooze 1..120, dim_s 10..600,
                                                  off_bat_s 30..3600 and >= dim_s + 10, off_usb_s 0 or 60..86400, rot 0..3 (else 0), quiet_from and quiet_to above 1439 become 0,
                                                  cab_mode above 1 becomes 0, cab_sens outside 1..5 becomes 3, cab_window_min above 120 becomes 120, else rounded down to a multiple of 5 */

#ifdef __cplusplus
}
#endif
