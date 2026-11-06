#ifndef HY_MQTTFMT_H
#define HY_MQTTFMT_H

/* MQTT topics, state payloads, command parsing and Home Assistant discovery configs. Pure C, no ESP-IDF headers. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hy/api.h"
#include "hy/integ.h"
#include "hy/types.h"

#define HY_MQTT_TOPIC_LEN 128

/* "<base>/<leaf>" and "<base>/med/<id>/<leaf>". Each returns the length, -1 when it does not fit. */
int hy_mqtt_topic(const char *base, const char *leaf, char *out, size_t len);
int hy_mqtt_med_topic(const char *base, uint32_t id, const char *leaf, char *out, size_t len);

typedef enum { HY_MC_LOG = 0, HY_MC_SKIP, HY_MC_SNOOZE } hy_mqtt_cmd_t;
/* "<base>/med/<id>/log|skip|snooze", id as in the route {id} rule (1..4294967295, no leading zero). The topic
 * need not be NUL-terminated. false otherwise. */
bool hy_mqtt_parse_cmd(const char *base, const char *topic, size_t topic_len, uint32_t *id, hy_mqtt_cmd_t *cmd);
/* A command is acted on only when it is not retained. QoS 1 duplicates are acted on. */
bool hy_mqtt_cmd_accept(bool retained);

typedef struct { bool has_ts; int64_t ts; bool has_note; char note[64]; bool confirm; } hy_mqtt_log_req_t;
/* Empty, "PRESS" or any payload that is not a JSON object: a plain log now. A JSON object (at most 255 bytes) is
 * read with hy_api_logreq_from_json (the web's rules for "ts", "note" and "confirm"; "med_id", "status" and
 * "orig_ts" are not used); a failure gives false: ignore the command. */
bool hy_mqtt_parse_log(const char *p, size_t n, hy_mqtt_log_req_t *out);
/* The web's window for a given ts: build_epoch <= ts <= now + 60. */
bool hy_mqtt_log_ts_ok(int64_t ts, int64_t now, int64_t build_epoch);
/* Empty or "PRESS": def. A decimal integer, or {"minutes":n}: n when 1..120. false otherwise. */
bool hy_mqtt_parse_snooze(const char *p, size_t n, uint16_t def, uint16_t *minutes);
/* An as-needed medicine with min_interval_min > 0, a log without ts and without confirm, and interval_check
 * giving HY_W_TOO_SOON: refused (false). Everything else true. */
bool hy_mqtt_log_decide(const hy_med_t *m, const hy_mqtt_log_req_t *req, hy_err_t interval_check);

/* State JSON of one tile, member order fixed:
 * {"id":3,"code":"A","name":"Antibiotic","status":"<kind>","due":b,"last_taken":"2025-11-06T07:02:00Z"|null,
 *  "next_due":"..."|null,"pills_left":n|null}. kind names as GET /api/state. due: kind due, late or snoozed.
 * Times are UTC ISO 8601 with "Z". Length, or -1 when it does not fit or the kind is out of range. */
int hy_mqtt_med_state(const hy_api_tile_t *t, char *buf, size_t len);

typedef struct { int8_t batt_pct; bool usb, charging; uint8_t net_mode; int8_t rssi; char clock_src[16];
                 bool clock_valid; int16_t adherence; /* -1 unknown */ char fw[32]; } hy_mqtt_device_t;
/* {"battery":n|null,"charging":b,"usb":b,"wifi_mode":"off|client|ap","rssi":n|null,"clock_source":".."|null,
 *  "clock_valid":b,"adherence_30d":n|null,"fw":".."}. battery < 0, rssi 0, an empty clock_source and adherence < 0
 * give null. */
int hy_mqtt_device_state(const hy_mqtt_device_t *d, char *buf, size_t len);
/* Rounded percent 100 * taken / resolved, -1 when resolved is 0. */
int16_t hy_mqtt_adherence_pct(uint32_t taken, uint32_t resolved);

/* ---- Home Assistant discovery ---- */

/* node id: "hygeia_" + the 4 hex digits of the client id, lower case ("hygeia-1A2B" gives "hygeia_1a2b"). */
void hy_disc_node_id(const char *client_id, char out[16]);
typedef enum { HY_DO_LAST_TAKEN = 0, HY_DO_NEXT_DUE, HY_DO_DUE, HY_DO_LOG, HY_DO_PILLS, HY_DO_MED_COUNT } hy_disc_med_obj_t;
typedef enum { HY_DO_BATTERY = 0, HY_DO_CHARGING, HY_DO_ADHERENCE, HY_DO_EVENT, HY_DO_DEV_COUNT } hy_disc_dev_obj_t;
/* "<prefix>/<component>/<node>/<object>/config". Components: sensor, sensor, binary_sensor, button, sensor for
 * the medicine objects; sensor, binary_sensor, sensor, event for the device objects. Object ids "med_<id>_last_taken",
 * "med_<id>_next_due", "med_<id>_due", "med_<id>_log", "med_<id>_pills", "battery", "charging", "adherence_30d",
 * "event". Length, or -1 when it does not fit or the object is out of range. */
int hy_disc_med_topic(const char *prefix, const char *node, uint32_t id, hy_disc_med_obj_t o, char *out, size_t len);
int hy_disc_dev_topic(const char *prefix, const char *node, hy_disc_dev_obj_t o, char *out, size_t len);

typedef struct { const char *base, *node, *fw; } hy_disc_ctx_t;
/* Config JSON (cJSON, unformatted), malloc'd, NULL on failure. Every config has "name", "unique_id"
 * "<node>_<object>", "availability_topic" "<base>/status" and the "device" block (identifiers, name "Hygeia",
 * manufacturer "Waveshare", model "ESP32-S3-Touch-AMOLED-2.16", sw_version). Medicine objects read
 * "<base>/med/<id>/state"; the button publishes "PRESS" to "<base>/med/<id>/log". The event entity reads
 * "<base>/event" and lists all nine event types. */
char *hy_disc_med_config(const hy_disc_ctx_t *c, uint32_t id, const char *name, hy_disc_med_obj_t o);
char *hy_disc_dev_config(const hy_disc_ctx_t *c, hy_disc_dev_obj_t o);
/* Parse "<prefix>/<component>/<node>/<object>/config" of this node: object id into obj. false for other nodes. */
bool hy_disc_parse_topic(const char *prefix, const char *node, const char *topic, size_t n, char obj[32]);

typedef struct { uint32_t id; bool pills; } hy_disc_med_t;   /* active medicines only */
/* Should this object id exist? Device objects: ha_on. Medicine objects: ha_on and the id is active (and pill
 * counting on for "_pills"). Unknown object ids: false (they are removed). */
bool hy_disc_wanted(const char *obj, bool ha_on, const hy_disc_med_t *meds, size_t n);
/* "<base>/med/<id>/state" of an id that is not active: true (clear it). Other topics false. */
bool hy_mqtt_state_stale(const char *base, const char *topic, size_t n, const hy_disc_med_t *meds, size_t nm);

#endif /* HY_MQTTFMT_H */
