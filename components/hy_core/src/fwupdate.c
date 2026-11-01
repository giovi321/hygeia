#include "hy/fwupdate.h"

#include <stdio.h>
#include <string.h>

/* ---- versions ---- */

/* Reads 1..5 decimal digits without a leading zero (except "0") up to 65535. Returns the new position or NULL. */
static const char *parse_num(const char *p, uint16_t *out)
{
    if (*p < '0' || *p > '9') return NULL;
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') return NULL;
    uint32_t v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint32_t)(*p - '0');
        if (++digits > 5 || v > 65535) return NULL;
        p++;
    }
    *out = (uint16_t)v;
    return p;
}

bool hy_fw_parse_release(const char *s, hy_semver_t *out)
{
    if (!s || s[0] != 'v') return false;
    hy_semver_t v;
    const char *p = parse_num(s + 1, &v.major);
    if (!p || *p++ != '.') return false;
    p = parse_num(p, &v.minor);
    if (!p || *p++ != '.') return false;
    p = parse_num(p, &v.patch);
    if (!p || *p != '\0') return false;
    if (out) *out = v;
    return true;
}

hy_fw_offer_t hy_fw_compare(const char *installed, const char *latest)
{
    hy_semver_t l, i;
    if (!hy_fw_parse_release(latest, &l)) return HY_FW_UP_TO_DATE;
    if (!hy_fw_parse_release(installed, &i)) return HY_FW_DEV;
    if (l.major != i.major) return l.major > i.major ? HY_FW_NEWER : HY_FW_UP_TO_DATE;
    if (l.minor != i.minor) return l.minor > i.minor ? HY_FW_NEWER : HY_FW_UP_TO_DATE;
    return l.patch > i.patch ? HY_FW_NEWER : HY_FW_UP_TO_DATE;
}

/* ---- URLs ---- */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Case-insensitive match of the whole of lit at s. Returns the position after it, or NULL. */
static const char *skip_ci(const char *s, const char *lit)
{
    while (*lit) {
        if (lower(*s) != lower(*lit)) return NULL;
        s++;
        lit++;
    }
    return s;
}

hy_err_t hy_fw_tag_from_location(const char *loc, const char *repo, char *tag, size_t len)
{
    if (!loc || !repo || !tag || len == 0 || repo[0] == '\0') return HY_E_INVALID;
    const char *p = loc;
    if (p[0] != '/') {
        p = skip_ci(p, "https://github.com");
        if (!p || p[0] != '/') return HY_E_INVALID;
    }
    p++; /* past the leading slash */
    p = skip_ci(p, repo);
    if (!p) return HY_E_INVALID;
    if (strcmp(p, "/releases") == 0) return HY_E_NOT_FOUND;
    if (strncmp(p, "/releases/tag/", 14) != 0) return HY_E_INVALID;
    p += 14;
    if (!hy_fw_parse_release(p, NULL)) return HY_E_INVALID;
    size_t n = strlen(p);
    if (n + 1 > len) return HY_E_INVALID;
    memcpy(tag, p, n + 1);
    return HY_OK;
}

bool hy_fw_redirect_ok(const char *url)
{
    if (!url) return false;
    const char *p = skip_ci(url, "https://");
    if (!p) return false;
    const char *end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#') {
        if (*end == '@') return false;
        end++;
    }
    const char *host_end = end;
    for (const char *c = p; c < end; c++) {
        if (*c == ':') { host_end = c; break; }
    }
    if (host_end < end) {
        if (end - host_end != 4 || memcmp(host_end + 1, "443", 3) != 0) return false;
    }
    size_t hl = (size_t)(host_end - p);
    if (hl == 0) return false;
    char host[64];
    if (hl >= sizeof host) return false;
    for (size_t i = 0; i < hl; i++) host[i] = lower(p[i]);
    host[hl] = '\0';
    if (strcmp(host, "github.com") == 0) return true;
    static const char suffix[] = ".githubusercontent.com";
    size_t sl = sizeof suffix - 1;
    return hl > sl && memcmp(host + hl - sl, suffix, sl) == 0;
}

/* ---- SHA256SUMS ---- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

hy_err_t hy_fw_sums_find(const char *text, size_t len, const char *name, uint8_t sha[32])
{
    if (!name || !sha || name[0] == '\0' || (len > 0 && !text)) return HY_E_INVALID;
    if (len > HY_FW_SUMS_MAX) return HY_E_INVALID;
    size_t nl = strlen(name);
    bool found = false;
    size_t pos = 0;
    while (pos < len) {
        size_t e = pos;
        while (e < len && text[e] != '\n') e++;
        size_t ll = e - pos;
        const char *line = text + pos;
        pos = e + 1;
        if (ll > 0 && line[ll - 1] == '\r') ll--;
        /* A line that ends in the name, as a whole word after a separator, is a line that names it. */
        if (ll < nl || memcmp(line + ll - nl, name, nl) != 0) continue;
        if (ll > nl && line[ll - nl - 1] != ' ' && line[ll - nl - 1] != '*') continue;
        if (ll != 64 + 2 + nl || line[64] != ' ' || (line[65] != ' ' && line[65] != '*')) return HY_E_INVALID;
        uint8_t h[32];
        for (size_t i = 0; i < 32; i++) {
            int hi = hexval(line[2 * i]), lo = hexval(line[2 * i + 1]);
            if (hi < 0 || lo < 0) return HY_E_INVALID;
            h[i] = (uint8_t)(hi << 4 | lo);
        }
        if (found) return HY_E_INVALID;
        found = true;
        memcpy(sha, h, 32);
    }
    return found ? HY_OK : HY_E_NOT_FOUND;
}

/* ---- status and errors ---- */

const char *hy_fw_state_name(hy_fw_state_t s)
{
    switch (s) {
    case HY_FWS_IDLE:        return "idle";
    case HY_FWS_CHECKING:    return "checking";
    case HY_FWS_UP_TO_DATE:  return "up_to_date";
    case HY_FWS_AVAILABLE:   return "available";
    case HY_FWS_DOWNLOADING: return "downloading";
    case HY_FWS_VERIFYING:   return "verifying";
    case HY_FWS_RESTARTING:  return "restarting";
    case HY_FWS_FAILED:      return "failed";
    }
    return "idle";
}

static const struct { const char *code; const char *message; } ERRS[HY_FWE_COUNT] = {
    [HY_FWE_NONE] = {NULL, NULL},
    [HY_FWE_NO_NETWORK] = {"no_network",
        "Not connected to a Wi-Fi network. Connect to a network with internet to check for updates"},
    [HY_FWE_NO_INTERNET] = {"no_internet", "Can't reach GitHub. Check that the Wi-Fi network has internet"},
    [HY_FWE_TLS] = {"tls", "The secure connection to GitHub failed"},
    [HY_FWE_NO_RELEASE] = {"no_release", "No release has been published yet"},
    [HY_FWE_BAD_RELEASE] = {"bad_release",
        "The latest release on GitHub is missing a file or has an unexpected name. Nothing was installed"},
    [HY_FWE_MISMATCH] = {"mismatch", "The downloaded file is not the expected hygeia version. Nothing was installed"},
    [HY_FWE_CHECKSUM] = {"checksum", "The download does not match its checksum. Nothing was installed"},
    [HY_FWE_CORRUPT] = {"corrupt", "The firmware file is damaged or incomplete. Nothing was installed"},
    [HY_FWE_FLASH] = {"flash", "Writing the firmware failed. Nothing was installed"},
    [HY_FWE_TIMEOUT] = {"timeout", "The download stalled. Nothing was installed"},
    [HY_FWE_BATTERY] = {"battery", "Battery below 30 %: connect USB before updating"},
    [HY_FWE_BUSY] = {"busy", "Another update, upload, export or import is running"},
    [HY_FWE_SELF_PENDING] = {"busy", "The device is still checking its own firmware, try again shortly"},
    [HY_FWE_MEMORY] = {"memory", "Not enough memory right now. Try again in a minute"},
    [HY_FWE_STALE] = {"stale", "Check for updates again before installing"},
};

const char *hy_fw_err_code(hy_fw_err_t e)
{
    return ((unsigned)e < HY_FWE_COUNT) ? ERRS[e].code : NULL;
}

const char *hy_fw_err_message(hy_fw_err_t e)
{
    return ((unsigned)e < HY_FWE_COUNT) ? ERRS[e].message : NULL;
}

/* ---- JSON encoders: fixed members, no allocation ---- */

typedef struct { char *buf; size_t len; size_t pos; bool over; } jw_t;

static void jw_raw(jw_t *w, const char *s, size_t n)
{
    if (w->over) return;
    if (w->pos + n + 1 > w->len) { w->over = true; return; }
    memcpy(w->buf + w->pos, s, n);
    w->pos += n;
    w->buf[w->pos] = '\0';
}

static void jw_str(jw_t *w, const char *s) { jw_raw(w, s, strlen(s)); }

static void jw_int(jw_t *w, long long v)
{
    char t[24];
    int n = snprintf(t, sizeof t, "%lld", v);
    jw_raw(w, t, (size_t)n);
}

/* A quoted, escaped JSON string. */
static void jw_quoted(jw_t *w, const char *s)
{
    jw_raw(w, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char t[2] = {'\\', (char)c};
            jw_raw(w, t, 2);
        } else if (c == '\n') jw_str(w, "\\n");
        else if (c == '\r') jw_str(w, "\\r");
        else if (c == '\t') jw_str(w, "\\t");
        else if (c < 0x20 || c == 0x7f) {
            char t[8];
            int n = snprintf(t, sizeof t, "\\u%04x", c);
            jw_raw(w, t, (size_t)n);
        } else {
            jw_raw(w, (const char *)&c, 1);
        }
    }
    jw_raw(w, "\"", 1);
}

static int jw_done(const jw_t *w) { return w->over ? -1 : (int)w->pos; }

int hy_fw_status_json(const hy_fw_status_t *st, int64_t now_s, char *buf, size_t len)
{
    if (!st || !buf || len == 0) return -1;
    jw_t w = {buf, len, 0, false};
    buf[0] = '\0';
    const char *code = hy_fw_err_code(st->err);
    const char *msg = hy_fw_err_message(st->err);
    jw_str(&w, "{\"state\":");
    jw_quoted(&w, hy_fw_state_name(st->state));
    jw_str(&w, ",\"installed\":");
    jw_quoted(&w, st->installed);
    jw_str(&w, ",\"latest\":");
    jw_quoted(&w, st->latest);
    jw_str(&w, st->installed_release ? ",\"installed_release\":true" : ",\"installed_release\":false");
    jw_str(&w, ",\"pct\":");
    jw_int(&w, st->pct);
    jw_str(&w, ",\"error\":");
    if (code) jw_quoted(&w, code); else jw_str(&w, "null");
    jw_str(&w, ",\"message\":");
    if (msg) jw_quoted(&w, msg); else jw_str(&w, "null");
    jw_str(&w, ",\"checked_ago_s\":");
    if (st->checked_s == 0) {
        jw_str(&w, "null");
    } else {
        int64_t ago = now_s - st->checked_s;
        jw_int(&w, ago < 0 ? 0 : ago);
    }
    jw_str(&w, "}");
    return jw_done(&w);
}

int hy_fw_event_json(const hy_fw_status_t *st, char *buf, size_t len)
{
    if (!st || !buf || len == 0) return -1;
    jw_t w = {buf, len, 0, false};
    buf[0] = '\0';
    jw_str(&w, "{\"ev\":\"update\",\"state\":");
    jw_quoted(&w, hy_fw_state_name(st->state));
    jw_str(&w, ",\"pct\":");
    jw_int(&w, st->pct);
    const char *code = hy_fw_err_code(st->err);
    if (st->state == HY_FWS_FAILED && code) {
        jw_str(&w, ",\"error\":");
        jw_quoted(&w, code);
    }
    jw_str(&w, "}");
    return jw_done(&w);
}
