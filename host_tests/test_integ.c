#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "cJSON.h"
#include "hy/api.h"
#include "hy/integ.h"
#include "hy/settings.h"

void setUp(void) {}
void tearDown(void) {}

static hy_integ_t cfg_with_secrets(void)
{
    hy_integ_t c;
    hy_integ_defaults(&c);
    c.mqtt.on = true;
    strcpy(c.mqtt.url, "mqtt://broker.lan");
    strcpy(c.mqtt.user, "hygeia");
    strcpy(c.mqtt.pass, "s3");
    c.n_hooks = 2;
    c.hook[0].on = true;
    strcpy(c.hook[0].url, "https://n8n.lan/webhook/hygeia");
    c.hook[0].events = HY_IE_ALL;
    strcpy(c.hook[0].secret, "example-secret");
    c.hook[1].on = false;
    strcpy(c.hook[1].url, "http://192.168.1.23:8090/hook");
    c.hook[1].events = (1u << HY_IE_MISSED) | (1u << HY_IE_DUE);
    return c;
}

static char *integ_text(const hy_integ_t *c, bool secrets)
{
    cJSON *o = cJSON_CreateObject();
    TEST_ASSERT_TRUE(hy_api_integ_add(o, c, secrets));
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    TEST_ASSERT_NOT_NULL(js);
    return js;
}

/* ---- defaults ---- */

static void test_integ_defaults(void)
{
    hy_integ_t c;
    memset(&c, 0xAB, sizeof c);
    hy_integ_defaults(&c);
    TEST_ASSERT_FALSE(c.mqtt.on);
    TEST_ASSERT_EQUAL_STRING("", c.mqtt.url);
    TEST_ASSERT_EQUAL_STRING("", c.mqtt.user);
    TEST_ASSERT_EQUAL_STRING("", c.mqtt.pass);
    TEST_ASSERT_EQUAL_STRING("hygeia", c.mqtt.base);
    TEST_ASSERT_FALSE(c.mqtt.ha);
    TEST_ASSERT_EQUAL_STRING("homeassistant", c.mqtt.ha_prefix);
    TEST_ASSERT_EQUAL_UINT8(0, c.n_hooks);
    hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_OK, hy_integ_validate(&c, &e));
}

/* ---- url ---- */

static void test_url_parse_mqtt(void)
{
    hy_url_t u;
    TEST_ASSERT_EQUAL(HY_OK, hy_url_parse("mqtt://broker.lan", true, &u));
    TEST_ASSERT_FALSE(u.tls);
    TEST_ASSERT_EQUAL_STRING("broker.lan", u.host);
    TEST_ASSERT_EQUAL_UINT16(1883, u.port);
    TEST_ASSERT_EQUAL_STRING("", u.path);

    TEST_ASSERT_EQUAL(HY_OK, hy_url_parse("mqtts://broker.lan:8884", true, &u));
    TEST_ASSERT_TRUE(u.tls);
    TEST_ASSERT_EQUAL_UINT16(8884, u.port);

    TEST_ASSERT_EQUAL(HY_OK, hy_url_parse("mqtts://broker.lan", true, &u));
    TEST_ASSERT_EQUAL_UINT16(8883, u.port);

    TEST_ASSERT_EQUAL(HY_OK, hy_url_parse("MQTT://192.168.1.23:1883/", true, &u));
    TEST_ASSERT_FALSE(u.tls);
    TEST_ASSERT_EQUAL_STRING("192.168.1.23", u.host);
    TEST_ASSERT_EQUAL_UINT16(1883, u.port);

    char longurl[300];
    memset(longurl, 'a', sizeof longurl);
    memcpy(longurl, "mqtt://", 7);
    longurl[256] = '\0';
    const char *bad[] = { "http://broker.lan", "mqtt://user:pw@broker.lan", "mqtt://broker.lan:0",
                          "mqtt://broker.lan:65536", "mqtt://-bad.lan", "mqtt://bad-", "mqtt://.bad.lan",
                          "mqtt://broker.lan/x", "mqtt://", "mqtt://broker.lan:", "mqtt://broker.lan:12a",
                          "mqtt://broker.lan?x", "mqtt://bro ker.lan", "mqtt://broker.lan#x", "broker.lan", "",
                          "mqtt:/broker.lan", longurl };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        TEST_ASSERT_EQUAL_MESSAGE(HY_E_INVALID, hy_url_parse(bad[i], true, &u), bad[i]);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_url_parse(NULL, true, &u));
    /* the same url as a webhook is a different scheme */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_url_parse("mqtt://broker.lan", false, &u));
}

static void test_url_parse_hook(void)
{
    hy_url_t u;
    TEST_ASSERT_EQUAL(HY_OK, hy_url_parse("https://n8n.lan/webhook/hygeia", false, &u));
    TEST_ASSERT_TRUE(u.tls);
    TEST_ASSERT_EQUAL_STRING("n8n.lan", u.host);
    TEST_ASSERT_EQUAL_UINT16(443, u.port);
    TEST_ASSERT_EQUAL_STRING("/webhook/hygeia", u.path);

    TEST_ASSERT_EQUAL(HY_OK, hy_url_parse("http://192.168.1.23:8090/hook?a=1", false, &u));
    TEST_ASSERT_FALSE(u.tls);
    TEST_ASSERT_EQUAL_UINT16(8090, u.port);
    TEST_ASSERT_EQUAL_STRING("/hook?a=1", u.path);

    TEST_ASSERT_EQUAL(HY_OK, hy_url_parse("HTTP://n8n.lan", false, &u));
    TEST_ASSERT_EQUAL_UINT16(80, u.port);
    TEST_ASSERT_EQUAL_STRING("", u.path);

    const char *bad[] = { "ftp://n8n.lan", "https://n8n.lan/a b", "https://n8n.lan/#x", "https://u@n8n.lan/",
                          "https://", "https://n8n.lan:0/", "https://n8n.lan/\x01", "https://n8n.lan/\x7f",
                          "mqtts://n8n.lan" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        TEST_ASSERT_EQUAL_MESSAGE(HY_E_INVALID, hy_url_parse(bad[i], false, &u), bad[i]);

    char host65[100] = "http://";
    memset(host65 + 7, 'a', 65);
    host65[7 + 65] = '\0';
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_url_parse(host65, false, &u));
    host65[7 + 64] = '\0';
    TEST_ASSERT_EQUAL(HY_OK, hy_url_parse(host65, false, &u));
}

static void test_topic_rules(void)
{
    TEST_ASSERT_EQUAL(HY_OK, hy_integ_check_topic("hygeia"));
    TEST_ASSERT_EQUAL(HY_OK, hy_integ_check_topic("home/hygeia"));
    TEST_ASSERT_EQUAL(HY_OK, hy_integ_check_topic("a_b-c"));
    char t32[34], t33[35];
    memset(t32, 'a', 32);
    t32[32] = '\0';
    memset(t33, 'a', 33);
    t33[33] = '\0';
    TEST_ASSERT_EQUAL(HY_OK, hy_integ_check_topic(t32));
    const char *bad[] = { "", "/x", "x/", "a//b", "a+b", "a#", "$SYS", "a b", "a.b", t33 };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        TEST_ASSERT_EQUAL_MESSAGE(HY_E_INVALID, hy_integ_check_topic(bad[i]), bad[i]);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_integ_check_topic(NULL));
}

static void test_cred_rules(void)
{
    TEST_ASSERT_EQUAL(HY_OK, hy_integ_check_cred(""));
    TEST_ASSERT_EQUAL(HY_OK, hy_integ_check_cred("p\xC3\xA4ss"));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_integ_check_cred("a\nb"));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_integ_check_cred("a\x7f"));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_integ_check_cred("\xC3"));
    char c65[66];
    memset(c65, 'x', 65);
    c65[65] = '\0';
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_integ_check_cred(c65));
    c65[64] = '\0';
    TEST_ASSERT_EQUAL(HY_OK, hy_integ_check_cred(c65));
}

/* ---- json out ---- */

static void test_integ_json_no_secrets(void)
{
    hy_integ_t c = cfg_with_secrets();
    char *js = integ_text(&c, false);
    TEST_ASSERT_NULL(strstr(js, "s3"));
    TEST_ASSERT_NULL(strstr(js, "example-secret"));
    TEST_ASSERT_NULL(strstr(js, "\"password\""));
    TEST_ASSERT_NULL(strstr(js, "\"secret\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"password_set\":true"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"secret_set\":true"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"secret_set\":false"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"events\":[\"missed\",\"due\"]"));
    free(js);

    hy_integ_defaults(&c);
    js = integ_text(&c, false);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"password_set\":false"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"webhooks\":[]"));
    free(js);
}

static void test_integ_json_secrets_export(void)
{
    hy_integ_t c = cfg_with_secrets();
    char *js = integ_text(&c, true);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"password\":\"s3\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"secret\":\"example-secret\""));
    free(js);
}

/* ---- json in ---- */

static void test_integ_put_mqtt_subset(void)
{
    hy_integ_t cur = cfg_with_secrets(), out;
    hy_api_err_t e;
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"mqtt\":{\"on\":true,\"url\":\"mqtts://broker.lan:8884\"}}",
                                                    &cur, &out, &e));
    TEST_ASSERT_TRUE(out.mqtt.on);
    TEST_ASSERT_EQUAL_STRING("mqtts://broker.lan:8884", out.mqtt.url);
    TEST_ASSERT_EQUAL_STRING("hygeia", out.mqtt.user);
    TEST_ASSERT_EQUAL_STRING("s3", out.mqtt.pass);
    TEST_ASSERT_EQUAL_STRING("hygeia", out.mqtt.base);
    TEST_ASSERT_EQUAL_UINT8(2, out.n_hooks);
    TEST_ASSERT_EQUAL_STRING("example-secret", out.hook[0].secret);

    /* other members, null as absent, password_set ignored */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"mqtt\":{\"base\":\"home/pills\",\"ha\":true,"
                                                    "\"ha_prefix\":\"ha\",\"user\":null,\"password_set\":false}}",
                                                    &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("home/pills", out.mqtt.base);
    TEST_ASSERT_TRUE(out.mqtt.ha);
    TEST_ASSERT_EQUAL_STRING("ha", out.mqtt.ha_prefix);
    TEST_ASSERT_EQUAL_STRING("hygeia", out.mqtt.user);
    TEST_ASSERT_EQUAL_STRING("s3", out.mqtt.pass);

    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"mqtt\":{\"password\":\"\"}}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("", out.mqtt.pass);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"mqtt\":{\"password\":\"new\"}}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("new", out.mqtt.pass);

    /* no integration members at all: unchanged */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"sound\":true}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_MEMORY(&cur, &out, sizeof cur);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"mqtt\":null,\"webhooks\":null}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_MEMORY(&cur, &out, sizeof cur);
}

static void test_integ_put_mqtt_on_needs_url(void)
{
    hy_integ_t cur, out;
    hy_integ_defaults(&cur);
    hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"mqtt\":{\"on\":true}}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("mqtt.url", e.field);
    TEST_ASSERT_EQUAL(400, e.http);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"mqtt\":{\"on\":true,\"url\":\"\"}}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("mqtt.url", e.field);
    /* off may keep a bad-free url or none */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"mqtt\":{\"url\":\"mqtt://broker.lan\"}}", &cur, &out, &e));
    TEST_ASSERT_FALSE(out.mqtt.on);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"mqtt\":{\"url\":\"http://broker.lan\"}}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("mqtt.url", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"mqtt\":{\"base\":\"/x\"}}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("mqtt.base", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"mqtt\":{\"ha_prefix\":\"a//b\"}}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("mqtt.ha_prefix", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"mqtt\":{\"user\":\"a\\nb\"}}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("mqtt.user", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"mqtt\":{\"password\":\"a\\u0001\"}}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("mqtt.password", e.field);
}

static void test_integ_put_hooks_replace(void)
{
    hy_integ_t cur = cfg_with_secrets(), out;
    hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/a\"},"
                                                    "{\"url\":\"http://n8n.lan/b\",\"on\":false}]}",
                                                    &cur, &out, &e));
    TEST_ASSERT_EQUAL_UINT8(2, out.n_hooks);
    TEST_ASSERT_TRUE(out.hook[0].on);
    TEST_ASSERT_EQUAL_UINT16(HY_IE_ALL, out.hook[0].events);
    TEST_ASSERT_FALSE(out.hook[1].on);

    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/a\","
                                                    "\"events\":[\"taken\",\"missed\"]}]}",
                                                    &cur, &out, &e));
    TEST_ASSERT_EQUAL_UINT8(1, out.n_hooks);
    TEST_ASSERT_EQUAL_UINT16((1u << 0) | (1u << 2), out.hook[0].events);
    /* mqtt untouched by a webhooks-only body */
    TEST_ASSERT_EQUAL_STRING("s3", out.mqtt.pass);

    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[]}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_UINT8(0, out.n_hooks);
    /* empty events list is allowed: the hook receives nothing */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/a\",\"events\":[]}]}",
                                                    &cur, &out, &e));
    TEST_ASSERT_EQUAL_UINT16(0, out.hook[0].events);
    /* four hooks fit */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/1\"},"
                                                    "{\"url\":\"https://n8n.lan/2\"},{\"url\":\"https://n8n.lan/3\"},"
                                                    "{\"url\":\"https://n8n.lan/4\"}]}",
                                                    &cur, &out, &e));
    TEST_ASSERT_EQUAL_UINT8(4, out.n_hooks);
}

/* A removed webhook leaves an empty, off slot: the hooks after it keep their index and their secret. */
static void test_integ_put_hook_blank_slot(void)
{
    hy_integ_t cur, out;
    hy_api_err_t e;
    hy_integ_defaults(&cur);
    cur.n_hooks = 3;
    for (int i = 0; i < 3; i++) {
        cur.hook[i].on = true;
        cur.hook[i].events = HY_IE_ALL;
        snprintf(cur.hook[i].url, sizeof cur.hook[i].url, "https://n8n.lan/%d", i);
        snprintf(cur.hook[i].secret, sizeof cur.hook[i].secret, "s%d", i);
    }
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/0\"},"
                                                    "{\"url\":\"\",\"on\":false},{\"url\":\"https://n8n.lan/2\"}]}",
                                                    &cur, &out, &e));
    TEST_ASSERT_EQUAL_UINT8(3, out.n_hooks);
    TEST_ASSERT_EQUAL_STRING("s0", out.hook[0].secret);
    TEST_ASSERT_EQUAL_STRING("", out.hook[1].url);
    TEST_ASSERT_FALSE(out.hook[1].on);
    TEST_ASSERT_EQUAL_STRING("", out.hook[1].secret);   /* the slot's secret goes with the hook */
    TEST_ASSERT_EQUAL_UINT16(0, out.hook[1].events);
    TEST_ASSERT_EQUAL_STRING("https://n8n.lan/2", out.hook[2].url);
    TEST_ASSERT_EQUAL_STRING("s2", out.hook[2].secret);
    TEST_ASSERT_EQUAL(HY_OK, hy_integ_validate(&out, &e));
    /* a new hook taking the blank slot starts without a secret; the others keep theirs */
    hy_integ_t again;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/0\"},"
                                                    "{\"url\":\"https://n8n.lan/new\"},{\"url\":\"https://n8n.lan/2\"}]}",
                                                    &out, &again, &e));
    TEST_ASSERT_TRUE(again.hook[1].on);
    TEST_ASSERT_EQUAL_STRING("", again.hook[1].secret);
    TEST_ASSERT_EQUAL_STRING("s2", again.hook[2].secret);
    /* two blank slots do not collide as duplicates */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"\"},{\"url\":\"\"},"
                                                    "{\"url\":\"https://n8n.lan/2\"}]}", &out, &again, &e));
    /* an empty url makes a blank slot whatever else the item says; a stored slot that is on with no url is refused */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"\",\"on\":true,\"secret\":\"x\"}]}",
                                                    &out, &again, &e));
    TEST_ASSERT_FALSE(again.hook[0].on);
    TEST_ASSERT_EQUAL_STRING("", again.hook[0].secret);
    again.hook[0].on = true;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_integ_validate(&again, &e));
    TEST_ASSERT_EQUAL_STRING("webhooks.0.url", e.field);
    /* a duplicate url is still refused */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/x\"},"
                                                           "{\"url\":\"https://N8N.lan/x\"}]}", &out, &again, &e));
}

static void test_integ_put_hook_secret_stays_with_slot(void)
{
    hy_integ_t cur, out;
    hy_integ_defaults(&cur);
    cur.n_hooks = 2;
    cur.hook[0].on = cur.hook[1].on = true;
    cur.hook[0].events = cur.hook[1].events = HY_IE_ALL;
    strcpy(cur.hook[0].url, "https://n8n.lan/one");
    strcpy(cur.hook[0].secret, "s1");
    strcpy(cur.hook[1].url, "https://n8n.lan/two");
    strcpy(cur.hook[1].secret, "s2");
    hy_api_err_t e;

    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/one-edited\"},"
                                                    "{\"url\":\"https://n8n.lan/two\",\"secret_set\":false}]}",
                                                    &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("s1", out.hook[0].secret);
    TEST_ASSERT_EQUAL_STRING("s2", out.hook[1].secret);
    TEST_ASSERT_EQUAL_STRING("https://n8n.lan/one-edited", out.hook[0].url);

    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/one\"},"
                                                    "{\"url\":\"https://n8n.lan/two\"},"
                                                    "{\"url\":\"https://n8n.lan/three\"}]}",
                                                    &cur, &out, &e));
    TEST_ASSERT_EQUAL_UINT8(3, out.n_hooks);
    TEST_ASSERT_EQUAL_STRING("s1", out.hook[0].secret);
    TEST_ASSERT_EQUAL_STRING("s2", out.hook[1].secret);
    TEST_ASSERT_EQUAL_STRING("", out.hook[2].secret);

    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/one\",\"secret\":\"x\"},"
                                                    "{\"url\":\"https://n8n.lan/two\",\"secret\":\"\"}]}",
                                                    &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("x", out.hook[0].secret);
    TEST_ASSERT_EQUAL_STRING("", out.hook[1].secret);

    /* the list shrinks: slot 0 keeps its secret, the removed slot's secret is gone */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/two\"}]}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_UINT8(1, out.n_hooks);
    TEST_ASSERT_EQUAL_STRING("s1", out.hook[0].secret);
}

static void test_integ_put_bad(void)
{
    hy_integ_t cur = cfg_with_secrets(), out, before;
    hy_api_err_t e;
    memset(&out, 0x5A, sizeof out);
    before = out;

    TEST_ASSERT_EQUAL(HY_E_INVALID,
                      hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/1\"},"
                                             "{\"url\":\"https://n8n.lan/2\"},{\"url\":\"https://n8n.lan/3\"},"
                                             "{\"url\":\"https://n8n.lan/4\"},{\"url\":\"https://n8n.lan/5\"}]}",
                                             &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("webhooks", e.field);
    TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof out);

    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/a\","
                                                           "\"events\":[\"door\"]}]}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("webhooks.0.events", e.field);
    TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof out);

    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/a\"},"
                                                           "{\"url\":\"HTTPS://N8N.LAN/a\"}]}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("webhooks.1.url", e.field);
    TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof out);

    /* path case is part of the identity */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/a\"},"
                                                    "{\"url\":\"https://n8n.lan/A\"}]}", &cur, &out, &e));

    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"mqtt\":{\"on\":\"yes\"}}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("mqtt.on", e.field);

    memset(&out, 0x5A, sizeof out);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"webhooks\":[{\"on\":true}]}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("webhooks.0.url", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"webhooks\":[{\"url\":5}]}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("webhooks.0.url", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"webhooks\":{}}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("webhooks", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"webhooks\":[3]}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("webhooks.0", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/a\",\"on\":1}]}",
                                                           &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("webhooks.0.on", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/a\","
                                                           "\"events\":\"taken\"}]}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("webhooks.0.events", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/a\","
                                                           "\"secret\":7}]}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("webhooks.0.secret", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"webhooks\":[{\"url\":\"https://n8n.lan/a\","
                                                           "\"secret\":\"a\\u0007\"}]}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("webhooks.0.secret", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("{\"mqtt\":5}", &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("mqtt", e.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("not json", &cur, &out, &e));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json("[]", &cur, &out, &e));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json(NULL, &cur, &out, &e));
    TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof out);

    /* a url longer than the field is refused, not cut */
    char big[400];
    strcpy(big, "{\"mqtt\":{\"url\":\"mqtt://");
    size_t n = strlen(big);
    memset(big + n, 'a', 300);
    strcpy(big + n + 300, "\"}}");
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_integ_from_json(big, &cur, &out, &e));
    TEST_ASSERT_EQUAL_STRING("mqtt.url", e.field);
}

/* ---- event names ---- */

static void test_iev_names_and_bus(void)
{
    static const char *const N[] = { "taken", "skipped", "missed", "due", "snoozed", "undone", "battery_low",
                                     "battery_critical", "clock_invalid" };
    for (int i = 0; i < HY_IE_COUNT; i++) {
        TEST_ASSERT_EQUAL_STRING(N[i], hy_iev_name((hy_iev_t)i));
        hy_iev_t t = HY_IE_COUNT;
        TEST_ASSERT_TRUE(hy_iev_from_name(N[i], &t));
        TEST_ASSERT_EQUAL(i, t);
    }
    TEST_ASSERT_NULL(hy_iev_name(HY_IE_COUNT));
    TEST_ASSERT_NULL(hy_iev_name((hy_iev_t)-1));
    hy_iev_t t;
    TEST_ASSERT_FALSE(hy_iev_from_name("door", &t));
    TEST_ASSERT_FALSE(hy_iev_from_name("", &t));
    TEST_ASSERT_FALSE(hy_iev_from_name(NULL, &t));
    TEST_ASSERT_FALSE(hy_iev_from_name("Taken", &t));

    TEST_ASSERT_TRUE(hy_iev_from_bus(HY_EV_TAKEN, &t));
    TEST_ASSERT_EQUAL(HY_IE_TAKEN, t);
    TEST_ASSERT_TRUE(hy_iev_from_bus(HY_EV_UNDONE, &t));
    TEST_ASSERT_EQUAL(HY_IE_UNDONE, t);
    TEST_ASSERT_TRUE(hy_iev_from_bus(HY_EV_SKIPPED, &t));
    TEST_ASSERT_EQUAL(HY_IE_SKIPPED, t);
    TEST_ASSERT_TRUE(hy_iev_from_bus(HY_EV_MISSED, &t));
    TEST_ASSERT_EQUAL(HY_IE_MISSED, t);
    TEST_ASSERT_TRUE(hy_iev_from_bus(HY_EV_DUE, &t));
    TEST_ASSERT_EQUAL(HY_IE_DUE, t);
    TEST_ASSERT_TRUE(hy_iev_from_bus(HY_EV_SNOOZED, &t));
    TEST_ASSERT_EQUAL(HY_IE_SNOOZED, t);
    TEST_ASSERT_TRUE(hy_iev_from_bus(HY_EV_BATTERY_LOW, &t));
    TEST_ASSERT_EQUAL(HY_IE_BATTERY_LOW, t);
    TEST_ASSERT_TRUE(hy_iev_from_bus(HY_EV_BATTERY_CRITICAL, &t));
    TEST_ASSERT_EQUAL(HY_IE_BATTERY_CRITICAL, t);
    TEST_ASSERT_TRUE(hy_iev_from_bus(HY_EV_CLOCK_INVALID, &t));
    TEST_ASSERT_EQUAL(HY_IE_CLOCK_INVALID, t);
    TEST_ASSERT_FALSE(hy_iev_from_bus(HY_EV_LOG_EDITED, &t));
    TEST_ASSERT_FALSE(hy_iev_from_bus(HY_EV_MEDS_CHANGED, &t));
    TEST_ASSERT_FALSE(hy_iev_from_bus(HY_EV_UNTRACKED, &t));
}

/* ---- status ---- */

static void test_integ_status_json(void)
{
    hy_integ_status_t s;
    memset(&s, 0, sizeof s);
    s.mqtt = HY_MQS_CONNECTED;
    s.mqtt_since = 1730000000;
    s.ha = true;
    s.mqtt_pending = 2;
    s.n_hooks = 2;
    s.hook[0].at = 1730000100;
    s.hook[0].http = 200;
    s.hook[0].pending = 0;
    s.hook[0].retry_in_s = -1;
    s.hook[1].at = 1730000200;
    s.hook[1].http = 0;
    s.hook[1].err = HY_HE_AUTH;
    s.hook[1].pending = 3;
    s.hook[1].retry_in_s = 60;
    s.hook[1].paused = true;
    s.pending = 5;
    s.dropped = 1;
    s.expired = 2;
    s.rejected = 3;
    s.lost = 4;
    s.outbox_err = HY_HE_STORAGE;
    s.test_running = true;
    s.test_index = 1;
    s.override = true;
    char *js = hy_api_integ_status_json(&s);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_EQUAL_STRING(
        "{\"mqtt\":{\"state\":\"connected\",\"error\":null,\"since\":1730000000,\"ha\":true,\"pending\":2},"
        "\"webhooks\":[{\"index\":0,\"last\":{\"at\":1730000100,\"status\":200,\"error\":null},\"pending\":0,"
        "\"retry_in_s\":null,\"paused\":false},"
        "{\"index\":1,\"last\":{\"at\":1730000200,\"status\":null,\"error\":\"auth\"},\"pending\":3,"
        "\"retry_in_s\":60,\"paused\":true}],"
        "\"outbox\":{\"pending\":5,\"capacity\":1000,\"dropped\":1,\"expired\":2,\"rejected\":3,\"lost\":4,"
        "\"error\":\"storage\"},"
        "\"test\":{\"running\":true,\"index\":1,\"status\":null,\"error\":null}}",
        js);
    TEST_ASSERT_NULL(strstr(js, "override"));
    free(js);

    memset(&s, 0, sizeof s);
    s.test_index = -1;
    js = hy_api_integ_status_json(&s);
    TEST_ASSERT_EQUAL_STRING(
        "{\"mqtt\":{\"state\":\"off\",\"error\":null,\"since\":null,\"ha\":false,\"pending\":0},"
        "\"webhooks\":[],"
        "\"outbox\":{\"pending\":0,\"capacity\":1000,\"dropped\":0,\"expired\":0,\"rejected\":0,\"lost\":0,"
        "\"error\":null},"
        "\"test\":{\"running\":false,\"index\":null,\"status\":null,\"error\":null}}",
        js);
    free(js);

    s.mqtt = HY_MQS_ERROR;
    s.mqtt_err = HY_HE_TLS;
    s.test_index = 0;
    s.test_http = 404;
    s.test_err = HY_HE_HTTP;
    js = hy_api_integ_status_json(&s);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"state\":\"error\",\"error\":\"tls\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"test\":{\"running\":false,\"index\":0,\"status\":404,\"error\":\"http\"}"));
    free(js);

    TEST_ASSERT_NULL(hy_api_integ_status_json(NULL));
    TEST_ASSERT_EQUAL_STRING("", hy_integ_err_name(HY_HE_NONE));
    TEST_ASSERT_EQUAL_STRING("busy", hy_integ_err_name(HY_HE_BUSY));
    TEST_ASSERT_NULL(hy_integ_err_name(HY_HE_COUNT));
}

/* ---- settings with integrations ---- */

static void test_settings_full_json(void)
{
    hy_settings_t st;
    hy_settings_defaults(&st);
    st.volume = 33;
    st.snooze_min = 20;
    hy_integ_t c = cfg_with_secrets();

    char *js = hy_api_settings_full_json(&st, &c, false);
    TEST_ASSERT_NOT_NULL(js);
    char *plain = hy_api_settings_json(&st);
    TEST_ASSERT_NOT_NULL(plain);
    /* the settings members come first, unchanged, then mqtt and webhooks */
    size_t n = strlen(plain) - 1;   /* without the closing brace */
    TEST_ASSERT_EQUAL_INT(0, strncmp(js, plain, n));
    TEST_ASSERT_EQUAL_CHAR(',', js[n]);
    TEST_ASSERT_NOT_NULL(strstr(js, ",\"mqtt\":{"));
    TEST_ASSERT_NOT_NULL(strstr(js, ",\"webhooks\":["));
    TEST_ASSERT_NULL(strstr(js, "s3"));
    TEST_ASSERT_NULL(strstr(js, "example-secret"));

    hy_settings_t back;
    hy_api_err_t e;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json(js, &st, &back, &e));
    TEST_ASSERT_EQUAL_MEMORY(&st, &back, sizeof st);
    hy_settings_t other;
    hy_settings_defaults(&other);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json(js, &other, &back, &e));
    TEST_ASSERT_EQUAL_UINT8(33, back.volume);
    TEST_ASSERT_EQUAL_UINT16(20, back.snooze_min);

    /* and the integration half reads back through the merge, secrets kept from the stored copy */
    hy_integ_t again;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_integ_from_json(js, &c, &again, &e));
    TEST_ASSERT_EQUAL_MEMORY(&c, &again, sizeof c);
    free(js);
    free(plain);

    js = hy_api_settings_full_json(&st, &c, true);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"password\":\"s3\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"secret\":\"example-secret\""));
    free(js);

    TEST_ASSERT_NULL(hy_api_settings_full_json(&st, NULL, false));
    TEST_ASSERT_NULL(hy_api_settings_full_json(NULL, &c, false));
}

/* ---- secrets in freed heap ---- */

static const char *s_needle;
static unsigned s_leaks;
static bool s_zero;   /* models the device's json_free, which zeroes every block before freeing it */

static void *track_malloc(size_t n)
{
    size_t *p = malloc(n + 16);
    if (!p) return NULL;
    p[0] = n;
    return (char *)p + 16;
}

static void track_free(void *q)
{
    if (!q) return;
    size_t *p = (size_t *)((char *)q - 16);
    size_t n = p[0], k = strlen(s_needle);
    if (s_zero) memset(q, 0, n);
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp((char *)q + i, s_needle, k) == 0) s_leaks++;
    free(p);
}

static unsigned leaks_of_mode(const char *body, const char *needle, bool settings_parse, bool zero)
{
    hy_settings_t cur, next;
    hy_integ_t ic = cfg_with_secrets(), io;
    hy_api_err_t e;
    cJSON_Hooks h = { track_malloc, track_free };
    hy_settings_defaults(&cur);
    s_needle = needle;
    s_leaks = 0;
    s_zero = zero;
    cJSON_InitHooks(&h);
    if (settings_parse) (void)hy_api_settings_from_json(body, &cur, &next, &e);
    else (void)hy_api_integ_from_json(body, &ic, &io, &e);
    cJSON_InitHooks(NULL);
    return s_leaks;
}

static unsigned leaks_of(const char *body, const char *needle, bool settings_parse)
{
    return leaks_of_mode(body, needle, settings_parse, false);
}

static void test_secrets_wiped_before_free(void)
{
    const char *body = "{\"volume\":50,\"mqtt\":{\"on\":true,\"url\":\"mqtt://broker.lan\",\"password\":\"example-secret\"},"
                       "\"webhooks\":[{\"url\":\"https://n8n.lan/webhook/hygeia\",\"secret\":\"example-secret-2\"}]}";
    /* the settings parse builds its own tree from the same body */
    TEST_ASSERT_EQUAL_UINT(0, leaks_of(body, "example-secret", true));
    TEST_ASSERT_EQUAL_UINT(0, leaks_of(body, "example-secret-2", true));
    TEST_ASSERT_EQUAL_UINT(0, leaks_of(body, "example-secret", false));
    TEST_ASSERT_EQUAL_UINT(0, leaks_of(body, "example-secret-2", false));
    /* a body that fails to merge still wipes */
    const char *bad = "{\"volume\":\"x\",\"mqtt\":{\"password\":\"example-secret\"}}";
    TEST_ASSERT_EQUAL_UINT(0, leaks_of(bad, "example-secret", true));
    /* repeated secret keys and repeated mqtt / webhooks members: every copy is wiped by the tree walk alone */
    const char *dup = "{\"mqtt\":{\"password\":\"example-secret\",\"password\":\"example-secret-2\"},"
                      "\"mqtt\":{\"password\":\"example-secret-3\"},"
                      "\"webhooks\":[{\"url\":\"https://n8n.lan/h\",\"secret\":\"example-secret-4\","
                      "\"secret\":\"example-secret-5\"}]}";
    /* every value starts with "example-secret", so one needle finds any copy left behind */
    TEST_ASSERT_EQUAL_UINT(0, leaks_of(dup, "example-secret", true));
    TEST_ASSERT_EQUAL_UINT(0, leaks_of(dup, "example-secret", false));
    /* malformed JSON: cJSON frees the partial tree itself before any wipe can see it. Only the device's zeroing
     * free covers that path, so this case runs with the hook that models it */
    const char *torn = "{\"mqtt\":{\"password\":\"example-secret\"},\"webhooks\":[{\"secret\":\"example-secret-2\"";
    TEST_ASSERT_EQUAL_UINT(0, leaks_of_mode(torn, "example-secret", true, true));
    TEST_ASSERT_EQUAL_UINT(0, leaks_of_mode(torn, "example-secret", false, true));
    /* control: a plain delete of the same body does leave the needle in the freed block */
    cJSON_Hooks h = { track_malloc, track_free };
    s_needle = "example-secret";
    s_zero = false;
    s_leaks = 0;
    cJSON_InitHooks(&h);
    cJSON_Delete(cJSON_Parse(body));
    cJSON_InitHooks(NULL);
    TEST_ASSERT_TRUE(s_leaks > 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_integ_defaults);
    RUN_TEST(test_secrets_wiped_before_free);
    RUN_TEST(test_url_parse_mqtt);
    RUN_TEST(test_url_parse_hook);
    RUN_TEST(test_topic_rules);
    RUN_TEST(test_cred_rules);
    RUN_TEST(test_integ_json_no_secrets);
    RUN_TEST(test_integ_json_secrets_export);
    RUN_TEST(test_integ_put_mqtt_subset);
    RUN_TEST(test_integ_put_mqtt_on_needs_url);
    RUN_TEST(test_integ_put_hooks_replace);
    RUN_TEST(test_integ_put_hook_blank_slot);
    RUN_TEST(test_integ_put_hook_secret_stays_with_slot);
    RUN_TEST(test_integ_put_bad);
    RUN_TEST(test_iev_names_and_bus);
    RUN_TEST(test_integ_status_json);
    RUN_TEST(test_settings_full_json);
    return UNITY_END();
}
