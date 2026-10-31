#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hy/netcfg.h"

#define NET_HOST_DEFAULT "hygeia"

typedef struct {
    char ssid[33];
    char pass[64];
    uint32_t ok;   /* last_ok: success sequence number, 0 = never */
} net_entry_t;

/* The persistent configuration, NVS namespace "wifi": "mode" u8, "n" u8, "s0".."s4" str, "p0".."p4" str,
 * "ok0".."ok4" u32, "okseq" u32, "ap_pass" str, "host" str. The only copy of the credentials. */
typedef struct {
    hy_net_mode_t mode;
    uint8_t n;
    net_entry_t e[HY_NET_MAX_KNOWN];
    uint32_t okseq;
    char ap_pass[64];
    char host[33];
} net_cfg_t;

/* Loads the configuration; invalid entries are dropped. An absent or invalid access point password is created from
 * the hardware random generator and saved. On an NVS error the defaults stand (mode off, no networks). */
esp_err_t net_nvs_load(net_cfg_t *c);
esp_err_t net_nvs_save_mode(hy_net_mode_t m);
/* "n" and every entry; keys of removed entries are erased. */
esp_err_t net_nvs_save_list(const net_cfg_t *c);
esp_err_t net_nvs_save_ok(uint8_t idx, uint32_t ok, uint32_t okseq);
esp_err_t net_nvs_save_str(const char *key, const char *val);
bool      net_host_valid(const char *host);

/* Captive DNS: a task on UDP port 53 answering every A query with ip_be (hy_dns_answer). Net task only. */
esp_err_t net_captive_start(uint32_t ip_be);
void      net_captive_stop(void);
