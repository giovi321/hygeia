#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hy/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HY_NET_OFF = 0, HY_NET_CLIENT, HY_NET_AP } hy_net_mode_t;

#define HY_NET_MAX_KNOWN 5
typedef struct {
    char ssid[33];
    uint32_t last_ok; /* success sequence number, 0 = never */
} hy_known_net_t;

/* 1..32 bytes, no control bytes; else HY_E_INVALID. */
hy_err_t hy_net_check_ssid(const char *ssid);
/* Client: "" (open) or 8..63 printable ASCII. AP: 8..63 printable ASCII. */
hy_err_t hy_net_check_pass(const char *pass, bool ap);
/* Indexes into k in try order: last_ok descending, never-succeeded ones last in list order. Returns n. */
size_t hy_net_try_order(const hy_known_net_t *k, size_t n, uint8_t *order);
/* Seconds before retry number `attempt` (1-based) of a full pass over the list: 5, 10, 20, 40, then 60. */
uint32_t hy_net_backoff_s(uint32_t attempt);
#define HY_NET_HINT_S 120 /* failing this long shows the access point hint */

/* "hygeia-" + upper-case hex of mac[4], mac[5]. */
void hy_net_ap_ssid(const uint8_t mac[6], char out[12]);
/* 10 characters from "23456789abcdefghjkmnpqrstuvwxyz" (31: no 0 1 i l o), char i = rnd[i] % 31. */
void hy_net_ap_pass(const uint8_t rnd[10], char out[11]);
/* "WIFI:T:WPA;S:<ssid>;P:<pass>;;" with \ ; , : " backslash-escaped; "T:nopass" and no P when pass is "".
 * Returns the length, -1 when len is too small. */
int hy_net_wifi_qr(const char *ssid, const char *pass, char *out, size_t len);

/* Battery radio rule. now and last_activity are monotonic seconds. */
typedef struct {
    hy_net_mode_t saved;
    bool usb;
    bool keep_on_battery;
    bool started_on_battery;
    int64_t last_activity;
    int64_t now;
    uint16_t idle_s;
} hy_radio_in_t;
#define HY_NET_BATT_IDLE_S 600
/* usb or keep_on_battery: saved. On battery: saved when started_on_battery and now - last_activity < idle_s,
 * else OFF. */
hy_net_mode_t hy_net_radio(const hy_radio_in_t *in);
/* USB unplugged while the radio runs: started_on_battery = true, last_activity = now.
 * Plugged: started_on_battery = false. */
void hy_net_on_usb_change(hy_radio_in_t *in, bool usb_now);

#ifdef __cplusplus
}
#endif
