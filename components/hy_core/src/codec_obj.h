#ifndef HY_CODEC_OBJ_H
#define HY_CODEC_OBJ_H

/* Single-record cJSON encode and decode shared by the stored format (codec.c) and the web API (api_*.c). */

#include "cJSON.h"
#include "hy/types.h"

/* Medicine object as stored. NULL when the record is out of range. Caller frees with cJSON_Delete. */
cJSON *hy_codec_med_obj(const hy_med_t *m);
/* Defaults, then the object's fields; id, code and name are required. */
hy_err_t hy_codec_med_from_obj(const cJSON *o, hy_med_t *m);
/* Log entry object as stored. NULL when the record is out of range. */
cJSON *hy_codec_log_obj(const hy_log_t *e);
hy_err_t hy_codec_log_from_obj(const cJSON *o, hy_log_t *out);

#endif /* HY_CODEC_OBJ_H */
