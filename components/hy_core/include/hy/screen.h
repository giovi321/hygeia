#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Screen state machine: ON, dimmed to the idle clock, or off. Pure: the caller passes a millisecond clock. */
typedef enum { HY_SCR_ON = 0, HY_SCR_DIM, HY_SCR_OFF } hy_scr_t;
/* TOUCH, USER_KEY, DUE and USB_IN turn the screen on. CHIME turns it on only on USB power: on battery a repeat
   chime does not wake the screen and changes nothing. PWR_SHORT toggles on/off. USB_OUT starts the battery off
   timer from the unplug. TICK applies the dim and off timers. */
typedef enum { HY_SE_TOUCH = 0, HY_SE_USER_KEY, HY_SE_PWR_SHORT, HY_SE_DUE, HY_SE_CHIME,
               HY_SE_USB_IN, HY_SE_USB_OUT, HY_SE_TICK } hy_scr_ev_t;
typedef struct { uint16_t dim_s; uint16_t off_bat_s; uint32_t off_usb_s; } hy_scr_cfg_t;  /* from settings */
typedef struct { hy_scr_t state; uint32_t activity_ms; bool usb; bool hold; } hy_screen_t;

void     hy_screen_init(hy_screen_t *s, bool usb, uint32_t now_ms);           /* ON */
/* hold: the power-off confirmation or the critical countdown shows, or a due card shows on USB (no dim, no off;
 * set by the UI, R1) */
void     hy_screen_set_hold(hy_screen_t *s, bool hold, uint32_t now_ms);
hy_scr_t hy_screen_event(hy_screen_t *s, hy_scr_ev_t ev, uint32_t now_ms, const hy_scr_cfg_t *cfg);
bool     hy_screen_may_sleep(const hy_screen_t *s);                            /* OFF and !usb */

#ifdef __cplusplus
}
#endif
