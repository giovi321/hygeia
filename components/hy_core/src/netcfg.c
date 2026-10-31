#include "hy/netcfg.h"

#include <string.h>

hy_err_t hy_net_check_ssid(const char *ssid)
{
    if (!ssid) return HY_E_INVALID;
    size_t n = strlen(ssid);
    if (n < 1 || n > 32) return HY_E_INVALID;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)ssid[i];
        if (c < 0x20 || c == 0x7F) return HY_E_INVALID;
    }
    return HY_OK;
}

hy_err_t hy_net_check_pass(const char *pass, bool ap)
{
    if (!pass) return HY_E_INVALID;
    size_t n = strlen(pass);
    if (n == 0) return ap ? HY_E_INVALID : HY_OK;
    if (n < 8 || n > 63) return HY_E_INVALID;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)pass[i];
        if (c < 0x20 || c > 0x7E) return HY_E_INVALID;
    }
    return HY_OK;
}

size_t hy_net_try_order(const hy_known_net_t *k, size_t n, uint8_t *order)
{
    /* Stable insertion sort: last_ok descending; never-succeeded (0) entries sort last in list order. */
    for (size_t i = 0; i < n; i++) {
        size_t j = i;
        while (j > 0 && k[order[j - 1]].last_ok < k[i].last_ok) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = (uint8_t)i;
    }
    return n;
}

uint32_t hy_net_backoff_s(uint32_t attempt)
{
    switch (attempt) {
    case 0:
    case 1: return 5;
    case 2: return 10;
    case 3: return 20;
    case 4: return 40;
    default: return 60;
    }
}

void hy_net_ap_ssid(const uint8_t mac[6], char out[12])
{
    static const char hex[] = "0123456789ABCDEF";
    memcpy(out, "hygeia-", 7);
    out[7] = hex[mac[4] >> 4];
    out[8] = hex[mac[4] & 0x0F];
    out[9] = hex[mac[5] >> 4];
    out[10] = hex[mac[5] & 0x0F];
    out[11] = '\0';
}

void hy_net_ap_pass(const uint8_t rnd[10], char out[11])
{
    static const char alpha[] = "23456789abcdefghjkmnpqrstuvwxyz";
    for (int i = 0; i < 10; i++) out[i] = alpha[rnd[i] % 31];
    out[10] = '\0';
}

/* Appends s to out with the WIFI: escapes applied. pos counts the characters needed, whether or not they fit. */
static void qr_put(char *out, size_t len, size_t *pos, const char *s, bool escape)
{
    for (; *s; s++) {
        if (escape && (*s == '\\' || *s == ';' || *s == ',' || *s == ':' || *s == '"')) {
            if (*pos + 1 < len) out[*pos] = '\\';
            (*pos)++;
        }
        if (*pos + 1 < len) out[*pos] = *s;
        (*pos)++;
    }
}

int hy_net_wifi_qr(const char *ssid, const char *pass, char *out, size_t len)
{
    size_t pos = 0;
    bool open = !pass || pass[0] == '\0';
    qr_put(out, len, &pos, open ? "WIFI:T:nopass;S:" : "WIFI:T:WPA;S:", false);
    qr_put(out, len, &pos, ssid ? ssid : "", true);
    qr_put(out, len, &pos, ";", false);
    if (!open) {
        qr_put(out, len, &pos, "P:", false);
        qr_put(out, len, &pos, pass, true);
        qr_put(out, len, &pos, ";", false);
    }
    qr_put(out, len, &pos, ";", false);
    if (len == 0 || pos + 1 > len) {
        if (len > 0) out[0] = '\0';
        return -1;
    }
    out[pos] = '\0';
    return (int)pos;
}

hy_net_mode_t hy_net_radio(const hy_radio_in_t *in)
{
    if (in->usb || in->keep_on_battery) return in->saved;
    if (in->started_on_battery && in->now - in->last_activity < (int64_t)in->idle_s) return in->saved;
    return HY_NET_OFF;
}

void hy_net_on_usb_change(hy_radio_in_t *in, bool usb_now)
{
    if (usb_now) {
        in->started_on_battery = false;
    } else {
        in->started_on_battery = true;
        in->last_activity = in->now;
    }
}
