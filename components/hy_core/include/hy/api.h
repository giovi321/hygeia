#ifndef HY_API_H
#define HY_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hy/types.h"

#define HY_API_VERSION 1

typedef struct {
    hy_err_t err;
    int http;
    const char *code;
    char field[24];
} hy_api_err_t;

/* HY_E_INVALID 400 "invalid"; HY_E_CODE 422 "code"; HY_E_NAME 422 "name"; HY_E_DOSE 422 "dose";
 * HY_E_TIMES 422 "times"; HY_E_INTERVAL 422 "interval"; HY_E_CODE_DUP 409 "code_dup";
 * HY_E_NOT_FOUND 404 "not_found"; HY_E_CLOCK 409 "clock_invalid"; HY_E_UNDO_EXPIRED 410 "undo_expired";
 * HY_E_FULL 409 "full"; HY_E_IO 503 "storage"; HY_W_TOO_SOON 409 "too_soon"; anything else 500 "internal".
 * field is the medicine field for the 422 codes and empty otherwise. */
void hy_api_err_from(hy_err_t e, hy_api_err_t *out);
/* {"error":"<code>","field":"<field>" (omitted when empty),"message":"<message>"}. Length, or -1 when it does
 * not fit. */
int hy_api_err_json(const hy_api_err_t *e, const char *message, char *buf, size_t len);

/* ---- medicines ---- */

/* FNV-1a 32 over the codec encoding with pill_count.remaining set to 0. 0 for a record the codec refuses. */
uint32_t hy_api_med_etag(const hy_med_t *m);
/* malloc'd: the codec object plus "etag":"%08x". NULL on failure. */
char *hy_api_med_json(const hy_med_t *m);
/* {"api":1,"meds":[...]}, archived included, each with its etag. malloc'd. */
char *hy_api_meds_json(const hy_med_t *m, size_t n);
/* New (base NULL): hy_med_defaults, then the body's fields. Edit: *base, then the fields present in the body.
 * Ignores "id", "etag", "archived", "order" and unknown fields; null counts as absent. Strings must be valid
 * UTF-8 (else HY_E_INVALID, field "name", "dose" or "code"); too long gives HY_E_NAME, HY_E_DOSE or HY_E_CODE
 * with the field, never truncated. Rules that need the whole list (code format, uniqueness) are left to
 * hy_med_validate. On failure *out is untouched. */
hy_err_t hy_api_med_from_json(const char *body, const hy_med_t *base, hy_med_t *out, hy_api_err_t *err);
/* {"ids":[...]}: every active medicine of meds[0..n) exactly once, no archived id, no unknown id; else
 * HY_E_INVALID. ids needs room for n entries; *n_ids receives the count. */
hy_err_t hy_api_order_from_json(const char *body, const hy_med_t *meds, size_t n, uint32_t *ids, size_t *n_ids,
                                hy_api_err_t *err);

/* ---- log ---- */

typedef struct {
    int64_t from, to;
    uint32_t med;
    uint16_t limit;
    int64_t after_ts;
    uint32_t after_id;
} hy_api_logq_t;
/* Query string "from=..&to=..&med=..&limit=..&after=<ts>.<id>" (a leading "?" is accepted). Defaults:
 * to = now + 1, from = to - 30 days, limit 200 (1..500), med 0, no cursor (after_ts 0, after_id 0). from >= to
 * or a non-number: HY_E_INVALID with the field. */
hy_err_t hy_api_logq_parse(const char *query, int64_t now, hy_api_logq_t *q, hy_api_err_t *err);
/* Removes the entries at or before the cursor (ts < after_ts, or ts == after_ts and id <= after_id), keeping
 * the order of the rest. Returns the number kept. */
size_t hy_api_page_filter(hy_log_t *v, size_t n, int64_t after_ts, uint32_t after_id);
/* Codec line plus "local":"YYYY-MM-DD HH:MM" in the entry's own tz. Length, or -1 when it does not fit. */
int hy_api_log_json(const hy_log_t *e, char *buf, size_t len);

typedef struct {
    uint32_t med_id;
    int64_t ts;
    int64_t orig_ts;
    hy_status_t status;
    char note[64];
    bool has_ts, has_note, has_status, has_med, confirm;
} hy_api_logreq_t;
/* Body of a log create or edit: "med_id", "ts", "orig_ts", "status", "note", "confirm". Absent fields leave
 * their has_* flag false. Over-long note: HY_E_INVALID, field "note". */
hy_err_t hy_api_logreq_from_json(const char *body, hy_api_logreq_t *out, hy_api_err_t *err);

/* RFC 4180 field: quoted when it holds , " CR or LF; a leading = + - @ TAB or CR gets a ' prefix. Length, or
 * -1 when it does not fit. */
int hy_csv_field(const char *in, char *out, size_t len);
/* id,local_time,utc,tz,code,name,status,slot_local,delta_min,note,source,edited (no line terminator); m NULL
 * gives code "?". Text columns go through hy_csv_field, numbers are bare. */
int hy_api_log_csv(const hy_log_t *e, const hy_med_t *m, char *buf, size_t len);

/* ---- time ---- */

typedef struct {
    int64_t utc_ms;
    char zone[48];
    bool has_zone;
    bool is_auto;
} hy_api_time_t;
/* {"utc_ms":..., "zone":"Europe/Zurich", "auto":true}. utc_ms before build_epoch * 1000: HY_E_INVALID field
 * "utc_ms". A zone must be in hy_tzdb (HY_E_INVALID, field "zone"); an auto request drops the zone. */
hy_err_t hy_api_time_from_json(const char *body, int64_t build_epoch, hy_api_time_t *out, hy_api_err_t *err);
/* Manual: true. Auto: true when the clock is invalid or |utc_ms - device_utc_ms| > 2000. */
bool hy_api_time_should_set(const hy_api_time_t *t, int64_t device_utc_ms, bool clock_valid);

#endif /* HY_API_H */
