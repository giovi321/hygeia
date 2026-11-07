#ifndef HY_INTEG_H
#define HY_INTEG_H

/* Integration settings (MQTT broker, webhooks): data, validation, JSON views. Pure C, no ESP-IDF headers. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hy/api.h"
#include "hy/events.h"
#include "hy/types.h"

#define HY_HOOK_MAX 4
#define HY_INTEG_URL_MAX 255      /* bytes, without the NUL */
#define HY_INTEG_CRED_MAX 64      /* user, password, secret */
#define HY_INTEG_TOPIC_MAX 32

/* Event types sent to MQTT and webhooks. */
typedef enum { HY_IE_TAKEN = 0, HY_IE_SKIPPED, HY_IE_MISSED, HY_IE_DUE, HY_IE_SNOOZED, HY_IE_UNDONE,
               HY_IE_BATTERY_LOW, HY_IE_BATTERY_CRITICAL, HY_IE_CLOCK_INVALID, HY_IE_COUNT } hy_iev_t;
#define HY_IE_ALL ((uint16_t)((1u << HY_IE_COUNT) - 1))
const char *hy_iev_name(hy_iev_t t);                 /* "taken" ... "clock_invalid"; NULL when out of range */
bool        hy_iev_from_name(const char *s, hy_iev_t *out);
/* True for the nine types (UNDONE included); false for LOG_EDITED, MEDS_CHANGED, UNTRACKED. */
bool        hy_iev_from_bus(hy_ev_type_t t, hy_iev_t *out);

typedef struct { bool on; char url[HY_INTEG_URL_MAX + 1]; char user[HY_INTEG_CRED_MAX + 1];
                 char pass[HY_INTEG_CRED_MAX + 1]; char base[HY_INTEG_TOPIC_MAX + 1];
                 bool ha; char ha_prefix[HY_INTEG_TOPIC_MAX + 1]; } hy_mqtt_cfg_t;
typedef struct { bool on; char url[HY_INTEG_URL_MAX + 1]; uint16_t events; char secret[HY_INTEG_CRED_MAX + 1]; } hy_hook_cfg_t;
/* The tag lets api.h name the type without including this header. */
typedef struct hy_integ_s { hy_mqtt_cfg_t mqtt; uint8_t n_hooks; hy_hook_cfg_t hook[HY_HOOK_MAX]; } hy_integ_t;

/* MQTT off, url "", user "", pass "", base "hygeia", ha off, ha_prefix "homeassistant", n_hooks 0. */
void hy_integ_defaults(hy_integ_t *c);

typedef struct { bool tls; char host[65]; uint16_t port; const char *path; /* into the input, "" when none */ } hy_url_t;
/* mqtt true: "mqtt://" (port 1883) or "mqtts://" (8883), no path other than "" or "/".
 * mqtt false: "http://" (80) or "https://" (443), optional path starting with "/".
 * Scheme case-insensitive. Host 1..64 bytes of A-Z a-z 0-9 . - (a dotted IPv4 qualifies), not starting or
 * ending with '.' or '-'. Optional ":port" 1..65535. No userinfo ('@' before the host), no '#', no spaces or
 * control bytes, at most HY_INTEG_URL_MAX bytes. HY_E_INVALID otherwise. */
hy_err_t hy_url_parse(const char *url, bool mqtt, hy_url_t *out);
/* 1..32 bytes of A-Z a-z 0-9 _ - /, not starting with '/' or '$', not ending with '/', no "//". */
hy_err_t hy_integ_check_topic(const char *t);
/* 0..64 bytes, no byte below 0x20 or 0x7F, valid UTF-8. */
hy_err_t hy_integ_check_cred(const char *s);
/* Whole-config rules: mqtt.on needs a valid mqtt url; an off mqtt may keep "" or a valid url; base and ha_prefix
 * pass hy_integ_check_topic; every hook url valid and unique (byte compare after lower-casing scheme and host);
 * events a subset of HY_IE_ALL. HY_E_INVALID with err->field set ("mqtt.url", "mqtt.base", "mqtt.ha_prefix",
 * "mqtt.user", "mqtt.password", "webhooks.<i>.url", "webhooks.<i>.events", "webhooks.<i>.secret"). */
hy_err_t hy_integ_validate(const hy_integ_t *c, hy_api_err_t *err);

struct cJSON;
/* Adds "mqtt":{"on","url","user","password_set","base","ha","ha_prefix"} and
 * "webhooks":[{"on","url","events":[names in hy_iev_t order],"secret_set"}] to obj. secrets true (export with
 * secrets only) also adds "password" and "secret" with the stored values. False on no memory. */
bool hy_api_integ_add(struct cJSON *obj, const hy_integ_t *c, bool secrets);
/* Merge of a settings PUT body onto *cur: only the "mqtt" and "webhooks" members are read.
 * "mqtt": any subset of on, url, user, password, base, ha, ha_prefix; "password":"" clears, absent keeps;
 * "password_set" ignored. "webhooks": an array that replaces the list (0..HY_HOOK_MAX items). Each item needs
 * "url"; "on" defaults true; "events" defaults to every type; "secret" string sets ("" clears); absent keeps the
 * secret of the stored hook in the same position (item i keeps slot i's secret, also when its url changed); an
 * item beyond the stored count starts with "". "secret_set" ignored.
 * null counts as absent. Then hy_integ_validate. Wrong types, more than 4 items, unknown event names:
 * HY_E_INVALID with the field. On failure *out is untouched. Uses one static scratch copy: call it from the
 * LVGL task only. */
hy_err_t hy_api_integ_from_json(const char *body, const hy_integ_t *cur, hy_integ_t *out, hy_api_err_t *err);

/* Same rules as hy_api_integ_from_json onto the defaults, result discarded; uses its own static scratch (PSRAM on
 * the device), so it may run beside hy_api_integ_from_json but not beside another call of itself: the backup
 * import is the only caller and runs one at a time. */
hy_err_t hy_api_integ_check_json(const char *body, hy_api_err_t *err);

/* Overwrites the "mqtt"."password" and "webhooks"[]."secret" strings of a parsed settings body. Call it on every
 * tree parsed from a settings body before cJSON_Delete, so no secret stays in freed heap. NULL is fine. */
void hy_api_integ_wipe_tree(struct cJSON *root);

/* ---- status view ---- */

typedef enum { HY_MQS_OFF = 0, HY_MQS_WAITING, HY_MQS_CONNECTING, HY_MQS_CONNECTED, HY_MQS_ERROR } hy_mqtt_state_t;
typedef enum { HY_HE_NONE = 0, HY_HE_OFFLINE, HY_HE_CONNECT, HY_HE_TLS, HY_HE_TIMEOUT, HY_HE_HTTP, HY_HE_MEMORY,
               HY_HE_AUTH, HY_HE_BUSY, HY_HE_STORAGE, HY_HE_COUNT } hy_integ_err_t;
/* "", "offline", "connect", "tls", "timeout", "http", "memory", "auth", "busy", "storage"; NULL out of range. */
const char *hy_integ_err_name(hy_integ_err_t e);
typedef struct { int64_t at; int16_t http; uint8_t err; uint32_t pending; int32_t retry_in_s; /* -1 none */
                 bool paused; } hy_hook_status_t;
typedef struct {
    hy_mqtt_state_t mqtt; uint8_t mqtt_err; int64_t mqtt_since; bool ha; uint32_t mqtt_pending;
    uint8_t n_hooks; hy_hook_status_t hook[HY_HOOK_MAX];
    uint32_t pending, dropped, expired, rejected, lost; uint8_t outbox_err; /* HY_HE_STORAGE when on the RAM fallback */
    bool test_running; int8_t test_index; int16_t test_http; uint8_t test_err;
    bool override;   /* debug RAM override active */
} hy_integ_status_t;
/* {"mqtt":{"state":"off|waiting|connecting|connected|error","error":name|null,"since":epoch|null,"ha":b,"pending":n},
 * "webhooks":[{"index":i,"last":{"at":epoch|null,"status":code|null,"error":name|null},"pending":n,
 * "retry_in_s":n|null,"paused":b}],"outbox":{"pending":n,"capacity":1000,"dropped":n,"expired":n,"rejected":n,
 * "lost":n,"error":name|null},"test":{"running":b,"index":i|null,"status":code|null,"error":name|null}}.
 * "override" is not shown. malloc'd; NULL on failure. */
char *hy_api_integ_status_json(const hy_integ_status_t *s);

#endif /* HY_INTEG_H */
