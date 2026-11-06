#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "cJSON.h"
#include "hy/codec.h"
#include "hy/hook.h"

void setUp(void) {}
void tearDown(void) {}

#define TZ_POSIX "CET-1CEST,M3.5.0,M10.5.0/3"
#define T_TAKEN  1762412520LL

static void base_ev(hy_hook_ev_t *e, hy_iev_t type)
{
    memset(e, 0, sizeof *e);
    e->type = type;
    e->ts = T_TAKEN;
    strcpy(e->zone, "Europe/Zurich");
    strcpy(e->device, "hygeia-1A2B");
    e->batt_pct = 82;
}

static void med_antibiotic(hy_hook_ev_t *e)
{
    e->has_med = true; e->med_id = 1;
    strcpy(e->code, "A"); strcpy(e->name, "Antibiotic"); strcpy(e->dose, "1 tab");
}

static void taken_log(hy_log_t *l)
{
    memset(l, 0, sizeof *l);
    l->id = 42; l->ts = T_TAKEN; strcpy(l->tz, TZ_POSIX); l->med_id = 1;
    l->status = HY_TAKEN; l->slot = 1762412400LL; l->delta_min = 2; l->source = HY_SRC_SCREEN;
}

static void test_hook_payload_taken(void)
{
    hy_hook_ev_t e;
    base_ev(&e, HY_IE_TAKEN);
    med_antibiotic(&e);
    e.slot = 1762412400LL;
    e.has_log = true;
    taken_log(&e.log);
    char line[400];
    TEST_ASSERT_TRUE(hy_log_to_jsonl(&e.log, line, sizeof line) > 0);

    char exp[900];
    snprintf(exp, sizeof exp,
             "{\"id\":7,\"event\":\"taken\",\"ts\":1762412520,\"tz\":\"Europe/Zurich\",\"device\":\"hygeia-1A2B\","
             "\"medicine\":{\"id\":1,\"code\":\"A\",\"name\":\"Antibiotic\",\"dose\":\"1 tab\"},"
             "\"slot\":1762412400,\"log\":%s,\"battery\":82}", line);
    char buf[HY_HOOK_PAYLOAD_MAX];
    int n = hy_hook_payload(&e, 7, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(exp), n);
    TEST_ASSERT_EQUAL_STRING(exp, buf);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"log\":{\"id\":42,\"ts\":1762412520,\"tz\":\"" TZ_POSIX "\""));

    cJSON *j = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(j);
    cJSON_Delete(j);
}

static void test_hook_payload_due_no_log(void)
{
    hy_hook_ev_t e;
    base_ev(&e, HY_IE_DUE);
    med_antibiotic(&e);
    e.slot = 1762412400LL;
    char buf[HY_HOOK_PAYLOAD_MAX];
    int n = hy_hook_payload(&e, 8, buf, sizeof buf);
    const char *exp =
        "{\"id\":8,\"event\":\"due\",\"ts\":1762412520,\"tz\":\"Europe/Zurich\",\"device\":\"hygeia-1A2B\","
        "\"medicine\":{\"id\":1,\"code\":\"A\",\"name\":\"Antibiotic\",\"dose\":\"1 tab\"},"
        "\"slot\":1762412400,\"log\":null,\"battery\":82}";
    TEST_ASSERT_EQUAL_INT((int)strlen(exp), n);
    TEST_ASSERT_EQUAL_STRING(exp, buf);
}

static void test_hook_payload_battery_low(void)
{
    hy_hook_ev_t e;
    base_ev(&e, HY_IE_BATTERY_LOW);
    e.value = 14;
    e.batt_pct = 15;
    char buf[HY_HOOK_PAYLOAD_MAX];
    int n = hy_hook_payload(&e, 9, buf, sizeof buf);
    const char *exp =
        "{\"id\":9,\"event\":\"battery_low\",\"ts\":1762412520,\"tz\":\"Europe/Zurich\",\"device\":\"hygeia-1A2B\","
        "\"medicine\":null,\"slot\":null,\"log\":null,\"battery\":14}";
    TEST_ASSERT_EQUAL_INT((int)strlen(exp), n);
    TEST_ASSERT_EQUAL_STRING(exp, buf);
}

static void test_hook_payload_undone_id_only(void)
{
    hy_hook_ev_t e;
    base_ev(&e, HY_IE_UNDONE);
    med_antibiotic(&e);
    e.slot = 1762412400LL;
    e.log_id = 42;
    char buf[HY_HOOK_PAYLOAD_MAX];
    int n = hy_hook_payload(&e, 10, buf, sizeof buf);
    const char *exp =
        "{\"id\":10,\"event\":\"undone\",\"ts\":1762412520,\"tz\":\"Europe/Zurich\",\"device\":\"hygeia-1A2B\","
        "\"medicine\":{\"id\":1,\"code\":\"A\",\"name\":\"Antibiotic\",\"dose\":\"1 tab\"},"
        "\"slot\":1762412400,\"log\":{\"id\":42},\"battery\":82}";
    TEST_ASSERT_EQUAL_INT((int)strlen(exp), n);
    TEST_ASSERT_EQUAL_STRING(exp, buf);
}

static void test_hook_payload_clock_invalid(void)
{
    hy_hook_ev_t e;
    base_ev(&e, HY_IE_CLOCK_INVALID);
    e.ts = 0;
    e.zone[0] = '\0';
    e.batt_pct = -1;
    char buf[HY_HOOK_PAYLOAD_MAX];
    int n = hy_hook_payload(&e, 1, buf, sizeof buf);
    const char *exp =
        "{\"id\":1,\"event\":\"clock_invalid\",\"ts\":null,\"tz\":null,\"device\":\"hygeia-1A2B\","
        "\"medicine\":null,\"slot\":null,\"log\":null,\"battery\":null}";
    TEST_ASSERT_EQUAL_INT((int)strlen(exp), n);
    TEST_ASSERT_EQUAL_STRING(exp, buf);
}

static void test_hook_payload_escapes(void)
{
    hy_hook_ev_t e;
    base_ev(&e, HY_IE_TAKEN);
    med_antibiotic(&e);
    const char *name = "Vitamin \"D\" a\\b \xc3\xa9 \t\x01";
    strcpy(e.name, name);
    strcpy(e.dose, "1\n2");
    char buf[HY_HOOK_PAYLOAD_MAX];
    int n = hy_hook_payload(&e, 3, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    cJSON *j = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *med = cJSON_GetObjectItem(j, "medicine");
    TEST_ASSERT_EQUAL_STRING(name, cJSON_GetObjectItem(med, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("1\n2", cJSON_GetObjectItem(med, "dose")->valuestring);
    cJSON_Delete(j);

    /* Same bytes as cJSON prints. */
    cJSON *s = cJSON_CreateString(name);
    char *printed = cJSON_PrintUnformatted(s);
    char want[80];
    snprintf(want, sizeof want, "\"name\":%s", printed);
    TEST_ASSERT_NOT_NULL(strstr(buf, want));
    cJSON_free(printed);
    cJSON_Delete(s);
}

static void test_hook_payload_too_small(void)
{
    hy_hook_ev_t e;
    base_ev(&e, HY_IE_TAKEN);
    med_antibiotic(&e);
    char buf[64];
    TEST_ASSERT_EQUAL_INT(-1, hy_hook_payload(&e, 7, buf, sizeof buf));
    char full[HY_HOOK_PAYLOAD_MAX];
    int n = hy_hook_payload(&e, 7, full, sizeof full);
    TEST_ASSERT_TRUE(n > 0);
    char exact[1024];
    TEST_ASSERT_EQUAL_INT(n, hy_hook_payload(&e, 7, exact, (size_t)n + 1));
    TEST_ASSERT_EQUAL_INT(-1, hy_hook_payload(&e, 7, exact, (size_t)n));
    TEST_ASSERT_EQUAL_INT(-1, hy_hook_payload(&e, 7, exact, 0));
    e.type = HY_IE_COUNT;
    TEST_ASSERT_EQUAL_INT(-1, hy_hook_payload(&e, 7, full, sizeof full));
}

static void test_hook_payload_log_too_small(void)
{
    hy_hook_ev_t e;
    base_ev(&e, HY_IE_TAKEN);
    med_antibiotic(&e);
    e.has_log = true;
    taken_log(&e.log);
    char full[HY_HOOK_PAYLOAD_MAX];
    int n = hy_hook_payload(&e, 7, full, sizeof full);
    TEST_ASSERT_TRUE(n > 0);
    for (int cut = 1; cut <= 30; cut++) {
        char small[HY_HOOK_PAYLOAD_MAX];
        TEST_ASSERT_EQUAL_INT(-1, hy_hook_payload(&e, 7, small, (size_t)n + 1 - (size_t)cut));
    }
}

static void test_hook_test_payload(void)
{
    char buf[HY_HOOK_PAYLOAD_MAX];
    int n = hy_hook_test_payload(T_TAKEN, "Europe/Zurich", "hygeia-1A2B", 82, buf, sizeof buf);
    const char *exp =
        "{\"id\":0,\"event\":\"test\",\"ts\":1762412520,\"tz\":\"Europe/Zurich\",\"device\":\"hygeia-1A2B\","
        "\"medicine\":null,\"slot\":null,\"log\":null,\"battery\":82}";
    TEST_ASSERT_EQUAL_INT((int)strlen(exp), n);
    TEST_ASSERT_EQUAL_STRING(exp, buf);
    n = hy_hook_test_payload(0, "", "hygeia-1A2B", -1, buf, sizeof buf);
    exp = "{\"id\":0,\"event\":\"test\",\"ts\":null,\"tz\":null,\"device\":\"hygeia-1A2B\","
          "\"medicine\":null,\"slot\":null,\"log\":null,\"battery\":null}";
    TEST_ASSERT_EQUAL_INT((int)strlen(exp), n);
    TEST_ASSERT_EQUAL_STRING(exp, buf);
    TEST_ASSERT_EQUAL_INT(-1, hy_hook_test_payload(T_TAKEN, "Europe/Zurich", "hygeia-1A2B", 82, buf, 40));
}

static void test_hook_sig_header_rfc4231(void)
{
    /* HMAC-SHA256("Jefe", "what do ya want for nothing?") */
    const uint8_t mac[32] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
        0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43 };
    char out[72];
    TEST_ASSERT_EQUAL_INT(71, hy_hook_sig_header(mac, out));
    TEST_ASSERT_EQUAL_STRING("sha256=5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", out);
}

static void test_hook_verdict_codes(void)
{
    const int retry[] = { -1, 0, 408, 429, 500, 503, 599 };
    const int ok[] = { 200, 204, 299 };
    const int reject[] = { 100, 301, 302, 400, 404, 410, 422, 600 };
    for (size_t i = 0; i < sizeof retry / sizeof *retry; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(HY_HV_RETRY, hy_hook_verdict(retry[i]), "retry");
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(HY_HV_DELIVERED, hy_hook_verdict(ok[i]), "delivered");
    for (size_t i = 0; i < sizeof reject / sizeof *reject; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(HY_HV_REJECT, hy_hook_verdict(reject[i]), "reject");
    TEST_ASSERT_EQUAL_INT(HY_HV_PAUSE, hy_hook_verdict(401));
    TEST_ASSERT_EQUAL_INT(HY_HV_PAUSE, hy_hook_verdict(403));
}

static void test_hook_retry_schedule(void)
{
    const uint32_t want[] = { 0, 60, 300, 1800, 1800, 1800 };
    for (uint32_t i = 0; i < 6; i++) TEST_ASSERT_EQUAL_UINT32(want[i], hy_hook_retry_s(i));
    TEST_ASSERT_EQUAL_UINT32(1800, hy_hook_retry_s(0xFFFFFFFFu));
}

static void test_ob_expired_rules(void)
{
    int64_t at = 1762000000LL;
    TEST_ASSERT_FALSE(hy_ob_expired(at, at + HY_OB_MAX_AGE_S, true));
    TEST_ASSERT_TRUE(hy_ob_expired(at, at + HY_OB_MAX_AGE_S + 1, true));
    TEST_ASSERT_FALSE(hy_ob_expired(0, at + 100 * 86400LL, true));
    TEST_ASSERT_FALSE(hy_ob_expired(at, at + HY_OB_MAX_AGE_S + 1, false));
    TEST_ASSERT_FALSE(hy_ob_expired(at, at - 10, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hook_payload_taken);
    RUN_TEST(test_hook_payload_due_no_log);
    RUN_TEST(test_hook_payload_battery_low);
    RUN_TEST(test_hook_payload_undone_id_only);
    RUN_TEST(test_hook_payload_clock_invalid);
    RUN_TEST(test_hook_payload_escapes);
    RUN_TEST(test_hook_payload_too_small);
    RUN_TEST(test_hook_payload_log_too_small);
    RUN_TEST(test_hook_test_payload);
    RUN_TEST(test_hook_sig_header_rfc4231);
    RUN_TEST(test_hook_verdict_codes);
    RUN_TEST(test_hook_retry_schedule);
    RUN_TEST(test_ob_expired_rules);
    return UNITY_END();
}
