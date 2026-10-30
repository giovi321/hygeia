#ifndef HY_CODEC_H
#define HY_CODEC_H

#include <stddef.h>
#include "hy/gap.h"
#include "hy/types.h"

/*
 * JSON codec, schema version 1. Needs cJSON only.
 *
 * Log line (JSON Lines, one object per line, no '\n' inside):
 *   {"id":7,"ts":1793181600,"tz":"CET-1CEST,M3.5.0,M10.5.0/3","med_id":2,
 *    "status":"taken|skipped|missed","slot":1793181000 (omitted when 0),
 *    "delta_min":10,"note":"text" (omitted when empty),
 *    "source":"screen|web|mqtt|auto","edited":false}
 *   Required on decode: id, ts, med_id, status. Others take zero values.
 *
 * Medicines file:
 *   {"v":1,"meds":[{"id":1,"code":"A","name":"Antibiotic","dose":"1 tab","color":0,
 *     "schedule":{"type":"fixed|as_needed","times":["08:00","20:00"],
 *                 "weekdays":127,"min_interval_min":0},
 *     "early_window_min":60,"late_after_min":30,"missed_after_min":120,
 *     "pill_count":{"enabled":false,"remaining":0,"alert":0},
 *     "active_from":20261001 (YYYYMMDD, omitted when 0),"active_until":20261031,
 *     "archived":false,"order":0}]}
 *   Required on decode: id, code, name. Missing optional fields take
 *   hy_med_defaults values.
 *
 * Decoding ignores unknown fields and treats null as absent. Wrong types,
 * unknown enum strings, strings too long for their buffers, times outside
 * 00:00-23:59 and more than HY_MAX_TIMES times give HY_E_INVALID; nothing is
 * truncated silently.
 */

#define HY_SCHEMA_VERSION 1

/* One line without '\n'. Returns the length, or -1 if len is too small (or e is invalid, or ts/slot beyond 15 digits). */
int      hy_log_to_jsonl(const hy_log_t *e, char *buf, size_t len);
hy_err_t hy_log_from_jsonl(const char *line, hy_log_t *out);

/*
 * Gap line: {"s":1793000000,"e":1793003600,"r":"off|lost|clock|jump"}. Required on decode:
 * s, e, r; e must be after s. Returns the length, or -1 if len is too small or the gap is invalid.
 */
int      hy_gap_to_jsonl(const hy_gap_t *g, hy_gap_reason_t r, char *buf, size_t len);
hy_err_t hy_gap_from_jsonl(const char *line, hy_gap_t *g, hy_gap_reason_t *r);

/* Returns a malloc'd NUL-terminated string (release with free()), or NULL on failure. */
char    *hy_meds_to_json(const hy_med_t *meds, size_t n);
/* v != 1 gives HY_E_INVALID; more than max medicines gives HY_E_FULL. *n is set on success only. */
hy_err_t hy_meds_from_json(const char *json, hy_med_t *out, size_t max, size_t *n);

#endif /* HY_CODEC_H */
