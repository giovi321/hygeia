#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hy/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint16_t major, minor, patch; } hy_semver_t;

/* A stable release tag: "v" then three decimal numbers 0..65535 separated by dots, no leading zeros (except "0"),
   nothing before or after. "v1.2.3" true; "1.2.3", "v1.2", "v1.2.3-rc1", "v01.2.3", "v1.2.3 ", "" false. */
bool hy_fw_parse_release(const char *s, hy_semver_t *out);

typedef enum { HY_FW_UP_TO_DATE = 0, HY_FW_NEWER, HY_FW_DEV } hy_fw_offer_t;
/* latest must parse as a release (else HY_FW_UP_TO_DATE: nothing to offer). installed not a release tag
   (dev-<sha>, git describe output, a prerelease, empty): HY_FW_DEV. Both releases: HY_FW_NEWER when latest > installed,
   else HY_FW_UP_TO_DATE (equal or installed newer). */
hy_fw_offer_t hy_fw_compare(const char *installed, const char *latest);

/* The Location of GET https://github.com/<repo>/releases/latest. Accepts
   "https://github.com/<repo>/releases/tag/<tag>" and the same path without scheme and host ("/<repo>/releases/tag/<tag>"),
   repo compared case-insensitively, tag must pass hy_fw_parse_release. Writes the tag. HY_E_NOT_FOUND when the
   Location is "https://github.com/<repo>/releases" (no release published), HY_E_INVALID otherwise. */
hy_err_t hy_fw_tag_from_location(const char *loc, const char *repo, char *tag, size_t len);

/* A redirect target the download may follow: scheme "https", host "github.com" or ending in ".githubusercontent.com"
   (case-insensitive), no userinfo, port absent or 443. */
bool hy_fw_redirect_ok(const char *url);

/* SHA256SUMS text (not NUL-terminated, len <= HY_FW_SUMS_MAX): finds the line whose file name equals name exactly and
   writes its 32-byte hash. A line is 64 hex digits (either case), then " " and either " " or "*", then the name, then
   "\n", "\r\n" or the end. Other lines are ignored. HY_E_NOT_FOUND when no line names it, HY_E_INVALID when len is
   over the limit, or when the matching line is malformed or appears twice. */
#define HY_FW_SUMS_MAX 1024
hy_err_t hy_fw_sums_find(const char *text, size_t len, const char *name, uint8_t sha[32]);

typedef enum { HY_FWS_IDLE = 0, HY_FWS_CHECKING, HY_FWS_UP_TO_DATE, HY_FWS_AVAILABLE, HY_FWS_DOWNLOADING,
               HY_FWS_VERIFYING, HY_FWS_RESTARTING, HY_FWS_FAILED } hy_fw_state_t;
typedef enum { HY_FWE_NONE = 0, HY_FWE_NO_NETWORK, HY_FWE_NO_INTERNET, HY_FWE_TLS, HY_FWE_NO_RELEASE,
               HY_FWE_BAD_RELEASE, HY_FWE_MISMATCH, HY_FWE_CHECKSUM, HY_FWE_CORRUPT, HY_FWE_FLASH, HY_FWE_TIMEOUT,
               HY_FWE_BATTERY, HY_FWE_BUSY, HY_FWE_SELF_PENDING, HY_FWE_MEMORY, HY_FWE_STALE, HY_FWE_COUNT } hy_fw_err_t;
typedef struct { hy_fw_state_t state; hy_fw_err_t err; char installed[32]; char latest[32]; bool installed_release;
                 uint8_t pct; int64_t checked_s; /* monotonic seconds of the last finished check, 0 = never */ } hy_fw_status_t;

const char *hy_fw_state_name(hy_fw_state_t s);  /* "idle" "checking" "up_to_date" "available" "downloading"
                                                   "verifying" "restarting" "failed" */
const char *hy_fw_err_code(hy_fw_err_t e);      /* API error codes; NULL for HY_FWE_NONE */
const char *hy_fw_err_message(hy_fw_err_t e);   /* API message; NULL for HY_FWE_NONE */
/* {"state":"available","installed":"dev-a88c56d","latest":"v0.5.0","installed_release":false,"pct":0,
    "error":null,"message":null,"checked_ago_s":12}; latest "" and checked_ago_s null when never checked.
   Strings JSON-escaped. Returns the length, -1 when len is too small. now_s: monotonic seconds. */
int hy_fw_status_json(const hy_fw_status_t *st, int64_t now_s, char *buf, size_t len);
/* Live event: {"ev":"update","state":"downloading","pct":42}, with "error":"<code>" when failed. Fits 192 bytes. */
int hy_fw_event_json(const hy_fw_status_t *st, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
