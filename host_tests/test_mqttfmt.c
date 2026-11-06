#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "unity.h"
#include "cJSON.h"
#include "hy/mqttfmt.h"

void setUp(void) {}
void tearDown(void) {}

#define T_TAKEN 1762412520LL

static void test_mqtt_topics(void)
{
    char b[HY_MQTT_TOPIC_LEN];
    TEST_ASSERT_EQUAL_INT(13, hy_mqtt_topic("hygeia", "status", b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("hygeia/status", b);
    TEST_ASSERT_EQUAL_INT(23, hy_mqtt_med_topic("home/hygeia", 3, "state", b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("home/hygeia/med/3/state", b);
    /* 129 bytes does not fit in 128 */
    char base[130];
    memset(base, 'a', 122);
    base[122] = '\0';
    TEST_ASSERT_EQUAL_INT(-1, hy_mqtt_topic(base, "status", b, sizeof b));   /* 129 bytes */
    base[120] = 0;
    TEST_ASSERT_EQUAL_INT(127, hy_mqtt_topic(base, "status", b, sizeof b));
}

static void test_mqtt_cmd_parse(void)
{
    uint32_t id = 0;
    hy_mqtt_cmd_t c = HY_MC_SKIP;
    const char *t = "hygeia/med/3/log";
    TEST_ASSERT_TRUE(hy_mqtt_parse_cmd("hygeia", t, strlen(t), &id, &c));
    TEST_ASSERT_EQUAL_UINT32(3, id);
    TEST_ASSERT_EQUAL_INT(HY_MC_LOG, c);
    t = "hygeia/med/12/snooze";
    TEST_ASSERT_TRUE(hy_mqtt_parse_cmd("hygeia", t, strlen(t), &id, &c));
    TEST_ASSERT_EQUAL_UINT32(12, id);
    TEST_ASSERT_EQUAL_INT(HY_MC_SNOOZE, c);
    t = "hygeia/med/4294967295/skip";
    TEST_ASSERT_TRUE(hy_mqtt_parse_cmd("hygeia", t, strlen(t), &id, &c));
    TEST_ASSERT_EQUAL_UINT32(4294967295u, id);
    TEST_ASSERT_EQUAL_INT(HY_MC_SKIP, c);
    /* the length bounds the match, not a NUL */
    t = "hygeia/med/3/logx";
    TEST_ASSERT_TRUE(hy_mqtt_parse_cmd("hygeia", t, strlen(t) - 1, &id, &c));
    static const char *const bad[] = { "hygeia/med/03/log", "hygeia/med/3/undo", "other/med/3/log",
                                       "hygeia/med//log", "hygeia/med/4294967296/log", "hygeia/med/3/logs",
                                       "hygeia/med/3", "hygeia/med/3/log/x", "hygeia/med/0/log",
                                       "hygeia/device" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        TEST_ASSERT_FALSE_MESSAGE(hy_mqtt_parse_cmd("hygeia", bad[i], strlen(bad[i]), &id, &c), bad[i]);
}

static void test_mqtt_cmd_retained_ignored(void)
{
    TEST_ASSERT_FALSE(hy_mqtt_cmd_accept(true));
    TEST_ASSERT_TRUE(hy_mqtt_cmd_accept(false));
}

static void test_mqtt_log_payload_plain(void)
{
    static const char *const plain[] = { "", "PRESS", "hello" };
    for (size_t i = 0; i < 3; i++) {
        hy_mqtt_log_req_t r;
        memset(&r, 0xAA, sizeof r);
        TEST_ASSERT_TRUE(hy_mqtt_parse_log(plain[i], strlen(plain[i]), &r));
        TEST_ASSERT_FALSE(r.has_ts);
        TEST_ASSERT_FALSE(r.has_note);
        TEST_ASSERT_FALSE(r.confirm);
    }
}

static void test_mqtt_log_payload_json(void)
{
    hy_mqtt_log_req_t r;
    const char *p = "{\"note\":\"after lunch\",\"confirm\":true}";
    TEST_ASSERT_TRUE(hy_mqtt_parse_log(p, strlen(p), &r));
    TEST_ASSERT_TRUE(r.has_note);
    TEST_ASSERT_EQUAL_STRING("after lunch", r.note);
    TEST_ASSERT_TRUE(r.confirm);
    TEST_ASSERT_FALSE(r.has_ts);

    p = "{\"ts\":1762412520}";
    TEST_ASSERT_TRUE(hy_mqtt_parse_log(p, strlen(p), &r));
    TEST_ASSERT_TRUE(r.has_ts);
    TEST_ASSERT_EQUAL_INT64(T_TAKEN, r.ts);
    TEST_ASSERT_FALSE(r.has_note);

    char big[128];
    char note[65];
    memset(note, 'n', 64);
    note[64] = '\0';
    snprintf(big, sizeof big, "{\"note\":\"%s\"}", note);
    TEST_ASSERT_FALSE(hy_mqtt_parse_log(big, strlen(big), &r));
    note[63] = '\0';
    snprintf(big, sizeof big, "{\"note\":\"%s\"}", note);
    TEST_ASSERT_TRUE(hy_mqtt_parse_log(big, strlen(big), &r));

    p = "{\"ts\":\"x\"}";
    TEST_ASSERT_FALSE(hy_mqtt_parse_log(p, strlen(p), &r));
    p = "{\"ts\":";
    TEST_ASSERT_FALSE(hy_mqtt_parse_log(p, strlen(p), &r));
    /* med_id and status in the body do not matter */
    p = "{\"med_id\":9,\"status\":\"skipped\",\"confirm\":true}";
    TEST_ASSERT_TRUE(hy_mqtt_parse_log(p, strlen(p), &r));
    TEST_ASSERT_TRUE(r.confirm);
}

static void test_mqtt_log_payload_ts_window(void)
{
    int64_t build = 1760000000, now = 1762412520;
    TEST_ASSERT_TRUE(hy_mqtt_log_ts_ok(build, now, build));
    TEST_ASSERT_FALSE(hy_mqtt_log_ts_ok(build - 1, now, build));
    TEST_ASSERT_TRUE(hy_mqtt_log_ts_ok(now + 60, now, build));
    TEST_ASSERT_FALSE(hy_mqtt_log_ts_ok(now + 61, now, build));
}

static void test_mqtt_snooze_payload(void)
{
    uint16_t m = 0;
    TEST_ASSERT_TRUE(hy_mqtt_parse_snooze("", 0, 15, &m));
    TEST_ASSERT_EQUAL_UINT16(15, m);
    m = 0;
    TEST_ASSERT_TRUE(hy_mqtt_parse_snooze("PRESS", 5, 15, &m));
    TEST_ASSERT_EQUAL_UINT16(15, m);
    TEST_ASSERT_TRUE(hy_mqtt_parse_snooze("30", 2, 15, &m));
    TEST_ASSERT_EQUAL_UINT16(30, m);
    const char *j = "{\"minutes\":5}";
    TEST_ASSERT_TRUE(hy_mqtt_parse_snooze(j, strlen(j), 15, &m));
    TEST_ASSERT_EQUAL_UINT16(5, m);
    TEST_ASSERT_TRUE(hy_mqtt_parse_snooze("120", 3, 15, &m));
    TEST_ASSERT_EQUAL_UINT16(120, m);
    static const char *const bad[] = { "0", "121", "abc", "1.5", "-5", "{\"minutes\":0}", "{\"minutes\":\"5\"}",
                                       "{}", "1000", "5 5" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        TEST_ASSERT_FALSE_MESSAGE(hy_mqtt_parse_snooze(bad[i], strlen(bad[i]), 15, &m), bad[i]);
}

static void test_mqtt_too_soon_refused(void)
{
    hy_med_t m;
    memset(&m, 0, sizeof m);
    m.type = HY_SCHED_AS_NEEDED;
    m.min_interval_min = 240;
    hy_mqtt_log_req_t r;
    memset(&r, 0, sizeof r);
    TEST_ASSERT_FALSE(hy_mqtt_log_decide(&m, &r, HY_W_TOO_SOON));
    r.confirm = true;
    TEST_ASSERT_TRUE(hy_mqtt_log_decide(&m, &r, HY_W_TOO_SOON));
    r.confirm = false;
    TEST_ASSERT_TRUE(hy_mqtt_log_decide(&m, &r, HY_OK));
    r.has_ts = true;
    r.ts = T_TAKEN;
    TEST_ASSERT_TRUE(hy_mqtt_log_decide(&m, &r, HY_W_TOO_SOON));
    r.has_ts = false;
    m.type = HY_SCHED_FIXED;
    TEST_ASSERT_TRUE(hy_mqtt_log_decide(&m, &r, HY_W_TOO_SOON));
    m.type = HY_SCHED_AS_NEEDED;
    m.min_interval_min = 0;
    TEST_ASSERT_TRUE(hy_mqtt_log_decide(&m, &r, HY_W_TOO_SOON));
}

static void iso(int64_t ts, char out[24])
{
    time_t t = (time_t)ts;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 24, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void tile_a(hy_api_tile_t *t)
{
    memset(t, 0, sizeof *t);
    t->id = 3;
    strcpy(t->code, "A");
    strcpy(t->name, "Antibiotic");
    t->kind = 3;   /* due */
    t->last_taken = T_TAKEN;
}

static void test_mqtt_med_state_json(void)
{
    hy_api_tile_t t;
    tile_a(&t);
    char ts[24], want[256], got[256];
    iso(T_TAKEN, ts);
    TEST_ASSERT_EQUAL_STRING("2025-11-06T07:02:00Z", ts);
    snprintf(want, sizeof want,
             "{\"id\":3,\"code\":\"A\",\"name\":\"Antibiotic\",\"status\":\"due\",\"due\":true,"
             "\"last_taken\":\"%s\",\"next_due\":null,\"pills_left\":null}", ts);
    int n = hy_mqtt_med_state(&t, got, sizeof got);
    TEST_ASSERT_EQUAL_STRING(want, got);
    TEST_ASSERT_EQUAL_INT((int)strlen(want), n);

    t.kind = 5;   /* snoozed */
    hy_mqtt_med_state(&t, got, sizeof got);
    TEST_ASSERT_NOT_NULL(strstr(got, "\"status\":\"snoozed\",\"due\":true"));
    t.kind = 4;   /* late */
    hy_mqtt_med_state(&t, got, sizeof got);
    TEST_ASSERT_NOT_NULL(strstr(got, "\"due\":true"));
    t.kind = 6;   /* missed */
    hy_mqtt_med_state(&t, got, sizeof got);
    TEST_ASSERT_NOT_NULL(strstr(got, "\"status\":\"missed\",\"due\":false"));
    t.kind = 0;
    hy_mqtt_med_state(&t, got, sizeof got);
    TEST_ASSERT_NOT_NULL(strstr(got, "\"status\":\"as_needed\",\"due\":false"));
    t.kind = 2;
    hy_mqtt_med_state(&t, got, sizeof got);
    TEST_ASSERT_NOT_NULL(strstr(got, "\"status\":\"since\",\"due\":false"));

    t.kind = 2;
    t.next_due = T_TAKEN + 3600;
    t.pills = true;
    t.pills_left = 12;
    t.last_taken = 0;
    iso(T_TAKEN + 3600, ts);
    snprintf(want, sizeof want, "\"last_taken\":null,\"next_due\":\"%s\",\"pills_left\":12}", ts);
    hy_mqtt_med_state(&t, got, sizeof got);
    TEST_ASSERT_NOT_NULL(strstr(got, want));

    /* the output parses back, also with a quote in the name */
    strcpy(t.name, "A\"b\\c");
    TEST_ASSERT_TRUE(hy_mqtt_med_state(&t, got, sizeof got) > 0);
    cJSON *j = cJSON_Parse(got);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_STRING("A\"b\\c", cJSON_GetObjectItem(j, "name")->valuestring);
    cJSON_Delete(j);

    TEST_ASSERT_EQUAL_INT(-1, hy_mqtt_med_state(&t, got, 20));
    t.kind = 7;
    TEST_ASSERT_EQUAL_INT(-1, hy_mqtt_med_state(&t, got, sizeof got));
}

static void test_mqtt_device_state_json(void)
{
    hy_mqtt_device_t d;
    memset(&d, 0, sizeof d);
    d.batt_pct = 82; d.usb = true; d.charging = false; d.net_mode = 1; d.rssi = -55;
    strcpy(d.clock_src, "ntp"); d.clock_valid = true; d.adherence = 67; strcpy(d.fw, "1.2.0");
    char b[256];
    int n = hy_mqtt_device_state(&d, b, sizeof b);
    const char *want = "{\"battery\":82,\"charging\":false,\"usb\":true,\"wifi_mode\":\"client\",\"rssi\":-55,"
                       "\"clock_source\":\"ntp\",\"clock_valid\":true,\"adherence_30d\":67,\"fw\":\"1.2.0\"}";
    TEST_ASSERT_EQUAL_STRING(want, b);
    TEST_ASSERT_EQUAL_INT((int)strlen(want), n);

    d.batt_pct = -1; d.rssi = 0; d.clock_src[0] = '\0'; d.clock_valid = false; d.adherence = -1; d.net_mode = 2;
    d.charging = true;
    hy_mqtt_device_state(&d, b, sizeof b);
    want = "{\"battery\":null,\"charging\":true,\"usb\":true,\"wifi_mode\":\"ap\",\"rssi\":null,"
           "\"clock_source\":null,\"clock_valid\":false,\"adherence_30d\":null,\"fw\":\"1.2.0\"}";
    TEST_ASSERT_EQUAL_STRING(want, b);
    d.net_mode = 0;
    hy_mqtt_device_state(&d, b, sizeof b);
    TEST_ASSERT_NOT_NULL(strstr(b, "\"wifi_mode\":\"off\""));
    TEST_ASSERT_EQUAL_INT(-1, hy_mqtt_device_state(&d, b, 30));
}

static void test_adherence_pct(void)
{
    TEST_ASSERT_EQUAL_INT16(-1, hy_mqtt_adherence_pct(0, 0));
    TEST_ASSERT_EQUAL_INT16(67, hy_mqtt_adherence_pct(2, 3));
    TEST_ASSERT_EQUAL_INT16(50, hy_mqtt_adherence_pct(1, 2));
    TEST_ASSERT_EQUAL_INT16(0, hy_mqtt_adherence_pct(0, 5));
    TEST_ASSERT_EQUAL_INT16(100, hy_mqtt_adherence_pct(7, 7));
}

static void test_disc_node_id(void)
{
    char n[16];
    hy_disc_node_id("hygeia-1A2B", n);
    TEST_ASSERT_EQUAL_STRING("hygeia_1a2b", n);
}

static hy_disc_ctx_t ctx(void)
{
    hy_disc_ctx_t c = { "hygeia", "hygeia_1a2b", "1.2.0" };
    return c;
}

static void check_device_block(cJSON *j)
{
    cJSON *av = cJSON_GetObjectItem(j, "availability_topic");
    TEST_ASSERT_EQUAL_STRING("hygeia/status", av->valuestring);
    cJSON *d = cJSON_GetObjectItem(j, "device");
    TEST_ASSERT_EQUAL_STRING("hygeia_1a2b", cJSON_GetArrayItem(cJSON_GetObjectItem(d, "identifiers"), 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("Hygeia", cJSON_GetObjectItem(d, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("Waveshare", cJSON_GetObjectItem(d, "manufacturer")->valuestring);
    TEST_ASSERT_EQUAL_STRING("ESP32-S3-Touch-AMOLED-2.16", cJSON_GetObjectItem(d, "model")->valuestring);
    TEST_ASSERT_EQUAL_STRING("1.2.0", cJSON_GetObjectItem(d, "sw_version")->valuestring);
}

static void test_disc_med_config_last_taken(void)
{
    hy_disc_ctx_t c = ctx();
    char *s = hy_disc_med_config(&c, 3, "Antibiotic", HY_DO_LAST_TAKEN);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NULL(strchr(s, '\n'));
    cJSON *j = cJSON_Parse(s);
    free(s);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_STRING("Antibiotic last taken", cJSON_GetObjectItem(j, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("hygeia_1a2b_med_3_last_taken", cJSON_GetObjectItem(j, "unique_id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("timestamp", cJSON_GetObjectItem(j, "device_class")->valuestring);
    TEST_ASSERT_EQUAL_STRING("hygeia/med/3/state", cJSON_GetObjectItem(j, "state_topic")->valuestring);
    TEST_ASSERT_EQUAL_STRING("{{ value_json.last_taken }}", cJSON_GetObjectItem(j, "value_template")->valuestring);
    check_device_block(j);
    cJSON_Delete(j);

    s = hy_disc_med_config(&c, 3, "Antibiotic", HY_DO_NEXT_DUE);
    j = cJSON_Parse(s);
    free(s);
    TEST_ASSERT_EQUAL_STRING("Antibiotic next due", cJSON_GetObjectItem(j, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("timestamp", cJSON_GetObjectItem(j, "device_class")->valuestring);
    TEST_ASSERT_EQUAL_STRING("{{ value_json.next_due }}", cJSON_GetObjectItem(j, "value_template")->valuestring);
    cJSON_Delete(j);

    s = hy_disc_med_config(&c, 3, "Antibiotic", HY_DO_DUE);
    j = cJSON_Parse(s);
    free(s);
    TEST_ASSERT_EQUAL_STRING("Antibiotic due", cJSON_GetObjectItem(j, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("{{ 'ON' if value_json.due else 'OFF' }}",
                             cJSON_GetObjectItem(j, "value_template")->valuestring);
    cJSON_Delete(j);

    s = hy_disc_med_config(&c, 3, "Antibiotic", HY_DO_PILLS);
    j = cJSON_Parse(s);
    free(s);
    TEST_ASSERT_EQUAL_STRING("Antibiotic pills left", cJSON_GetObjectItem(j, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("pills", cJSON_GetObjectItem(j, "unit_of_measurement")->valuestring);
    TEST_ASSERT_EQUAL_STRING("{{ value_json.pills_left }}", cJSON_GetObjectItem(j, "value_template")->valuestring);
    cJSON_Delete(j);

    TEST_ASSERT_NULL(hy_disc_med_config(&c, 3, "Antibiotic", HY_DO_MED_COUNT));
}

static void test_disc_button_and_event(void)
{
    hy_disc_ctx_t c = ctx();
    char *s = hy_disc_med_config(&c, 3, "Antibiotic", HY_DO_LOG);
    cJSON *j = cJSON_Parse(s);
    free(s);
    TEST_ASSERT_EQUAL_STRING("Antibiotic log", cJSON_GetObjectItem(j, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("hygeia/med/3/log", cJSON_GetObjectItem(j, "command_topic")->valuestring);
    TEST_ASSERT_EQUAL_STRING("PRESS", cJSON_GetObjectItem(j, "payload_press")->valuestring);
    TEST_ASSERT_EQUAL_STRING("hygeia_1a2b_med_3_log", cJSON_GetObjectItem(j, "unique_id")->valuestring);
    check_device_block(j);
    cJSON_Delete(j);

    s = hy_disc_dev_config(&c, HY_DO_EVENT);
    j = cJSON_Parse(s);
    free(s);
    TEST_ASSERT_EQUAL_STRING("hygeia/event", cJSON_GetObjectItem(j, "state_topic")->valuestring);
    TEST_ASSERT_EQUAL_STRING("hygeia_1a2b_event", cJSON_GetObjectItem(j, "unique_id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("{{ {'event_type': value_json.event} | to_json }}",
                             cJSON_GetObjectItem(j, "value_template")->valuestring);
    cJSON *types = cJSON_GetObjectItem(j, "event_types");
    TEST_ASSERT_EQUAL_INT(9, cJSON_GetArraySize(types));
    for (int i = 0; i < 9; i++)
        TEST_ASSERT_EQUAL_STRING(hy_iev_name((hy_iev_t)i), cJSON_GetArrayItem(types, i)->valuestring);
    check_device_block(j);
    cJSON_Delete(j);

    s = hy_disc_dev_config(&c, HY_DO_BATTERY);
    j = cJSON_Parse(s);
    free(s);
    TEST_ASSERT_EQUAL_STRING("battery", cJSON_GetObjectItem(j, "device_class")->valuestring);
    TEST_ASSERT_EQUAL_STRING("%", cJSON_GetObjectItem(j, "unit_of_measurement")->valuestring);
    TEST_ASSERT_EQUAL_STRING("hygeia/device", cJSON_GetObjectItem(j, "state_topic")->valuestring);
    TEST_ASSERT_EQUAL_STRING("{{ value_json.battery }}", cJSON_GetObjectItem(j, "value_template")->valuestring);
    cJSON_Delete(j);

    s = hy_disc_dev_config(&c, HY_DO_CHARGING);
    j = cJSON_Parse(s);
    free(s);
    TEST_ASSERT_EQUAL_STRING("battery_charging", cJSON_GetObjectItem(j, "device_class")->valuestring);
    cJSON_Delete(j);

    s = hy_disc_dev_config(&c, HY_DO_ADHERENCE);
    j = cJSON_Parse(s);
    free(s);
    TEST_ASSERT_EQUAL_STRING("hygeia_1a2b_adherence_30d", cJSON_GetObjectItem(j, "unique_id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("{{ value_json.adherence_30d }}", cJSON_GetObjectItem(j, "value_template")->valuestring);
    cJSON_Delete(j);
    TEST_ASSERT_NULL(hy_disc_dev_config(&c, HY_DO_DEV_COUNT));
}

static void test_disc_topics(void)
{
    char b[HY_MQTT_TOPIC_LEN];
    hy_disc_med_topic("homeassistant", "hygeia_1a2b", 3, HY_DO_LAST_TAKEN, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/hygeia_1a2b/med_3_last_taken/config", b);
    hy_disc_med_topic("homeassistant", "hygeia_1a2b", 3, HY_DO_NEXT_DUE, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/hygeia_1a2b/med_3_next_due/config", b);
    hy_disc_med_topic("homeassistant", "hygeia_1a2b", 3, HY_DO_DUE, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/hygeia_1a2b/med_3_due/config", b);
    hy_disc_med_topic("homeassistant", "hygeia_1a2b", 3, HY_DO_LOG, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("homeassistant/button/hygeia_1a2b/med_3_log/config", b);
    hy_disc_med_topic("homeassistant", "hygeia_1a2b", 3, HY_DO_PILLS, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/hygeia_1a2b/med_3_pills/config", b);
    hy_disc_dev_topic("homeassistant", "hygeia_1a2b", HY_DO_BATTERY, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/hygeia_1a2b/battery/config", b);
    hy_disc_dev_topic("homeassistant", "hygeia_1a2b", HY_DO_CHARGING, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/hygeia_1a2b/charging/config", b);
    hy_disc_dev_topic("homeassistant", "hygeia_1a2b", HY_DO_ADHERENCE, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/hygeia_1a2b/adherence_30d/config", b);
    hy_disc_dev_topic("homeassistant", "hygeia_1a2b", HY_DO_EVENT, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("homeassistant/event/hygeia_1a2b/event/config", b);
    TEST_ASSERT_EQUAL_INT(-1, hy_disc_dev_topic("homeassistant", "hygeia_1a2b", HY_DO_EVENT, b, 10));
}

static void test_disc_topic_parse(void)
{
    char obj[32];
    const char *t = "homeassistant/sensor/hygeia_1a2b/med_3_last_taken/config";
    TEST_ASSERT_TRUE(hy_disc_parse_topic("homeassistant", "hygeia_1a2b", t, strlen(t), obj));
    TEST_ASSERT_EQUAL_STRING("med_3_last_taken", obj);
    t = "homeassistant/event/hygeia_1a2b/event/config";
    TEST_ASSERT_TRUE(hy_disc_parse_topic("homeassistant", "hygeia_1a2b", t, strlen(t), obj));
    TEST_ASSERT_EQUAL_STRING("event", obj);
    static const char *const bad[] = {
        "homeassistant/sensor/hygeia_ffff/battery/config",
        "homeassistant/sensor/hygeia_1a2b/battery",
        "homeassistant/sensor/hygeia_1a2b/battery/state",
        "homeassistant/sensor/hygeia_1a2b/battery/config/x",
        "homeassistant/sensor/hygeia_1a2bc/battery/config",
        "other/sensor/hygeia_1a2b/battery/config",
        "homeassistant/hygeia_1a2b/battery/config",
        "homeassistant/sensor/hygeia_1a2b//config",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        TEST_ASSERT_FALSE_MESSAGE(hy_disc_parse_topic("homeassistant", "hygeia_1a2b", bad[i], strlen(bad[i]), obj), bad[i]);
}

static void test_disc_wanted_archived_and_pills(void)
{
    hy_disc_med_t meds[2] = { { 3, true }, { 5, false } };
    TEST_ASSERT_TRUE(hy_disc_wanted("med_3_pills", true, meds, 2));
    TEST_ASSERT_FALSE(hy_disc_wanted("med_5_pills", true, meds, 2));
    TEST_ASSERT_TRUE(hy_disc_wanted("med_5_due", true, meds, 2));
    TEST_ASSERT_TRUE(hy_disc_wanted("med_3_last_taken", true, meds, 2));
    TEST_ASSERT_TRUE(hy_disc_wanted("med_3_next_due", true, meds, 2));
    TEST_ASSERT_TRUE(hy_disc_wanted("med_3_log", true, meds, 2));
    TEST_ASSERT_FALSE(hy_disc_wanted("med_9_due", true, meds, 2));
    TEST_ASSERT_TRUE(hy_disc_wanted("battery", true, meds, 2));
    TEST_ASSERT_TRUE(hy_disc_wanted("event", true, NULL, 0));
    TEST_ASSERT_FALSE(hy_disc_wanted("med_3_bogus", true, meds, 2));
    TEST_ASSERT_FALSE(hy_disc_wanted("med_03_due", true, meds, 2));
    TEST_ASSERT_FALSE(hy_disc_wanted("unknown", true, meds, 2));
    TEST_ASSERT_FALSE(hy_disc_wanted("med_3", true, meds, 2));
    static const char *const all[] = { "med_3_pills", "med_5_due", "med_3_log", "battery", "charging",
                                       "adherence_30d", "event" };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        TEST_ASSERT_FALSE_MESSAGE(hy_disc_wanted(all[i], false, meds, 2), all[i]);
}

static void test_state_stale(void)
{
    hy_disc_med_t meds[2] = { { 3, true }, { 5, false } };
    const char *t = "hygeia/med/9/state";
    TEST_ASSERT_TRUE(hy_mqtt_state_stale("hygeia", t, strlen(t), meds, 2));
    t = "hygeia/med/3/state";
    TEST_ASSERT_FALSE(hy_mqtt_state_stale("hygeia", t, strlen(t), meds, 2));
    t = "hygeia/device";
    TEST_ASSERT_FALSE(hy_mqtt_state_stale("hygeia", t, strlen(t), meds, 2));
    t = "hygeia/med/9/log";
    TEST_ASSERT_FALSE(hy_mqtt_state_stale("hygeia", t, strlen(t), meds, 2));
    t = "hygeia/med/09/state";
    TEST_ASSERT_FALSE(hy_mqtt_state_stale("hygeia", t, strlen(t), meds, 2));
    t = "other/med/9/state";
    TEST_ASSERT_FALSE(hy_mqtt_state_stale("hygeia", t, strlen(t), meds, 2));
    t = "hygeia/med/9/state";
    TEST_ASSERT_TRUE(hy_mqtt_state_stale("hygeia", t, strlen(t), NULL, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mqtt_topics);
    RUN_TEST(test_mqtt_cmd_parse);
    RUN_TEST(test_mqtt_cmd_retained_ignored);
    RUN_TEST(test_mqtt_log_payload_plain);
    RUN_TEST(test_mqtt_log_payload_json);
    RUN_TEST(test_mqtt_log_payload_ts_window);
    RUN_TEST(test_mqtt_snooze_payload);
    RUN_TEST(test_mqtt_too_soon_refused);
    RUN_TEST(test_mqtt_med_state_json);
    RUN_TEST(test_mqtt_device_state_json);
    RUN_TEST(test_adherence_pct);
    RUN_TEST(test_disc_node_id);
    RUN_TEST(test_disc_med_config_last_taken);
    RUN_TEST(test_disc_button_and_event);
    RUN_TEST(test_disc_topics);
    RUN_TEST(test_disc_topic_parse);
    RUN_TEST(test_disc_wanted_archived_and_pills);
    RUN_TEST(test_state_stale);
    return UNITY_END();
}
