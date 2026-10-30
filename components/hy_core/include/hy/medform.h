#ifndef HY_MEDFORM_H
#define HY_MEDFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "hy/types.h"
#include "hy/med.h"

/*
 * State and rules of the on-device medicine form. Pure: no clock, storage or UI.
 * Setters change the draft only on success and set `dirty` only when a value
 * actually changes.
 */

typedef enum { HY_MF_NAME = 0, HY_MF_DOSE, HY_MF_CODE, HY_MF_COLOR, HY_MF_TYPE, HY_MF_TIMES,
               HY_MF_INTERVAL, HY_MF_REVIEW } hy_mf_step_t;

typedef struct {
    hy_med_t draft;       /* only the form fields are meaningful on edit */
    uint32_t edit_id;     /* 0 = new */
    bool dirty;           /* any setter changed a value since open */
    bool from_review;     /* the step was opened from Review: Next returns to Review */
    hy_mf_step_t step;
} hy_medform_t;

#define HY_MF_NAME_MAX 32
#define HY_MF_DOSE_MAX 16
#define HY_MF_STEPS     6  /* "Step n of 6": name, dose, code, colour, type, times or interval */

/* New: defaults (hy_med_defaults), type FIXED, times {480} (08:00), n_times 1, min_interval 0,
   color = hy_mf_default_color, code "", name "", dose "", step NAME, dirty false. */
void     hy_mf_new(hy_medform_t *f, const hy_med_t *meds, size_t n);
/* Edit: copies the live record of id (times sorted ascending); step REVIEW.
   HY_E_NOT_FOUND for unknown or archived. */
hy_err_t hy_mf_edit(hy_medform_t *f, const hy_med_t *meds, size_t n, uint32_t id);

/* Trims leading and trailing spaces, collapses runs of spaces to one, upper-cases the first byte when a..z.
   HY_E_NAME when empty after trimming or longer than 32 bytes; the draft is unchanged on error. */
hy_err_t hy_mf_set_name(hy_medform_t *f, const char *name);
/* Trims and collapses spaces like the name, no automatic capital. Empty is allowed. HY_E_DOSE when longer
   than 16 bytes after trimming; the draft is unchanged on error. */
hy_err_t hy_mf_set_dose(hy_medform_t *f, const char *dose);

/* 1 or 2 of A..Z 0..9 after upper-casing a..z, else HY_E_CODE. HY_E_CODE_DUP when an active medicine
   other than self_id has it; *dup_id (may be NULL) gets that medicine's id. */
hy_err_t hy_mf_check_code(const char *code, const hy_med_t *meds, size_t n, uint32_t self_id, uint32_t *dup_id);
hy_err_t hy_mf_set_code(hy_medform_t *f, const char *code, const hy_med_t *meds, size_t n, uint32_t *dup_id);
/* First free candidate (not used by an active medicine other than self_id), in this order:
   1. the name's first character upper-cased, when A..Z or 0..9
   2. the first characters of the first two words ("Vitamin D" -> "VD")
   3. the first character followed by each later letter or digit of the name in order ("VI", "VT", "VA", ...)
   4. A..Z, then 0..9, as single characters
   out = "" when none is free. */
void     hy_mf_suggest_code(const char *name, const hy_med_t *meds, size_t n, uint32_t self_id, char out[3]);

/* Lowest palette index 0..9 not used by an active medicine; when all are used, the least used, lowest
   index on a tie. */
uint8_t  hy_mf_default_color(const hy_med_t *meds, size_t n);
hy_err_t hy_mf_set_color(hy_medform_t *f, uint8_t color);          /* HY_E_INVALID above 9 */
/* The other type's values stay in the draft. Switching to FIXED with no times adds 08:00. */
void     hy_mf_set_type(hy_medform_t *f, hy_sched_type_t t);

/* Time list, minutes after local midnight, kept ascending. add: HY_E_TIMES when >= 1440, a duplicate, or
   already 8. remove: HY_E_TIMES when idx is out of range or it is the last time. */
hy_err_t hy_mf_add_time(hy_medform_t *f, uint16_t min);
hy_err_t hy_mf_remove_time(hy_medform_t *f, size_t idx);
/* Default for "Add a time": the first of 480, 720, 1080, 1320 (08:00, 12:00, 18:00, 22:00) not in the list,
   else 480. */
uint16_t hy_mf_next_time_default(const hy_medform_t *f);

/* Interval choices for the roller: 0, 30, 60, 120, 180, 240, 360, 480, 720, 1440, plus `current` inserted in
   ascending order when it is in 1..1440 and not already listed. Returns the count (at most 11). */
size_t   hy_mf_interval_options(uint16_t current, uint16_t *out, size_t max);
hy_err_t hy_mf_set_interval(hy_medform_t *f, uint16_t min);        /* HY_E_INTERVAL above 1440 */

/* Step flow. Add: NAME -> DOSE -> CODE -> COLOR -> TYPE -> TIMES (fixed) or INTERVAL (as needed) -> REVIEW.
   next() from any step with from_review set returns REVIEW. prev() of NAME, and of REVIEW in edit mode,
   returns NAME and the caller leaves the form. */
hy_mf_step_t hy_mf_next(const hy_medform_t *f);
hy_mf_step_t hy_mf_prev(const hy_medform_t *f);
int          hy_mf_step_number(const hy_medform_t *f);   /* 1..6 for the counted steps, 0 for REVIEW */

/* Builds the record to save. New: hy_med_defaults + the form fields, order = max order + 1, archived false.
   Edit: the CURRENT live record of edit_id from meds (not the copy taken at hy_mf_edit) with only code,
   name, dose, color, type, times, n_times, min_interval_min replaced. As needed: n_times 0 and times zeroed.
   Fixed: min_interval_min keeps the live value (0 for a new medicine). Then hy_med_validate against meds.
   HY_E_NOT_FOUND when edit_id is gone or archived. *bad (may be NULL) = hy_mf_step_for_err(result), except
   that a step is named only when the form can fix the error there: HY_E_INVALID gives COLOR only when the
   draft colour is above 9, HY_E_TIMES gives TIMES only when the draft's own times are invalid, and
   HY_E_INTERVAL gives INTERVAL only for an as-needed medicine; otherwise REVIEW (the error comes from a
   web-only field of the live record). */
hy_err_t hy_mf_build(const hy_medform_t *f, const hy_med_t *meds, size_t n, hy_med_t *out, hy_mf_step_t *bad);
/* NAME for HY_E_NAME; DOSE for HY_E_DOSE; CODE for HY_E_CODE and HY_E_CODE_DUP; COLOR for HY_E_INVALID;
   TIMES for HY_E_TIMES; INTERVAL for HY_E_INTERVAL; REVIEW for everything else. */
hy_mf_step_t hy_mf_step_for_err(hy_err_t e);

/* Today's occurrences of the built record that would be due right away at save: occurrences in
   (now - missed_after_min * 60, now] in posix_tz. The window can reach the previous local day when now is
   early in the morning. On edit only times not present in `old` count.
   Returns the count, at most max. */
size_t   hy_mf_due_on_save(const hy_med_t *built, const hy_med_t *old /* NULL = new */, int64_t now,
                           const char *posix_tz, int64_t *out, size_t max);

#endif
