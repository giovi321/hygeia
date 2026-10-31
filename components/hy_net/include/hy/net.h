#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hy/netcfg.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PAUSED: a mode is saved but the battery rule keeps the radio off. FAILING: the client has been connecting for
 * HY_NET_HINT_S or more (the access point hint; the mode stays client), or the radio failed to start (retried every
 * 30 s).
 * Battery rule: on USB, or with keep on battery, the saved mode runs. On battery the radio runs only after
 * hy_net_set_mode (or after USB was unplugged while it ran), until HY_NET_BATT_IDLE_S after the last
 * hy_net_activity. Once that idle time has turned it off, activity does not bring it back: only hy_net_set_mode or
 * USB does. */
typedef enum { HY_NS_OFF = 0, HY_NS_PAUSED, HY_NS_CONNECTING, HY_NS_CONNECTED, HY_NS_FAILING, HY_NS_AP } hy_net_state_t;

/* ssid: the network connected to (client), "" otherwise. rssi: dBm while connected, else 0. ip: network order, the
 * client address while connected, 192.168.4.1 in access point mode, else 0. ap_clients: stations joined to the
 * access point. ap_ssid: the access point name. host: the host name (mDNS <host>.local, DHCP host name). */
typedef struct {
    hy_net_mode_t saved;
    hy_net_state_t state;
    char ssid[33];
    int8_t rssi;
    uint32_t ip;
    uint8_t ap_clients;
    char ap_ssid[12];
    char host[33];
} hy_net_status_t;

#define HY_NET_MAX_SUBS 4

/* After nvs_flash_init: loads NVS namespace "wifi", creates the access point password on first boot, starts
 * esp_netif and the default event loop, creates the station and access point netifs. The radio stays off. */
esp_err_t hy_net_init(void);
/* After hy_power_start: starts the net task, which applies hy_net_radio from then on. */
void      hy_net_start(void);
/* Any task: a copy under the mutex. */
void      hy_net_status(hy_net_status_t *out);
/* Any task; persisted. ssid (client only) is tried first and must be a known network (ESP_ERR_NOT_FOUND); NULL keeps
 * the try order. On battery the call counts as "started on battery": the radio runs until HY_NET_BATT_IDLE_S after
 * the last activity. ESP_ERR_INVALID_ARG for an unknown mode. */
esp_err_t hy_net_set_mode(hy_net_mode_t m, const char *ssid);
/* The known networks in list order, at most HY_NET_MAX_KNOWN; returns the count. */
size_t    hy_net_known(hy_known_net_t *out);
/* The saved password of a known network (backup export with secrets only). ESP_ERR_NOT_FOUND when unknown. */
esp_err_t hy_net_pass_get(const char *ssid, char out[64]);
/* Adds a network or replaces the password of the same SSID. ESP_ERR_INVALID_ARG when hy_net_check_ssid or
 * hy_net_check_pass(pass, false) fails, ESP_ERR_NO_MEM for a sixth network. */
esp_err_t hy_net_add(const char *ssid, const char *pass);
/* ESP_ERR_NOT_FOUND when the SSID is not known. */
esp_err_t hy_net_forget(const char *ssid);
/* The access point password (10 characters unless changed). */
esp_err_t hy_net_ap_pass_get(char out[64]);
/* hy_net_check_pass(pass, true), else ESP_ERR_INVALID_ARG. A running access point restarts with it. */
esp_err_t hy_net_ap_pass_set(const char *pass);
/* 1..32 of a-z 0-9 -, not leading '-' (else ESP_ERR_INVALID_ARG); default "hygeia". Applies to mDNS at once and to
 * DHCP at the next connection. */
esp_err_t hy_net_host_set(const char *host);
/* A web request or a touch: restarts the battery idle timer. Any task, no blocking. */
void      hy_net_activity(void);
/* Settings wifi_keep_batt: keep the radio on while on battery. Any task. */
void      hy_net_set_keep_batt(bool keep);
/* Called in the net task on every status change (state, saved mode, ssid, ip, access point clients, host; not on
 * rssi alone). At most HY_NET_MAX_SUBS; returns the slot, -1 when full. The callback must not block. */
int       hy_net_subscribe(void (*cb)(const hy_net_status_t *st, void *ctx), void *ctx);

#if CONFIG_HY_DEBUG_CONSOLE
/* "wifi" command: "" status, "mode off|client|ap [ssid]", "add <ssid> [pass]", "forget <ssid>", "appass", "batt" */
void hy_net_debug(const char *args);
#endif

#ifdef __cplusplus
}
#endif
