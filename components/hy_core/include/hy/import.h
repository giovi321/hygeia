#ifndef HY_IMPORT_H
#define HY_IMPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hy/actions.h"
#include "hy/api.h"
#include "hy/gap.h"
#include "hy/netcfg.h"
#include "hy/settings.h"
#include "hy/types.h"

/*
 * Backup file (export and import), one JSON object:
 *   {"v":1,"api":1,"exported":<epoch s>,"meds":[<codec medicine>...],"next_id":n,
 *    "gaps":[{"s","e","r"}...],"settings":{<hy_api_settings_json>},
 *    "wifi":{"networks":[{"ssid","pass"}],"ap_pass":".."} (only in an export with secrets),
 *    "log":[<codec log line>...]}
 * The export writes "log" as the last member so it can be streamed one entry at a time. The import parses
 * everything but "log" as one cJSON tree and walks the "log" array one element at a time with a string-aware
 * bracket scanner, so a large file never becomes one tree. "log" may sit anywhere in the object.
 *
 * Required: "v" (1, else HY_E_INVALID field "v") and "meds". Optional: next_id (absent: max id + 1), gaps,
 * settings, wifi, log. Every error is HY_E_INVALID with the member in err->field ("json" when the text is
 * not one object, or nests deeper than 8 levels in the head or 2 in the log array), except medicine rule
 * failures (hy_med_validate codes with err->field from hy_api_err_from) and too many medicines (HY_E_FULL).
 */

#define HY_IMPORT_GAPS_MAX 1100      /* the store trims gaps.jsonl to 1000 after 1100 */
#define HY_IMPORT_ENTRY_MAX 1024     /* longest log element, as the store's longest line */

typedef enum { HY_IMPORT_MERGE = 0, HY_IMPORT_REPLACE } hy_import_mode_t;
typedef enum { HY_IMP_SAME = 0, HY_IMP_CHANGE, HY_IMP_ADD } hy_import_kind_t;

typedef struct {
    size_t meds_add, meds_change, meds_same;
    size_t log_add, log_dup;
    size_t log_skip;     /* entries of a medicine the file does not list: never imported */
    size_t gaps_add;     /* merge: every gap of the file; the caller narrows it against the stored gaps */
    bool settings;       /* the file has settings */
    bool secrets;        /* the file has Wi-Fi secrets */
} hy_import_diff_t;

/* Saved networks and the access point password. Passwords are never logged; hy_import_free wipes them. */
typedef struct {
    size_t n;
    char ssid[HY_NET_MAX_KNOWN][33];
    char pass[HY_NET_MAX_KNOWN][64];
    char ap_pass[64];    /* "" = none */
} hy_backup_wifi_t;

/* What a file medicine becomes on the device. dev_id 0: a fresh id the upsert assigns (merge ADD); the caller
 * records it with hy_import_set_dev_id once known, so the medicine's log follows. */
typedef struct {
    uint32_t file_id;
    uint32_t dev_id;
    uint8_t kind;        /* hy_import_kind_t */
} hy_import_med_t;

typedef struct {
    int64_t exported;
    hy_med_t meds[HY_MAX_STORED];
    hy_import_med_t map[HY_MAX_STORED];   /* one per meds[] entry, filled by the plan */
    size_t n_meds;
    uint32_t next_id;
    hy_gap_t gaps[HY_IMPORT_GAPS_MAX];
    uint8_t gap_reasons[HY_IMPORT_GAPS_MAX];
    size_t n_gaps;
    char *settings;      /* the "settings" object as JSON text (malloc'd), NULL when absent */
    bool has_wifi;
    hy_backup_wifi_t wifi;
    const char *log;     /* the '[' of the log array inside the parsed text, NULL when absent */
    size_t n_log;        /* elements of the log array */
} hy_import_t;

/* Parses and checks the whole file (every log element too) into *imp; *imp needs no setup. The text must stay
 * unchanged while imp->log is used. On failure nothing is left to free. */
hy_err_t hy_import_parse(const char *json, hy_import_t *imp, hy_api_err_t *err);
/* Frees imp->settings and wipes the secrets. */
void     hy_import_free(hy_import_t *imp);

typedef bool (*hy_import_log_exists_t)(void *ctx, uint32_t med, int64_t ts, hy_status_t st);

/*
 * Pure: fills imp->map and *diff from the current medicines (archived included) and the log_exists callback,
 * which answers from data the caller collected (never the store itself).
 * Merge: same id and same code is the same medicine (SAME when the etag matches, else CHANGE); same id with a
 * different code, or an unknown id, is ADD under a fresh id with its log following it. A log entry is a
 * duplicate when (device medicine, ts, status) exists; entries of an ADD medicine are always added.
 * Replace: every medicine, entry and gap of the file is added under its own id; nothing is kept. Ids must be
 * unique and nonzero (log ids also below 0xFFFFFFFF; HY_E_INVALID field "meds" or "log").
 * Both modes check the resulting medicine list with hy_med_validate and the limits (HY_E_FULL), so the commit
 * does not fail on a rule halfway.
 */
hy_err_t hy_import_plan_parsed(hy_import_t *imp, hy_import_mode_t mode, const hy_med_t *cur, size_t n_cur,
                               hy_import_log_exists_t log_exists, void *ctx, hy_import_diff_t *diff,
                               hy_api_err_t *err);
/* hy_import_parse, hy_import_plan_parsed and hy_import_free in one call (diff only). */
hy_err_t hy_import_plan(const char *json, hy_import_mode_t mode, const hy_med_t *cur, size_t n_cur,
                        hy_import_log_exists_t log_exists, void *ctx, hy_import_diff_t *diff, hy_api_err_t *err);

/* Device medicine of a file medicine id after the plan. false when the file does not list it (the entry is
 * skipped) or its fresh id is not known yet. *kind may be NULL. */
bool     hy_import_target(const hy_import_t *imp, uint32_t file_med, uint32_t *dev_med, uint8_t *kind);
/* Records the fresh id a merge ADD got from the upsert. */
void     hy_import_set_dev_id(hy_import_t *imp, uint32_t file_med, uint32_t dev_id);

/* Walks the log array: 1 with the entry in *out, 0 at the end, -1 on a malformed element (never after a
 * successful hy_import_parse). The cursor holds the element's text while it is decoded (about 1 KB): keep it
 * off small stacks. */
typedef struct {
    const char *p;
    bool started, done;
    char line[HY_IMPORT_ENTRY_MAX];
} hy_import_cursor_t;
void     hy_import_log_begin(const hy_import_t *imp, hy_import_cursor_t *c);
int      hy_import_log_next(hy_import_cursor_t *c, hy_log_t *out);

/* ---- export ---- */

typedef struct {
    int64_t exported;
    const hy_med_t *meds;
    size_t n_meds;
    uint32_t next_id;
    const hy_gap_t *gaps;
    const uint8_t *gap_reasons;          /* hy_gap_reason_t values */
    size_t n_gaps;
    const hy_settings_t *settings;       /* NULL: no "settings" member */
    const hy_backup_wifi_t *wifi;        /* NULL: no "wifi" member (an export without secrets) */
} hy_export_head_t;

/* Everything before the log entries: '{' ... ,"log":[ (malloc'd, release with free()). The entries follow as
 * codec log lines (hy_log_to_jsonl) separated by ',', then HY_EXPORT_TAIL. NULL on failure. */
char    *hy_export_head_json(const hy_export_head_t *h);
#define HY_EXPORT_TAIL "]}"
/* First line of the CSV export, without the line terminator; the rows are hy_api_log_csv. */
#define HY_EXPORT_CSV_HEADER "id,local_time,utc,tz,code,name,status,slot_local,delta_min,note,source,edited"

#endif /* HY_IMPORT_H */
