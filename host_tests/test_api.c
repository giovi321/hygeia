#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "cJSON.h"
#include "hy/api.h"
#include "hy/codec.h"
#include "hy/events.h"
#include "hy/settings.h"
#include "hy/med.h"
#include "hy/routes.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- routes ---- */

static void test_route_ids(void)
{
    uint32_t id = 0;
    const hy_route_t *r = hy_route_match(HY_M_POST, "/api/medicines/2/log", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_MED_LOG, r->id);
    TEST_ASSERT_EQUAL_UINT32(2, id);

    r = hy_route_match(HY_M_GET, "/api/medicines/4294967295", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_MED, r->id);
    TEST_ASSERT_EQUAL_UINT32(4294967295u, id);

    const char *bad[] = { "/api/medicines/0", "/api/medicines/01", "/api/medicines/-1",
                          "/api/medicines/4294967296", "/api/medicines/2/", "/api/medicines/2x",
                          "/api/medicines/+2", "/api/medicines/", "/api/medicines/99999999999999999999",
                          "/api/medicines/2/log/", "/api/nothing", "api/medicines", "" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        TEST_ASSERT_NULL_MESSAGE(hy_route_match(HY_M_GET, bad[i], &id), bad[i]);

    /* query stripped */
    r = hy_route_match(HY_M_GET, "/api/log?from=1&to=2", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_LOG, r->id);
    r = hy_route_match(HY_M_GET, "/api/medicines/7?x=1", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT32(7, id);

    /* wrong method */
    TEST_ASSERT_NULL(hy_route_match(HY_M_GET, "/api/medicines/2/log", &id));
    TEST_ASSERT_NULL(hy_route_match(HY_M_GET, "/api/pair/start", &id));
    TEST_ASSERT_NULL(hy_route_match(HY_M_DELETE, "/api/log", &id));
    TEST_ASSERT_NULL(hy_route_match(HY_M_GET, NULL, &id));

    /* id may be NULL */
    TEST_ASSERT_NOT_NULL(hy_route_match(HY_M_PUT, "/api/medicines/5", NULL));

    /* order is a literal route, and not an id */
    r = hy_route_match(HY_M_PUT, "/api/medicines/order", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_MED_ORDER, r->id);
    TEST_ASSERT_NULL(hy_route_match(HY_M_GET, "/api/medicines/order", &id));

    /* browsers are forgotten by a body ({"index":n} or {"all":true}), not by a path segment */
    r = hy_route_match(HY_M_DELETE, "/api/browsers", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_BROWSERS_DEL, r->id);
    TEST_ASSERT_NULL(hy_route_match(HY_M_DELETE, "/api/browsers/1", &id));

    /* firmware update from GitHub */
    r = hy_route_match(HY_M_GET, "/api/update", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_UPDATE, r->id);
    r = hy_route_match(HY_M_POST, "/api/update/check", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_UPDATE_CHECK, r->id);
    r = hy_route_match(HY_M_POST, "/api/update/install", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_UPDATE_INSTALL, r->id);
    TEST_ASSERT_NULL(hy_route_match(HY_M_GET, "/api/update/check", &id));
}

static void test_route_table_complete(void)
{
    size_t n = 0;
    const hy_route_t *t = hy_routes(&n);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT((size_t)HY_R_COUNT, n);
    for (int id = 0; id < HY_R_COUNT; id++) {
        int seen = 0;
        for (size_t i = 0; i < n; i++)
            if ((int)t[i].id == id) seen++;
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen, "route id appears exactly once");
    }
    for (size_t i = 0; i < n; i++) {
        bool open = (t[i].flags & HY_RF_OPEN) != 0;
        bool expect = t[i].id == HY_R_INFO || t[i].id == HY_R_PAIR_START || t[i].id == HY_R_PAIR;
        TEST_ASSERT_EQUAL_MESSAGE(expect, open, t[i].pattern);
        TEST_ASSERT_NOT_NULL(t[i].screen);
        TEST_ASSERT_EQUAL_CHAR('/', t[i].pattern[0]);
    }
    /* no two rows share a method and pattern */
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            TEST_ASSERT_FALSE(t[i].m == t[j].m && strcmp(t[i].pattern, t[j].pattern) == 0);
}

/* ---- errors ---- */

static void test_err_map(void)
{
    static const struct { hy_err_t e; int http; const char *code; const char *field; } cases[] = {
        { HY_E_INVALID, 400, "invalid", "" },
        { HY_E_CODE, 422, "code", "code" },
        { HY_E_NAME, 422, "name", "name" },
        { HY_E_DOSE, 422, "dose", "dose" },
        { HY_E_TIMES, 422, "times", "times" },
        { HY_E_INTERVAL, 422, "interval", "min_interval_min" },
        { HY_E_CODE_DUP, 409, "code_dup", "code" },
        { HY_E_NOT_FOUND, 404, "not_found", "" },
        { HY_E_CLOCK, 409, "clock_invalid", "" },
        { HY_E_UNDO_EXPIRED, 410, "undo_expired", "" },
        { HY_E_FULL, 409, "full", "" },
        { HY_E_IO, 503, "storage", "" },
        { HY_W_TOO_SOON, 409, "too_soon", "" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        hy_api_err_t a;
        hy_api_err_from(cases[i].e, &a);
        TEST_ASSERT_EQUAL(cases[i].e, a.err);
        TEST_ASSERT_EQUAL_INT(cases[i].http, a.http);
        TEST_ASSERT_EQUAL_STRING(cases[i].code, a.code);
        TEST_ASSERT_EQUAL_STRING(cases[i].field, a.field);
    }
    hy_api_err_t a;
    hy_api_err_from((hy_err_t)99, &a);
    TEST_ASSERT_EQUAL_INT(500, a.http);
}

static void test_err_json(void)
{
    hy_api_err_t a;
    char buf[200];
    hy_api_err_from(HY_E_NAME, &a);
    int n = hy_api_err_json(&a, "Name too long \"x\"", buf, sizeof buf);
    TEST_ASSERT_EQUAL((int)strlen(buf), n);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"name\",\"field\":\"name\",\"message\":\"Name too long \\\"x\\\"\"}", buf);
    hy_api_err_from(HY_E_IO, &a);
    n = hy_api_err_json(&a, "disk", buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"storage\",\"message\":\"disk\"}", buf);
    TEST_ASSERT_EQUAL(-1, hy_api_err_json(&a, "disk", buf, 10));
}

/* ---- medicines ---- */

static hy_med_t mk(uint32_t id, const char *code, const char *name)
{
    hy_med_t m;
    hy_med_defaults(&m);
    m.id = id;
    strcpy(m.code, code);
    strcpy(m.name, name);
    return m;
}

static hy_med_t full_med(void)
{
    hy_med_t m = mk(3, "V", "Vitamin D");
    strcpy(m.dose, "1 tab");
    m.color = 4;
    m.type = HY_SCHED_FIXED;
    m.times[0] = 8 * 60; m.times[1] = 20 * 60; m.n_times = 2;
    m.weekdays = 0x55;
    m.min_interval_min = 30;
    m.early_window_min = 45; m.late_after_min = 20; m.missed_after_min = 100;
    m.pills_enabled = true; m.pills_left = 12; m.pills_alert = 5;
    m.active_from = 20261001; m.active_until = 20261231;
    m.archived = false; m.order = 2;
    return m;
}

static void assert_med_eq(const hy_med_t *a, const hy_med_t *b)
{
    TEST_ASSERT_EQUAL_STRING(a->code, b->code);
    TEST_ASSERT_EQUAL_STRING(a->name, b->name);
    TEST_ASSERT_EQUAL_STRING(a->dose, b->dose);
    TEST_ASSERT_EQUAL_UINT8(a->color, b->color);
    TEST_ASSERT_EQUAL(a->type, b->type);
    TEST_ASSERT_EQUAL_UINT8(a->n_times, b->n_times);
    for (int i = 0; i < a->n_times; i++) TEST_ASSERT_EQUAL_UINT16(a->times[i], b->times[i]);
    TEST_ASSERT_EQUAL_UINT8(a->weekdays, b->weekdays);
    TEST_ASSERT_EQUAL_UINT16(a->min_interval_min, b->min_interval_min);
    TEST_ASSERT_EQUAL_UINT16(a->early_window_min, b->early_window_min);
    TEST_ASSERT_EQUAL_UINT16(a->late_after_min, b->late_after_min);
    TEST_ASSERT_EQUAL_UINT16(a->missed_after_min, b->missed_after_min);
    TEST_ASSERT_EQUAL(a->pills_enabled, b->pills_enabled);
    TEST_ASSERT_EQUAL_INT16(a->pills_left, b->pills_left);
    TEST_ASSERT_EQUAL_INT16(a->pills_alert, b->pills_alert);
    TEST_ASSERT_EQUAL_INT32(a->active_from, b->active_from);
    TEST_ASSERT_EQUAL_INT32(a->active_until, b->active_until);
}

static void test_med_roundtrip(void)
{
    hy_med_t m = full_med(), out;
    hy_api_err_t err;
    char *j = hy_api_med_json(&m);
    TEST_ASSERT_NOT_NULL(j);
    char want[32];
    snprintf(want, sizeof want, "\"etag\":\"%08x\"", (unsigned)hy_api_med_etag(&m));
    TEST_ASSERT_NOT_NULL(strstr(j, want));
    TEST_ASSERT_EQUAL(HY_OK, hy_api_med_from_json(j, NULL, &out, &err));
    assert_med_eq(&m, &out);
    /* archived and order are not taken from the body */
    TEST_ASSERT_FALSE(out.archived);
    TEST_ASSERT_EQUAL_UINT16(0, out.order);
    TEST_ASSERT_EQUAL_UINT32(0, out.id);
    free(j);
}

static void test_med_new_defaults(void)
{
    hy_med_t out, d;
    hy_api_err_t err;
    hy_med_defaults(&d);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_med_from_json("{\"code\":\"A\",\"name\":\"Antibiotic\"}", NULL, &out, &err));
    TEST_ASSERT_EQUAL_STRING("A", out.code);
    TEST_ASSERT_EQUAL_STRING("Antibiotic", out.name);
    TEST_ASSERT_EQUAL_UINT8(d.weekdays, out.weekdays);
    TEST_ASSERT_EQUAL_UINT16(d.early_window_min, out.early_window_min);
    TEST_ASSERT_EQUAL_UINT16(d.missed_after_min, out.missed_after_min);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_med_from_json("{}", NULL, &out, &err));
}

static void test_med_merge_partial(void)
{
    hy_med_t base = full_med(), out;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_med_from_json("{\"dose\":\"2 tab\"}", &base, &out, &err));
    hy_med_t want = base;
    strcpy(want.dose, "2 tab");
    assert_med_eq(&want, &out);
    TEST_ASSERT_EQUAL_UINT32(base.id, out.id);
    TEST_ASSERT_EQUAL_UINT16(base.order, out.order);

    /* nested: a schedule with only the weekdays, a pill_count with only the alert */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_med_from_json(
        "{\"schedule\":{\"weekdays\":3},\"pill_count\":{\"alert\":9},\"name\":null}", &base, &out, &err));
    want = base;
    want.weekdays = 3;
    want.pills_alert = 9;
    assert_med_eq(&want, &out);

    /* clearing a date with 0 */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_med_from_json("{\"active_until\":0}", &base, &out, &err));
    TEST_ASSERT_EQUAL_INT32(0, out.active_until);
    TEST_ASSERT_EQUAL_INT32(base.active_from, out.active_from);

    /* wrong type */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json("{\"color\":\"red\"}", &base, &out, &err));
    TEST_ASSERT_EQUAL_INT(400, err.http);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json("{\"schedule\":3}", &base, &out, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json("[1]", &base, &out, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json("{bad", &base, &out, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json(NULL, &base, &out, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json("{\"schedule\":{\"times\":[\"25:00\"]}}", &base, &out, &err));
}

static void test_med_ignores_protected(void)
{
    hy_med_t base = full_med(), out;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_med_from_json(
        "{\"id\":9,\"etag\":\"deadbeef\",\"archived\":true,\"order\":0,\"future\":{\"x\":1}}", &base, &out, &err));
    TEST_ASSERT_EQUAL_UINT32(3, out.id);
    TEST_ASSERT_FALSE(out.archived);
    TEST_ASSERT_EQUAL_UINT16(2, out.order);
    base.archived = true;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_med_from_json("{\"archived\":false}", &base, &out, &err));
    TEST_ASSERT_TRUE(out.archived);
}

static void test_med_bad_utf8(void)
{
    hy_med_t out;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json("{\"code\":\"A\",\"name\":\"\xC3\"}", NULL, &out, &err));
    TEST_ASSERT_EQUAL_STRING("name", err.field);
    TEST_ASSERT_EQUAL_INT(400, err.http);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json("{\"dose\":\"\xFF\"}", NULL, &out, &err));
    TEST_ASSERT_EQUAL_STRING("dose", err.field);
    /* overlong encoding and a surrogate are not UTF-8 either */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json("{\"name\":\"\xC0\x80\"}", NULL, &out, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json("{\"name\":\"\xED\xA0\x80\"}", NULL, &out, &err));
    /* valid multi-byte is fine: 16 two-byte characters fill the 32 bytes */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_med_from_json(
        "{\"name\":\"\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C"
        "\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\"}", NULL, &out, &err));

    /* lengths: 32 fits, 33 does not; dose 16 fits, 17 does not; code 2 fits, 3 does not */
    char body[256];
    snprintf(body, sizeof body, "{\"name\":\"%s\"}", "12345678901234567890123456789012");
    TEST_ASSERT_EQUAL(HY_OK, hy_api_med_from_json(body, NULL, &out, &err));
    snprintf(body, sizeof body, "{\"name\":\"%s\"}", "123456789012345678901234567890123");
    TEST_ASSERT_EQUAL(HY_E_NAME, hy_api_med_from_json(body, NULL, &out, &err));
    TEST_ASSERT_EQUAL_INT(422, err.http);
    TEST_ASSERT_EQUAL_STRING("name", err.field);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_med_from_json("{\"dose\":\"1234567890123456\"}", NULL, &out, &err));
    TEST_ASSERT_EQUAL(HY_E_DOSE, hy_api_med_from_json("{\"dose\":\"12345678901234567\"}", NULL, &out, &err));
    TEST_ASSERT_EQUAL_STRING("dose", err.field);
    TEST_ASSERT_EQUAL(HY_E_CODE, hy_api_med_from_json("{\"code\":\"ABC\"}", NULL, &out, &err));
    TEST_ASSERT_EQUAL_STRING("code", err.field);
}

static void test_med_duplicate_members(void)
{
    hy_med_t out;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json(
        "{\"code\":\"A\",\"name\":\"ok\",\"name\":\"\xC3\"}", NULL, &out, &err));
    TEST_ASSERT_EQUAL_STRING("name", err.field);
    TEST_ASSERT_EQUAL_INT(400, err.http);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json(
        "{\"dose\":\"1\",\"dose\":\"12345678901234567\"}", NULL, &out, &err));
    TEST_ASSERT_EQUAL_STRING("dose", err.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json(
        "{\"schedule\":{\"weekdays\":1,\"weekdays\":2}}", NULL, &out, &err));
    TEST_ASSERT_EQUAL_STRING("weekdays", err.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json(
        "{\"pill_count\":{\"alert\":1,\"alert\":2}}", NULL, &out, &err));
    TEST_ASSERT_EQUAL_STRING("alert", err.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json("{\"color\":1,\"color\":2}", NULL, &out, &err));
    TEST_ASSERT_EQUAL_STRING("color", err.field);
    /* a bad string is caught whichever position it has */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json("{\"name\":\"\xC3\",\"dose\":\"1\"}", NULL, &out, &err));
    TEST_ASSERT_EQUAL_STRING("name", err.field);
}

static void test_etag_changes_with_any_field(void)
{
    hy_med_t base = full_med();
    uint32_t e0 = hy_api_med_etag(&base);
    hy_med_t same = base;
    TEST_ASSERT_EQUAL_HEX32(e0, hy_api_med_etag(&same));

    hy_med_t v[24];
    int n = 0;
    v[n] = base; v[n++].code[0] = 'W';
    v[n] = base; v[n++].name[0] = 'X';
    v[n] = base; strcpy(v[n++].dose, "3 tab");
    v[n] = base; v[n++].color = 5;
    v[n] = base; v[n++].type = HY_SCHED_AS_NEEDED;
    v[n] = base; v[n++].times[0] = 9 * 60;
    v[n] = base; v[n].times[2] = 12 * 60; v[n++].n_times = 3;
    v[n] = base; v[n++].weekdays = 0x7F;
    v[n] = base; v[n++].min_interval_min = 31;
    v[n] = base; v[n++].early_window_min = 46;
    v[n] = base; v[n++].late_after_min = 21;
    v[n] = base; v[n++].missed_after_min = 101;
    v[n] = base; v[n++].pills_enabled = false;
    v[n] = base; v[n++].pills_alert = 6;
    v[n] = base; v[n++].active_from = 20261002;
    v[n] = base; v[n++].active_until = 20261230;
    v[n] = base; v[n++].archived = true;
    v[n] = base; v[n++].order = 3;
    v[n] = base; v[n++].id = 4;
    for (int i = 0; i < n; i++)
        TEST_ASSERT_NOT_EQUAL_MESSAGE(e0, hy_api_med_etag(&v[i]), "a field change must change the etag");

    /* a dose on the device must not make an open form stale */
    hy_med_t rem = base;
    rem.pills_left = 3;
    TEST_ASSERT_EQUAL_HEX32(e0, hy_api_med_etag(&rem));
}

static void test_meds_json(void)
{
    hy_med_t m[3] = { mk(1, "A", "Antibiotic"), mk(2, "V", "Vitamin D"), mk(3, "M", "Magnesium") };
    m[1].archived = true;
    char *j = hy_api_meds_json(m, 3);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_INT(0, strncmp(j, "{\"api\":1,\"meds\":[", 17));
    TEST_ASSERT_NOT_NULL(strstr(j, "\"Vitamin D\""));
    int etags = 0;
    for (const char *p = j; (p = strstr(p, "\"etag\":")) != NULL; p++) etags++;
    TEST_ASSERT_EQUAL_INT(3, etags);
    free(j);
    j = hy_api_meds_json(NULL, 0);
    TEST_ASSERT_EQUAL_STRING("{\"api\":1,\"meds\":[]}", j);
    free(j);
}

static void test_order_rules(void)
{
    hy_med_t m[4] = { mk(1, "A", "Antibiotic"), mk(2, "V", "Vitamin D"), mk(3, "M", "Magnesium"),
                      mk(4, "Z", "Zinc") };
    m[3].archived = true;
    uint32_t ids[4];
    size_t n_ids = 0;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_order_from_json("{\"ids\":[3,1,2]}", m, 4, ids, &n_ids, &err));
    TEST_ASSERT_EQUAL_UINT(3, n_ids);
    TEST_ASSERT_EQUAL_UINT32(3, ids[0]);
    TEST_ASSERT_EQUAL_UINT32(1, ids[1]);
    TEST_ASSERT_EQUAL_UINT32(2, ids[2]);
    const char *bad[] = { "{\"ids\":[1,2]}", "{\"ids\":[1,1,2]}", "{\"ids\":[1,2,3,4]}", "{\"ids\":[1,2,9]}",
                          "{\"ids\":[1,2,3,3]}", "{\"ids\":[]}", "{\"ids\":\"x\"}", "{}", "{\"ids\":[1,2,\"3\"]}",
                          "{\"ids\":[1,2,3.5]}", "{\"ids\":[1,2,-3]}", "[1,2,3]", "nope", NULL };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        n_ids = 77;
        TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_order_from_json(bad[i], m, 4, ids, &n_ids, &err));
        TEST_ASSERT_EQUAL_INT(400, err.http);
    }
    /* nothing active: an empty list is the only valid one */
    hy_med_t arch = mk(1, "A", "Antibiotic");
    arch.archived = true;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_order_from_json("{\"ids\":[]}", &arch, 1, ids, &n_ids, &err));
    TEST_ASSERT_EQUAL_UINT(0, n_ids);
}

/* ---- log ---- */

static void test_logq_defaults(void)
{
    hy_api_logq_t q;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_logq_parse(NULL, 1000000, &q, &err));
    TEST_ASSERT_EQUAL_INT64(1000001, q.to);
    TEST_ASSERT_EQUAL_INT64(1000001 - 30 * 86400, q.from);
    TEST_ASSERT_EQUAL_UINT16(200, q.limit);
    TEST_ASSERT_EQUAL_UINT32(0, q.med);
    TEST_ASSERT_EQUAL_INT64(0, q.after_ts);
    TEST_ASSERT_EQUAL_UINT32(0, q.after_id);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_logq_parse("", 1000000, &q, &err));
    TEST_ASSERT_EQUAL_UINT16(200, q.limit);

    TEST_ASSERT_EQUAL(HY_OK, hy_api_logq_parse("from=100&to=5000&med=7&limit=500&after=4000.12", 9, &q, &err));
    TEST_ASSERT_EQUAL_INT64(100, q.from);
    TEST_ASSERT_EQUAL_INT64(5000, q.to);
    TEST_ASSERT_EQUAL_UINT32(7, q.med);
    TEST_ASSERT_EQUAL_UINT16(500, q.limit);
    TEST_ASSERT_EQUAL_INT64(4000, q.after_ts);
    TEST_ASSERT_EQUAL_UINT32(12, q.after_id);

    /* only to: from follows it; unknown keys ignored; leading ? accepted */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_logq_parse("?to=3000000&x=1&limit=1", 9, &q, &err));
    TEST_ASSERT_EQUAL_INT64(3000000 - 30 * 86400, q.from);
    TEST_ASSERT_EQUAL_UINT16(1, q.limit);
}

static void test_logq_bad(void)
{
    hy_api_logq_t q;
    hy_api_err_t err;
    static const struct { const char *query; const char *field; } cases[] = {
        { "from=10&to=10", "from" },
        { "from=20&to=10", "from" },
        { "limit=0", "limit" },
        { "limit=501", "limit" },
        { "limit=x", "limit" },
        { "med=x", "med" },
        { "med=4294967296", "med" },
        { "from=abc", "from" },
        { "to=", "to" },
        { "after=5", "after" },
        { "after=5.x", "after" },
        { "after=.5", "after" },
        { "from=99999999999999999999", "from" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(HY_E_INVALID, hy_api_logq_parse(cases[i].query, 1000000, &q, &err), cases[i].query);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].field, err.field, cases[i].query);
        TEST_ASSERT_EQUAL_INT(400, err.http);
    }
}

static void test_page_filter(void)
{
    hy_log_t v[4];
    memset(v, 0, sizeof v);
    int64_t ts[4] = { 10, 10, 10, 11 };
    for (int i = 0; i < 4; i++) { v[i].ts = ts[i]; v[i].id = (uint32_t)(i + 1); }
    size_t k = hy_api_page_filter(v, 4, 10, 2);
    TEST_ASSERT_EQUAL_UINT(2, k);
    TEST_ASSERT_EQUAL_UINT32(3, v[0].id);
    TEST_ASSERT_EQUAL_UINT32(4, v[1].id);

    for (int i = 0; i < 4; i++) { v[i].ts = ts[i]; v[i].id = (uint32_t)(i + 1); }
    TEST_ASSERT_EQUAL_UINT(4, hy_api_page_filter(v, 4, 0, 0));
    TEST_ASSERT_EQUAL_UINT(0, hy_api_page_filter(v, 4, 11, 4));
    TEST_ASSERT_EQUAL_UINT(0, hy_api_page_filter(v, 0, 5, 5));
}

static hy_log_t mk_log(void)
{
    hy_log_t e;
    memset(&e, 0, sizeof e);
    e.id = 42;
    e.ts = 1793181600LL; /* 2026-10-28 10:00:00 UTC */
    strcpy(e.tz, "CET-1CEST,M3.5.0,M10.5.0/3");
    e.med_id = 2;
    e.status = HY_TAKEN;
    e.slot = 1793181000LL;
    e.delta_min = -10;
    e.source = HY_SRC_WEB;
    return e;
}

static void test_log_json_local(void)
{
    hy_log_t e = mk_log();
    char buf[512];
    int n = hy_api_log_json(&e, buf, sizeof buf);
    TEST_ASSERT_EQUAL((int)strlen(buf), n);
    /* the October change was on the 25th, so the 28th is CET (UTC+1) */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"local\":\"2026-10-28 11:00\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"med_id\":2"));
    hy_log_t d;
    TEST_ASSERT_EQUAL(HY_OK, hy_log_from_jsonl(buf, &d));
    TEST_ASSERT_EQUAL_INT64(e.ts, d.ts);
    TEST_ASSERT_EQUAL(-1, hy_api_log_json(&e, buf, 20));
    /* empty tz reads as UTC */
    e.tz[0] = '\0';
    hy_api_log_json(&e, buf, sizeof buf);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"local\":\"2026-10-28 10:00\""));
}

static void test_logreq(void)
{
    hy_api_logreq_t r;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_logreq_from_json(
        "{\"med_id\":2,\"ts\":1793181600,\"orig_ts\":1793181000,\"status\":\"skipped\",\"note\":\"late\","
        "\"confirm\":true}", &r, &err));
    TEST_ASSERT_TRUE(r.has_med && r.has_ts && r.has_note && r.has_status && r.confirm);
    TEST_ASSERT_EQUAL_UINT32(2, r.med_id);
    TEST_ASSERT_EQUAL_INT64(1793181600LL, r.ts);
    TEST_ASSERT_EQUAL_INT64(1793181000LL, r.orig_ts);
    TEST_ASSERT_EQUAL(HY_SKIPPED, r.status);
    TEST_ASSERT_EQUAL_STRING("late", r.note);

    TEST_ASSERT_EQUAL(HY_OK, hy_api_logreq_from_json("{\"note\":\"\"}", &r, &err));
    TEST_ASSERT_TRUE(r.has_note);
    TEST_ASSERT_FALSE(r.has_ts || r.has_status || r.has_med || r.confirm);

    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_logreq_from_json("{\"status\":\"eaten\"}", &r, &err));
    TEST_ASSERT_EQUAL_STRING("status", err.field);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_logreq_from_json("{\"status\":\"missed\"}", &r, &err));
    TEST_ASSERT_EQUAL(HY_MISSED, r.status);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_logreq_from_json("{\"ts\":\"x\"}", &r, &err));
    TEST_ASSERT_EQUAL_STRING("ts", err.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_logreq_from_json("{\"med_id\":0}", &r, &err));
    TEST_ASSERT_EQUAL_STRING("med_id", err.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_logreq_from_json("{\"ts\":1.5}", &r, &err));
    char body[200];
    snprintf(body, sizeof body, "{\"note\":\"%0*d\"}", 64, 0);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_logreq_from_json(body, &r, &err));
    TEST_ASSERT_EQUAL_STRING("note", err.field);
    snprintf(body, sizeof body, "{\"note\":\"%0*d\"}", 63, 0);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_logreq_from_json(body, &r, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_logreq_from_json("{\"note\":\"\xC3\"}", &r, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_logreq_from_json("[]", &r, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_logreq_from_json(NULL, &r, &err));
}

/* ---- csv ---- */

static void test_csv_formula(void)
{
    char o[64];
    TEST_ASSERT_EQUAL(9, hy_csv_field("=SUM(A1)", o, sizeof o));
    TEST_ASSERT_EQUAL_STRING("'=SUM(A1)", o);
    hy_csv_field("+1", o, sizeof o);
    TEST_ASSERT_EQUAL_STRING("'+1", o);
    hy_csv_field("-1", o, sizeof o);
    TEST_ASSERT_EQUAL_STRING("'-1", o);
    hy_csv_field("@x", o, sizeof o);
    TEST_ASSERT_EQUAL_STRING("'@x", o);
    hy_csv_field("\tx", o, sizeof o);
    TEST_ASSERT_EQUAL_STRING("'\tx", o);
    hy_csv_field("\rx", o, sizeof o);
    TEST_ASSERT_EQUAL_STRING("\"'\rx\"", o);
    hy_csv_field("a,b", o, sizeof o);
    TEST_ASSERT_EQUAL_STRING("\"a,b\"", o);
    hy_csv_field("say \"hi\"", o, sizeof o);
    TEST_ASSERT_EQUAL_STRING("\"say \"\"hi\"\"\"", o);
    hy_csv_field("l1\nl2", o, sizeof o);
    TEST_ASSERT_EQUAL_STRING("\"l1\nl2\"", o);
    hy_csv_field("plain", o, sizeof o);
    TEST_ASSERT_EQUAL_STRING("plain", o);
    TEST_ASSERT_EQUAL(0, hy_csv_field("", o, sizeof o));
    TEST_ASSERT_EQUAL_STRING("", o);
    TEST_ASSERT_EQUAL(-1, hy_csv_field("abcdef", o, 6));
    TEST_ASSERT_EQUAL(5, hy_csv_field("abcde", o, 6));
    TEST_ASSERT_EQUAL(-1, hy_csv_field("a,b", o, 4));
}

static void test_log_csv(void)
{
    hy_log_t e = mk_log();
    hy_med_t m = mk(2, "V", "Vitamin D");
    strcpy(e.note, "=SUM(A1)");
    char buf[512];
    int n = hy_api_log_csv(&e, &m, buf, sizeof buf);
    TEST_ASSERT_EQUAL((int)strlen(buf), n);
    TEST_ASSERT_EQUAL_STRING("42,2026-10-28 11:00,1793181600,\"CET-1CEST,M3.5.0,M10.5.0/3\",V,Vitamin D,taken,"
                             "2026-10-28 10:50,-10,'=SUM(A1),web,false", buf);
    /* no medicine, no slot */
    e.slot = 0;
    e.note[0] = '\0';
    e.source = HY_SRC_SCREEN;
    e.edited = true;
    hy_api_log_csv(&e, NULL, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("42,2026-10-28 11:00,1793181600,\"CET-1CEST,M3.5.0,M10.5.0/3\",?,,taken,,-10,,screen,true", buf);
    TEST_ASSERT_EQUAL(-1, hy_api_log_csv(&e, NULL, buf, 30));
}

/* ---- time ---- */

#define BUILD 1700000000LL

static void test_time_parse(void)
{
    hy_api_time_t t;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_time_from_json(
        "{\"utc_ms\":1800000000123,\"zone\":\"Europe/Zurich\"}", BUILD, &t, &err));
    TEST_ASSERT_EQUAL_INT64(1800000000123LL, t.utc_ms);
    TEST_ASSERT_TRUE(t.has_zone);
    TEST_ASSERT_EQUAL_STRING("Europe/Zurich", t.zone);
    TEST_ASSERT_FALSE(t.is_auto);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_time_from_json("{\"utc_ms\":1800000000123}", BUILD, &t, &err));
    TEST_ASSERT_FALSE(t.has_zone);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_time_from_json("{\"zone\":\"Europe/Zurich\"}", BUILD, &t, &err));
    TEST_ASSERT_EQUAL_STRING("utc_ms", err.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_time_from_json("{\"utc_ms\":\"1\"}", BUILD, &t, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_time_from_json("{\"utc_ms\":1.5}", BUILD, &t, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_time_from_json("{\"utc_ms\":1800000000123,\"auto\":1}", BUILD, &t, &err));
    TEST_ASSERT_EQUAL_STRING("auto", err.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_time_from_json("nope", BUILD, &t, &err));
}

static void test_time_before_build(void)
{
    hy_api_time_t t;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_time_from_json("{\"utc_ms\":1700000000000}", BUILD, &t, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_time_from_json("{\"utc_ms\":1699999999999}", BUILD, &t, &err));
    TEST_ASSERT_EQUAL_STRING("utc_ms", err.field);
    TEST_ASSERT_EQUAL_INT(400, err.http);
}

static void test_time_unknown_zone(void)
{
    hy_api_time_t t;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_time_from_json(
        "{\"utc_ms\":1800000000000,\"zone\":\"Mars/Base\"}", BUILD, &t, &err));
    TEST_ASSERT_EQUAL_STRING("zone", err.field);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_time_from_json(
        "{\"utc_ms\":1800000000000,\"zone\":7}", BUILD, &t, &err));
    TEST_ASSERT_EQUAL_STRING("zone", err.field);
    /* a zone longer than the buffer cannot be in the table */
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_time_from_json(
        "{\"utc_ms\":1800000000000,\"zone\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}",
        BUILD, &t, &err));
}

static void test_time_auto_drops_zone(void)
{
    hy_api_time_t t;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_time_from_json(
        "{\"utc_ms\":1800000000000,\"zone\":\"Europe/London\",\"auto\":true}", BUILD, &t, &err));
    TEST_ASSERT_TRUE(t.is_auto);
    TEST_ASSERT_FALSE(t.has_zone);
    TEST_ASSERT_EQUAL_STRING("", t.zone);
    /* an auto request does not fail on a zone it drops */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_time_from_json(
        "{\"utc_ms\":1800000000000,\"zone\":\"Mars/Base\",\"auto\":true}", BUILD, &t, &err));
    TEST_ASSERT_FALSE(t.has_zone);
}

static void test_time_auto_small_diff(void)
{
    hy_api_time_t t;
    memset(&t, 0, sizeof t);
    t.utc_ms = 1800000000000LL;
    t.is_auto = true;
    TEST_ASSERT_FALSE(hy_api_time_should_set(&t, t.utc_ms + 1500, true));
    TEST_ASSERT_FALSE(hy_api_time_should_set(&t, t.utc_ms - 1500, true));
    TEST_ASSERT_FALSE(hy_api_time_should_set(&t, t.utc_ms + 2000, true));
    TEST_ASSERT_TRUE(hy_api_time_should_set(&t, t.utc_ms + 2500, true));
    TEST_ASSERT_TRUE(hy_api_time_should_set(&t, t.utc_ms - 2500, true));
    TEST_ASSERT_TRUE(hy_api_time_should_set(&t, t.utc_ms, false));
    t.is_auto = false;
    TEST_ASSERT_TRUE(hy_api_time_should_set(&t, t.utc_ms, true));
    TEST_ASSERT_TRUE(hy_api_time_should_set(&t, t.utc_ms + 1, true));
}

/* ---- settings ---- */

static hy_settings_t full_settings(void)
{
    hy_settings_t s;
    hy_settings_defaults(&s);
    s.sound = false; s.dnd = true; s.volume = 35; s.brightness = 80; s.snooze_min = 20;
    s.dim_s = 45; s.off_bat_s = 300; s.off_usb_s = 900; s.rot_lock = true; s.rot = 2;
    s.quiet_from = 22 * 60; s.quiet_to = 7 * 60 + 30; s.wifi_keep_batt = true;
    s.cab_on = true; s.cab_mode = HY_CAB_TAUGHT; s.cab_chime = false; s.cab_light = true; s.cab_sens = 4; s.cab_window_min = 45;
    s.wake_single_tap = true;
    s.double_tap_wake = false;
    return s;
}

static void assert_settings_eq(const hy_settings_t *a, const hy_settings_t *b)
{
    TEST_ASSERT_EQUAL(a->sound, b->sound);
    TEST_ASSERT_EQUAL(a->dnd, b->dnd);
    TEST_ASSERT_EQUAL_UINT8(a->volume, b->volume);
    TEST_ASSERT_EQUAL_UINT8(a->brightness, b->brightness);
    TEST_ASSERT_EQUAL_UINT16(a->snooze_min, b->snooze_min);
    TEST_ASSERT_EQUAL_UINT16(a->dim_s, b->dim_s);
    TEST_ASSERT_EQUAL_UINT16(a->off_bat_s, b->off_bat_s);
    TEST_ASSERT_EQUAL_UINT32(a->off_usb_s, b->off_usb_s);
    TEST_ASSERT_EQUAL(a->rot_lock, b->rot_lock);
    TEST_ASSERT_EQUAL_UINT8(a->rot, b->rot);
    TEST_ASSERT_EQUAL_UINT16(a->quiet_from, b->quiet_from);
    TEST_ASSERT_EQUAL_UINT16(a->quiet_to, b->quiet_to);
    TEST_ASSERT_EQUAL(a->wifi_keep_batt, b->wifi_keep_batt);
    TEST_ASSERT_EQUAL(a->cab_on, b->cab_on);
    TEST_ASSERT_EQUAL_UINT8(a->cab_mode, b->cab_mode);
    TEST_ASSERT_EQUAL(a->cab_chime, b->cab_chime);
    TEST_ASSERT_EQUAL(a->cab_light, b->cab_light);
    TEST_ASSERT_EQUAL_UINT8(a->cab_sens, b->cab_sens);
    TEST_ASSERT_EQUAL_UINT8(a->cab_window_min, b->cab_window_min);
    TEST_ASSERT_EQUAL(a->wake_single_tap, b->wake_single_tap);
    TEST_ASSERT_EQUAL(a->double_tap_wake, b->double_tap_wake);
}

static void test_settings_roundtrip(void)
{
    hy_settings_t s = full_settings(), cur, out;
    hy_settings_defaults(&cur);
    char *js = hy_api_settings_json(&s);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"quiet\":{\"from\":\"22:00\",\"to\":\"07:30\"}"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"rot_lock\":true"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"wifi_keep_batt\":true"));
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json(js, &cur, &out, &err));
    TEST_ASSERT_EQUAL(s.sound, out.sound);
    TEST_ASSERT_EQUAL(s.dnd, out.dnd);
    TEST_ASSERT_EQUAL_UINT8(s.volume, out.volume);
    TEST_ASSERT_EQUAL_UINT8(s.brightness, out.brightness);
    TEST_ASSERT_EQUAL_UINT16(s.snooze_min, out.snooze_min);
    TEST_ASSERT_EQUAL_UINT16(s.dim_s, out.dim_s);
    TEST_ASSERT_EQUAL_UINT16(s.off_bat_s, out.off_bat_s);
    TEST_ASSERT_EQUAL_UINT32(s.off_usb_s, out.off_usb_s);
    TEST_ASSERT_EQUAL(s.rot_lock, out.rot_lock);
    TEST_ASSERT_EQUAL_UINT16(s.quiet_from, out.quiet_from);
    TEST_ASSERT_EQUAL_UINT16(s.quiet_to, out.quiet_to);
    TEST_ASSERT_EQUAL(s.wifi_keep_batt, out.wifi_keep_batt);
    TEST_ASSERT_FALSE(out.double_tap_wake);
    /* the applied rotation is read-only: the body's "rot" never reaches the settings */
    TEST_ASSERT_EQUAL_UINT8(cur.rot, out.rot);
    free(js);
}

static void test_settings_merge_partial(void)
{
    hy_settings_t cur = full_settings(), out;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"volume\":30}", &cur, &out, &err));
    TEST_ASSERT_EQUAL_UINT8(30, out.volume);
    cur.volume = 30;
    assert_settings_eq(&cur, &out);
    /* null counts as absent */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"volume\":null,\"dnd\":null}", &cur, &out, &err));
    assert_settings_eq(&cur, &out);
    /* unknown members are ignored */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"nope\":1,\"rot\":3}", &cur, &out, &err));
    assert_settings_eq(&cur, &out);
}

static void test_settings_quiet_null(void)
{
    hy_settings_t cur = full_settings(), out;
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"quiet\":null}", &cur, &out, &err));
    TEST_ASSERT_EQUAL_UINT16(0, out.quiet_from);
    TEST_ASSERT_EQUAL_UINT16(0, out.quiet_to);
    TEST_ASSERT_EQUAL_UINT8(cur.volume, out.volume);
    char *js = hy_api_settings_json(&out);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"quiet\":null"));
    free(js);
    /* from == to is also no quiet hours and reads back as null */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"quiet\":{\"from\":\"10:00\",\"to\":\"10:00\"}}", &cur, &out, &err));
    js = hy_api_settings_json(&out);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"quiet\":null"));
    free(js);
    /* set */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"quiet\":{\"from\":\"23:59\",\"to\":\"00:00\"}}", &cur, &out, &err));
    TEST_ASSERT_EQUAL_UINT16(1439, out.quiet_from);
    TEST_ASSERT_EQUAL_UINT16(0, out.quiet_to);
}

static void test_settings_bad_type(void)
{
    hy_settings_t cur = full_settings(), out, keep;
    hy_api_err_t err;
    keep = out = cur;
    out.volume = 1;
    keep = out;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_settings_from_json("{\"volume\":\"x\"}", &cur, &out, &err));
    TEST_ASSERT_EQUAL_STRING("volume", err.field);
    TEST_ASSERT_EQUAL_INT(400, err.http);
    assert_settings_eq(&keep, &out);   /* untouched on failure */
    const char *bad[][2] = {
        { "{\"sound\":1}", "sound" }, { "{\"dnd\":\"yes\"}", "dnd" }, { "{\"brightness\":1.5}", "brightness" },
        { "{\"snooze_min\":-1}", "snooze_min" }, { "{\"dim_s\":[]}", "dim_s" }, { "{\"off_bat_s\":{}}", "off_bat_s" },
        { "{\"off_usb_s\":true}", "off_usb_s" }, { "{\"rot_lock\":0}", "rot_lock" },
        { "{\"wifi_keep_batt\":\"no\"}", "wifi_keep_batt" }, { "{\"quiet\":5}", "quiet" },
        { "{\"quiet\":{\"from\":\"22:00\"}}", "quiet" }, { "{\"quiet\":{\"from\":\"24:00\",\"to\":\"01:00\"}}", "quiet" },
        { "{\"quiet\":{\"from\":\"7:00\",\"to\":\"01:00\"}}", "quiet" }, { "{\"quiet\":{\"from\":1,\"to\":2}}", "quiet" },
        { "{\"volume\":1,\"volume\":2}", "volume" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(HY_E_INVALID, hy_api_settings_from_json(bad[i][0], &cur, &out, &err), bad[i][0]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(bad[i][1], err.field, bad[i][0]);
    }
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_settings_from_json("[]", &cur, &out, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_settings_from_json("nope", &cur, &out, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_settings_from_json(NULL, &cur, &out, &err));
}

static void test_settings_clamped(void)
{
    hy_settings_t cur, out;
    hy_settings_defaults(&cur);
    hy_api_err_t err;
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"brightness\":3}", &cur, &out, &err));
    TEST_ASSERT_EQUAL_UINT8(10, out.brightness);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"volume\":900,\"snooze_min\":99999,\"dim_s\":5}", &cur, &out, &err));
    TEST_ASSERT_EQUAL_UINT8(100, out.volume);
    TEST_ASSERT_EQUAL_UINT16(120, out.snooze_min);
    TEST_ASSERT_EQUAL_UINT16(10, out.dim_s);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"off_usb_s\":5000000000}", &cur, &out, &err));
    TEST_ASSERT_EQUAL_UINT32(86400, out.off_usb_s);
}

static void test_api_settings_cab_roundtrip(void)
{
    hy_settings_t cur, out;
    hy_settings_defaults(&cur);
    hy_api_err_t err;
    char *js = hy_api_settings_json(&cur);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"cabinet\":{\"on\":false,\"mode\":\"any\",\"chime\":true,\"light\":true,"
                                    "\"sensitivity\":3,\"window_min\":15}"));
    free(js);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"cabinet\":{\"window_min\":30,\"mode\":\"taught\"}}", &cur, &out, &err));
    TEST_ASSERT_EQUAL_UINT8(30, out.cab_window_min);
    TEST_ASSERT_EQUAL_UINT8(HY_CAB_TAUGHT, out.cab_mode);
    out.cab_window_min = cur.cab_window_min;
    out.cab_mode = cur.cab_mode;
    assert_settings_eq(&cur, &out);   /* only those two changed */
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json(
        "{\"cabinet\":{\"on\":true,\"chime\":false,\"light\":false,\"sensitivity\":5,\"window_min\":0,\"mode\":\"any\"}}",
        &cur, &out, &err));
    TEST_ASSERT_TRUE(out.cab_on);
    TEST_ASSERT_FALSE(out.cab_chime);
    TEST_ASSERT_FALSE(out.cab_light);
    TEST_ASSERT_EQUAL_UINT8(5, out.cab_sens);
    TEST_ASSERT_EQUAL_UINT8(0, out.cab_window_min);
    js = hy_api_settings_json(&out);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"cabinet\":{\"on\":true,\"mode\":\"any\",\"chime\":false,\"light\":false,"
                                    "\"sensitivity\":5,\"window_min\":0}"));
    free(js);
}

static void test_api_settings_wake_tap(void)
{
    hy_settings_t cur, out;
    hy_settings_defaults(&cur);
    hy_api_err_t err;
    char *js = hy_api_settings_json(&cur);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"wake_single_tap\":false"));
    free(js);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"wake_single_tap\":true}", &cur, &out, &err));
    TEST_ASSERT_TRUE(out.wake_single_tap);
    out.wake_single_tap = cur.wake_single_tap;
    assert_settings_eq(&cur, &out);   /* only that one changed */
    cur.wake_single_tap = true;
    js = hy_api_settings_json(&cur);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"wake_single_tap\":true"));
    free(js);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"volume\":10}", &cur, &out, &err));   /* absent keeps the value */
    TEST_ASSERT_TRUE(out.wake_single_tap);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"wake_single_tap\":false}", &cur, &out, &err));
    TEST_ASSERT_FALSE(out.wake_single_tap);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_settings_from_json("{\"wake_single_tap\":1}", &cur, &out, &err));
    TEST_ASSERT_EQUAL_STRING("wake_single_tap", err.field);
    TEST_ASSERT_EQUAL_INT(400, err.http);
}

static void test_api_settings_double_tap_wake(void)
{
    hy_settings_t cur, out;
    hy_settings_defaults(&cur);
    hy_api_err_t err;
    char *js = hy_api_settings_json(&cur);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"double_tap_wake\":true"));
    free(js);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"double_tap_wake\":false}", &cur, &out, &err));
    TEST_ASSERT_FALSE(out.double_tap_wake);
    out.double_tap_wake = cur.double_tap_wake;
    assert_settings_eq(&cur, &out);   /* only that one changed */
    cur.double_tap_wake = false;
    js = hy_api_settings_json(&cur);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"double_tap_wake\":false"));
    free(js);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"volume\":10}", &cur, &out, &err));   /* absent keeps the value */
    TEST_ASSERT_FALSE(out.double_tap_wake);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json("{\"double_tap_wake\":true}", &cur, &out, &err));
    TEST_ASSERT_TRUE(out.double_tap_wake);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_settings_from_json("{\"double_tap_wake\":1}", &cur, &out, &err));
    TEST_ASSERT_EQUAL_STRING("double_tap_wake", err.field);
    TEST_ASSERT_EQUAL_INT(400, err.http);
}

static void test_api_settings_cab_bad(void)
{
    hy_settings_t cur = full_settings(), out, keep;
    hy_api_err_t err;
    out = cur;
    keep = out;
    const char *bad[][2] = {
        { "{\"cabinet\":{\"window_min\":17}}", "cabinet.window_min" },
        { "{\"cabinet\":{\"window_min\":125}}", "cabinet.window_min" },
        { "{\"cabinet\":{\"window_min\":-5}}", "cabinet.window_min" },
        { "{\"cabinet\":{\"window_min\":\"5\"}}", "cabinet.window_min" },
        { "{\"cabinet\":{\"sensitivity\":6}}", "cabinet.sensitivity" },
        { "{\"cabinet\":{\"sensitivity\":0}}", "cabinet.sensitivity" },
        { "{\"cabinet\":{\"mode\":\"door\"}}", "cabinet.mode" },
        { "{\"cabinet\":{\"mode\":1}}", "cabinet.mode" },
        { "{\"cabinet\":{\"on\":1}}", "cabinet.on" },
        { "{\"cabinet\":{\"chime\":\"yes\"}}", "cabinet.chime" },
        { "{\"cabinet\":{\"light\":0}}", "cabinet.light" },
        { "{\"cabinet\":5}", "cabinet" },
        { "{\"volume\":10,\"cabinet\":{\"window_min\":17}}", "cabinet.window_min" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(HY_E_INVALID, hy_api_settings_from_json(bad[i][0], &cur, &out, &err), bad[i][0]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(bad[i][1], err.field, bad[i][0]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(400, err.http, bad[i][0]);
        assert_settings_eq(&keep, &out);
    }
}

/* ---- state and events ---- */

/* ---- cabinet open ---- */

/* Unity here has no double support */
#define ASSERT_NUM(want, got) TEST_ASSERT_TRUE_MESSAGE(fabs((double)(want) - (double)(got)) < 1e-4, #got)

static void test_route_cabinet(void)
{
    uint32_t id = 0;
    const hy_route_t *r = hy_route_match(HY_M_GET, "/api/cabinet", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_CABINET, r->id);
    TEST_ASSERT_EQUAL_UINT8(0, r->flags);   /* paired session */
    r = hy_route_match(HY_M_POST, "/api/cabinet/test", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_CABINET_TEST, r->id);
    TEST_ASSERT_EQUAL_UINT8(0, r->flags);
    r = hy_route_match(HY_M_GET, "/api/cabinet/samples.csv", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_CABINET_SAMPLES, r->id);
    TEST_ASSERT_EQUAL_UINT8(0, r->flags);
    r = hy_route_match(HY_M_DELETE, "/api/cabinet/teach", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_CABINET_DEL, r->id);
    TEST_ASSERT_EQUAL_UINT8(0, r->flags);
    /* methods are exact */
    TEST_ASSERT_NULL(hy_route_match(HY_M_PUT, "/api/cabinet", &id));
    TEST_ASSERT_NULL(hy_route_match(HY_M_GET, "/api/cabinet/test", &id));
    TEST_ASSERT_NULL(hy_route_match(HY_M_POST, "/api/cabinet/samples.csv", &id));
    TEST_ASSERT_NULL(hy_route_match(HY_M_GET, "/api/cabinet/teach", &id));
    /* a query is ignored */
    TEST_ASSERT_NOT_NULL(hy_route_match(HY_M_GET, "/api/cabinet?x=1", &id));
}

static void test_routes_integ(void)
{
    uint32_t id = 0;
    const hy_route_t *r = hy_route_match(HY_M_GET, "/api/integrations", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_INTEG, r->id);
    TEST_ASSERT_EQUAL_UINT8(0, r->flags);   /* paired session */
    TEST_ASSERT_EQUAL_STRING("", r->screen);
    r = hy_route_match(HY_M_POST, "/api/webhook/test", &id);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(HY_R_WEBHOOK_TEST, r->id);
    TEST_ASSERT_EQUAL_UINT8(0, r->flags);
    TEST_ASSERT_EQUAL_STRING("", r->screen);
    /* methods are exact */
    TEST_ASSERT_NULL(hy_route_match(HY_M_POST, "/api/integrations", &id));
    TEST_ASSERT_NULL(hy_route_match(HY_M_PUT, "/api/integrations", &id));
    TEST_ASSERT_NULL(hy_route_match(HY_M_GET, "/api/webhook/test", &id));
    TEST_ASSERT_NOT_NULL(hy_route_match(HY_M_GET, "/api/integrations?x=1", &id));
}

static hy_cab_status_t cab_status_taught(void)
{
    hy_cab_status_t st;
    memset(&st, 0, sizeof st);
    st.taught = true; st.taught_at = 1782000000; st.mode = HY_CAB_TAUGHT; st.live = true; st.testing = true;
    st.thr = 0.75f; st.trig_mg = 90;
    st.wom_irq = 11; st.captures = 7; st.matches = 5; st.others = 2; st.reactions = 3;
    st.test_matches = 4; st.test_others = 1;
    return st;
}

static void test_api_cabinet_json_not_taught(void)
{
    hy_cab_status_t st;
    memset(&st, 0, sizeof st);
    st.mode = HY_CAB_ANY;
    cJSON *o = hy_api_cabinet_json(&st, NULL, 0);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "taught")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(o, "taught_at")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(o, "threshold")));
    TEST_ASSERT_EQUAL_STRING("any", cJSON_GetObjectItemCaseSensitive(o, "mode")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "live")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "testing")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "teaching")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "checking")));
    TEST_ASSERT_EQUAL_INT(0, (int)cJSON_GetObjectItemCaseSensitive(o, "trigger_mg")->valuedouble);
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, "counts");
    TEST_ASSERT_TRUE(cJSON_IsObject(c));
    const char *keys[] = {"wom", "captures", "matches", "others", "reactions"};
    for (size_t i = 0; i < 5; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(c, keys[i]);
        TEST_ASSERT_TRUE_MESSAGE(cJSON_IsNumber(v), keys[i]);
        TEST_ASSERT_EQUAL_INT(0, (int)v->valuedouble);
    }
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "test");
    TEST_ASSERT_EQUAL_INT(0, (int)cJSON_GetObjectItemCaseSensitive(t, "matches")->valuedouble);
    TEST_ASSERT_EQUAL_INT(0, (int)cJSON_GetObjectItemCaseSensitive(t, "others")->valuedouble);
    const cJSON *rec = cJSON_GetObjectItemCaseSensitive(o, "recent");
    TEST_ASSERT_TRUE(cJSON_IsArray(rec));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(rec));
    cJSON_Delete(o);
}

static void test_api_cabinet_json_taught(void)
{
    hy_cab_status_t st = cab_status_taught();
    static hy_cab_recent_t rec[3];
    memset(rec, 0, sizeof rec);
    rec[0] = (hy_cab_recent_t){.kind = HY_CAB_REC_TEST, .at = 1782000100, .dist = 0.5f, .thr = 0.75f, .match = true};
    rec[1] = (hy_cab_recent_t){.kind = HY_CAB_REC_LIVE, .at = 0, .dist = INFINITY, .thr = 0.75f, .match = false};
    rec[2] = (hy_cab_recent_t){.kind = HY_CAB_REC_LIVE, .at = 1782000300, .dist = 1.25f, .thr = 0.75f, .match = false};
    cJSON *o = hy_api_cabinet_json(&st, rec, 3);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "taught")));
    ASSERT_NUM(1782000000.0, cJSON_GetObjectItemCaseSensitive(o, "taught_at")->valuedouble);
    TEST_ASSERT_EQUAL_STRING("taught", cJSON_GetObjectItemCaseSensitive(o, "mode")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "live")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "testing")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "checking")));
    ASSERT_NUM(0.75, cJSON_GetObjectItemCaseSensitive(o, "threshold")->valuedouble);
    TEST_ASSERT_EQUAL_INT(90, (int)cJSON_GetObjectItemCaseSensitive(o, "trigger_mg")->valuedouble);
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, "counts");
    TEST_ASSERT_EQUAL_INT(11, (int)cJSON_GetObjectItemCaseSensitive(c, "wom")->valuedouble);
    TEST_ASSERT_EQUAL_INT(7, (int)cJSON_GetObjectItemCaseSensitive(c, "captures")->valuedouble);
    TEST_ASSERT_EQUAL_INT(5, (int)cJSON_GetObjectItemCaseSensitive(c, "matches")->valuedouble);
    TEST_ASSERT_EQUAL_INT(2, (int)cJSON_GetObjectItemCaseSensitive(c, "others")->valuedouble);
    TEST_ASSERT_EQUAL_INT(3, (int)cJSON_GetObjectItemCaseSensitive(c, "reactions")->valuedouble);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "test");
    TEST_ASSERT_EQUAL_INT(4, (int)cJSON_GetObjectItemCaseSensitive(t, "matches")->valuedouble);
    TEST_ASSERT_EQUAL_INT(1, (int)cJSON_GetObjectItemCaseSensitive(t, "others")->valuedouble);
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, "recent");
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(a));
    const cJSON *r0 = cJSON_GetArrayItem(a, 0), *r1 = cJSON_GetArrayItem(a, 1), *r2 = cJSON_GetArrayItem(a, 2);
    TEST_ASSERT_EQUAL_STRING("test", cJSON_GetObjectItemCaseSensitive(r0, "kind")->valuestring);
    ASSERT_NUM(1782000100.0, cJSON_GetObjectItemCaseSensitive(r0, "at")->valuedouble);
    ASSERT_NUM(0.5, cJSON_GetObjectItemCaseSensitive(r0, "dist")->valuedouble);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r0, "match")));
    /* a clock that was not valid and a capture a gate rejected give nulls, never 0 or inf */
    TEST_ASSERT_EQUAL_STRING("live", cJSON_GetObjectItemCaseSensitive(r1, "kind")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(r1, "at")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(r1, "dist")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r1, "match")));
    ASSERT_NUM(1.25, cJSON_GetObjectItemCaseSensitive(r2, "dist")->valuedouble);
    /* the whole answer prints (no inf or nan in the text) */
    char *txt = cJSON_PrintUnformatted(o);
    TEST_ASSERT_NOT_NULL(txt);
    TEST_ASSERT_NULL(strstr(txt, "inf"));
    TEST_ASSERT_NULL(strstr(txt, "nan"));
    free(txt);
    cJSON_Delete(o);
    /* taught with a non-finite threshold: null */
    st.thr = NAN;
    o = hy_api_cabinet_json(&st, NULL, 0);
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(o, "threshold")));
    cJSON_Delete(o);
    TEST_ASSERT_NULL(hy_api_cabinet_json(NULL, NULL, 0));
}

static void test_api_cabinet_event_json(void)
{
    hy_cab_status_t st = cab_status_taught();
    char buf[192];
    int n = hy_api_cabinet_event_json(&st, 0.5f, 0.75f, true, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
    cJSON *o = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_STRING("cabinet", cJSON_GetObjectItemCaseSensitive(o, "ev")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(o, "type"));
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "test");
    TEST_ASSERT_EQUAL_INT(4, (int)cJSON_GetObjectItemCaseSensitive(t, "matches")->valuedouble);
    TEST_ASSERT_EQUAL_INT(1, (int)cJSON_GetObjectItemCaseSensitive(t, "others")->valuedouble);
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(o, "last");
    ASSERT_NUM(0.5, cJSON_GetObjectItemCaseSensitive(l, "dist")->valuedouble);
    ASSERT_NUM(0.75, cJSON_GetObjectItemCaseSensitive(l, "thr")->valuedouble);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(l, "match")));
    cJSON_Delete(o);
    /* a rejected capture and the Any movement trigger (no distance) give nulls */
    n = hy_api_cabinet_event_json(&st, INFINITY, 0.0f, false, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN(0, n);
    o = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(o);
    l = cJSON_GetObjectItemCaseSensitive(o, "last");
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(l, "dist")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(l, "match")));
    cJSON_Delete(o);
    TEST_ASSERT_EQUAL_INT(-1, hy_api_cabinet_event_json(&st, 0.5f, 0.75f, true, buf, 20));
    TEST_ASSERT_EQUAL_INT(-1, hy_api_cabinet_event_json(NULL, 0.5f, 0.75f, true, buf, sizeof buf));
}

static void test_state_json_nulls(void)
{
    hy_api_state_t st;
    memset(&st, 0, sizeof st);
    st.batt_pct = -1;
    strcpy(st.zone, "UTC");
    char *js = hy_api_state_json(&st);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"api\":1"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"utc_ms\":null"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"clock_valid\":false"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"at\":null"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"pct\":null"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"tiles\":[]"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"due\":[]"));
    free(js);

    hy_api_tile_t tl[2];
    memset(tl, 0, sizeof tl);
    tl[0].id = 1; strcpy(tl[0].code, "A"); strcpy(tl[0].name, "Antibiotic"); tl[0].color = 2;
    tl[0].kind = 2; tl[0].ring = 640; tl[0].last_taken = 1793181600LL; tl[0].next_due = 0; tl[0].pills = false;
    tl[1].id = 2; strcpy(tl[1].code, "V"); strcpy(tl[1].name, "Vitamin D"); tl[1].kind = 3; tl[1].ring = 1000;
    tl[1].next_due = 1793185200LL; tl[1].pills = true; tl[1].pills_left = 12;
    hy_api_due_t du = { 2, 1793185200LL, HY_OCC_DUE, 0 };
    st.utc_ms = 1793181600123LL; st.clock_valid = true; st.batt_pct = 80; st.usb = true;
    strcpy(st.sync_src, "browser"); st.sync_at = 1793181000LL;
    strcpy(st.estimate, "2 d");
    st.net_mode = 1; st.net_state = 3; strcpy(st.ssid, "HomeNet"); st.rssi = -58; strcpy(st.ip, "192.168.1.23");
    strcpy(st.host, "hygeia.local");
    st.tiles = tl; st.n_tiles = 2; st.due = &du; st.n_due = 1;
    js = hy_api_state_json(&st);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_NOT_NULL(strstr(js, "\"utc_ms\":1793181600123"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"src\":\"browser\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"pct\":80"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"mode\":\"client\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"state\":\"connected\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"ssid\":\"HomeNet\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"kind\":\"since\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"kind\":\"due\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"next_due\":null"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"last_taken\":null"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"pills_left\":12"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"pills_left\":null"));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"state\":\"due\""));
    TEST_ASSERT_NOT_NULL(strstr(js, "\"snoozed_until\":null"));
    free(js);
    TEST_ASSERT_NULL(hy_api_state_json(NULL));
    st.tiles = NULL;   /* a count without an array is refused */
    TEST_ASSERT_NULL(hy_api_state_json(&st));
}

static void test_event_json_names(void)
{
    static const char *const names[] = { "taken", "skipped", "missed", "due", "snoozed", "undone", "log_edited",
                                         "meds_changed", "untracked", "battery_low", "battery_critical",
                                         "clock_invalid" };
    TEST_ASSERT_EQUAL(HY_EV_CLOCK_INVALID + 1, sizeof names / sizeof names[0]);
    for (int t = HY_EV_TAKEN; t <= HY_EV_CLOCK_INVALID; t++) {
        hy_event_t ev = { (hy_ev_type_t)t, 1793181600LL, 2, 42, 1793181000LL, -7 };
        char buf[160], want[64];
        int n = hy_api_event_json(&ev, buf, sizeof buf);
        TEST_ASSERT_EQUAL((int)strlen(buf), n);
        snprintf(want, sizeof want, "\"ev\":\"%s\"", names[t]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, want), buf);
        TEST_ASSERT_NOT_NULL(strstr(buf, "\"ts\":1793181600"));
        TEST_ASSERT_NOT_NULL(strstr(buf, "\"med_id\":2"));
        TEST_ASSERT_NOT_NULL(strstr(buf, "\"log_id\":42"));
        TEST_ASSERT_NOT_NULL(strstr(buf, "\"slot\":1793181000"));
        TEST_ASSERT_NOT_NULL(strstr(buf, "\"value\":-7"));
    }
    hy_event_t bad = { (hy_ev_type_t)99, 0, 0, 0, 0, 0 };
    char buf[160];
    TEST_ASSERT_EQUAL(-1, hy_api_event_json(&bad, buf, sizeof buf));
    hy_event_t ok = { HY_EV_TAKEN, 1, 2, 3, 4, 5 };
    TEST_ASSERT_EQUAL(-1, hy_api_event_json(&ok, buf, 10));
    TEST_ASSERT_EQUAL(-1, hy_api_event_json(NULL, buf, sizeof buf));
}

/* ---- sample files for the web mock ---- */

static void write_sample(const char *dir, const char *name, const char *text)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fputs(text, f);
    fputc('\n', f);
    fclose(f);
}

static void test_api_samples(void)
{
    const char *dir = getenv("HY_SAMPLES_DIR");
    if (!dir || !*dir) TEST_IGNORE_MESSAGE("HY_SAMPLES_DIR not set");

    hy_med_t m[3] = { mk(1, "A", "Antibiotic"), mk(2, "V", "Vitamin D"), mk(3, "M", "Magnesium") };
    strcpy(m[0].dose, "1 tab"); m[0].color = 1; m[0].type = HY_SCHED_FIXED;
    m[0].times[0] = 8 * 60; m[0].times[1] = 20 * 60; m[0].n_times = 2; m[0].weekdays = 127; m[0].order = 0;
    strcpy(m[1].dose, "2 drops"); m[1].color = 4; m[1].type = HY_SCHED_FIXED;
    m[1].times[0] = 9 * 60; m[1].n_times = 1; m[1].weekdays = 127; m[1].order = 1;
    m[1].pills_enabled = true; m[1].pills_left = 12; m[1].pills_alert = 3;
    strcpy(m[2].dose, "1 cap"); m[2].color = 6; m[2].type = HY_SCHED_AS_NEEDED; m[2].min_interval_min = 240;
    m[2].order = 2;

    char *js = hy_api_meds_json(m, 3);
    TEST_ASSERT_NOT_NULL(js);
    write_sample(dir, "meds.json", js);
    free(js);
    js = hy_api_med_json(&m[0]);
    TEST_ASSERT_NOT_NULL(js);
    write_sample(dir, "med.json", js);
    free(js);

    hy_log_t e[3];
    memset(e, 0, sizeof e);
    for (int i = 0; i < 3; i++) {
        e[i].id = (uint32_t)(i + 1);
        strcpy(e[i].tz, "CET-1CEST,M3.5.0,M10.5.0/3");
        e[i].status = HY_TAKEN;
        e[i].source = i == 1 ? HY_SRC_WEB : HY_SRC_SCREEN;
    }
    e[0].ts = 1793170800LL; e[0].med_id = 1; e[0].slot = 1793170800LL; e[0].delta_min = 0;     /* 08:00 */
    e[1].ts = 1793174700LL; e[1].med_id = 2; e[1].slot = 1793174400LL; e[1].delta_min = 5;     /* 09:05 */
    e[2].ts = 1793181600LL; e[2].med_id = 3; strcpy(e[2].note, "after lunch");
    char lines[3][512], big[2048];
    size_t off = (size_t)snprintf(big, sizeof big, "{\"log\":[");
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(hy_api_log_json(&e[i], lines[i], sizeof lines[i]) > 0);
        off += (size_t)snprintf(big + off, sizeof big - off, "%s%s", i ? "," : "", lines[i]);
    }
    snprintf(big + off, sizeof big - off, "],\"next\":null}");
    write_sample(dir, "log.json", big);

    hy_api_tile_t tl[3];
    memset(tl, 0, sizeof tl);
    tl[0].id = 1; strcpy(tl[0].code, "A"); strcpy(tl[0].name, "Antibiotic"); tl[0].color = 1;
    tl[0].kind = 2 /* since */; tl[0].ring = 420; tl[0].last_taken = 1793170800LL; tl[0].next_due = 1793214000LL;
    tl[1].id = 2; strcpy(tl[1].code, "V"); strcpy(tl[1].name, "Vitamin D"); tl[1].color = 4;
    tl[1].kind = 3 /* due */; tl[1].ring = 1000; tl[1].last_taken = 1793088300LL; tl[1].next_due = 1793174400LL;
    tl[1].pills = true; tl[1].pills_left = 12;
    tl[2].id = 3; strcpy(tl[2].code, "M"); strcpy(tl[2].name, "Magnesium"); tl[2].color = 6;
    tl[2].kind = 0 /* as_needed */; tl[2].ring = 1000; tl[2].last_taken = 1793181600LL;
    hy_api_due_t du = { 2, 1793174400LL, HY_OCC_DUE, 0 };
    hy_api_state_t st;
    memset(&st, 0, sizeof st);
    st.utc_ms = 1793181600123LL; st.clock_valid = true; strcpy(st.zone, "Europe/Zurich");
    strcpy(st.sync_src, "browser"); st.sync_at = 1793181300LL;
    st.batt_pct = 76; st.usb = false; st.charging = false; strcpy(st.estimate, "3 d");
    st.net_mode = 1; st.net_state = 3; strcpy(st.ssid, "HomeNet"); st.rssi = -58;
    strcpy(st.ip, "192.168.1.23"); strcpy(st.host, "hygeia.local");
    st.tiles = tl; st.n_tiles = 3; st.due = &du; st.n_due = 1;
    js = hy_api_state_json(&st);
    TEST_ASSERT_NOT_NULL(js);
    write_sample(dir, "state.json", js);
    free(js);

    hy_settings_t s;
    hy_settings_defaults(&s);
    s.quiet_from = 22 * 60; s.quiet_to = 7 * 60;
    js = hy_api_settings_json(&s);
    TEST_ASSERT_NOT_NULL(js);
    write_sample(dir, "settings.json", js);
    free(js);

    hy_api_err_t err;
    hy_api_err_from(HY_E_NAME, &err);
    char eb[256];
    TEST_ASSERT_TRUE(hy_api_err_json(&err, "The name is too long.", eb, sizeof eb) > 0);
    write_sample(dir, "error.json", eb);
}

/* ---- JSON nesting limit (CJSON_NESTING_LIMIT 16, as on the device) ---- */

/* {"<key>":[[...]]} nested `levels` deep in total (the outer object counts as one level). */
static char *deep_body(const char *key, int levels)
{
    size_t klen = strlen(key);
    char *b = malloc(klen + 5 + 2 * (size_t)levels + 2);
    TEST_ASSERT_NOT_NULL(b);
    size_t k = (size_t)sprintf(b, "{\"%s\":", key);
    for (int i = 1; i < levels; i++) {
        b[k++] = '[';
    }
    for (int i = 1; i < levels; i++) {
        b[k++] = ']';
    }
    b[k++] = '}';
    b[k] = '\0';
    return b;
}

static void test_json_nesting_limit(void)
{
    char *ok = deep_body("x", 16), *deep = deep_body("x", 17), *huge = deep_body("code", 1000);
    cJSON *o = cJSON_Parse(ok);
    TEST_ASSERT_NOT_NULL(o);
    cJSON_Delete(o);
    TEST_ASSERT_NULL(cJSON_Parse(deep));
    TEST_ASSERT_NULL(cJSON_Parse(huge));   /* the shape of a hostile POST /api/pair body */

    /* settings ignore unknown members: 16 levels pass, 17 are refused and leave *out untouched */
    hy_settings_t cur, out;
    hy_api_err_t err;
    hy_settings_defaults(&cur);
    TEST_ASSERT_EQUAL(HY_OK, hy_api_settings_from_json(ok, &cur, &out, &err));
    memset(&out, 0xA5, sizeof out);
    hy_settings_t before;
    memcpy(&before, &out, sizeof out);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_settings_from_json(deep, &cur, &out, &err));
    TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof out);
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_settings_from_json(huge, &cur, &out, &err));

    hy_med_t m;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json(deep, NULL, &m, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_med_from_json(huge, NULL, &m, &err));
    hy_api_logreq_t r;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_logreq_from_json(deep, &r, &err));
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_logreq_from_json(huge, &r, &err));
    hy_api_time_t t;
    TEST_ASSERT_EQUAL(HY_E_INVALID, hy_api_time_from_json(deep, 1700000000LL, &t, &err));
    free(ok);
    free(deep);
    free(huge);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_route_ids);
    RUN_TEST(test_json_nesting_limit);
    RUN_TEST(test_route_table_complete);
    RUN_TEST(test_err_map);
    RUN_TEST(test_err_json);
    RUN_TEST(test_med_roundtrip);
    RUN_TEST(test_med_new_defaults);
    RUN_TEST(test_med_merge_partial);
    RUN_TEST(test_med_ignores_protected);
    RUN_TEST(test_med_bad_utf8);
    RUN_TEST(test_med_duplicate_members);
    RUN_TEST(test_etag_changes_with_any_field);
    RUN_TEST(test_meds_json);
    RUN_TEST(test_order_rules);
    RUN_TEST(test_logq_defaults);
    RUN_TEST(test_logq_bad);
    RUN_TEST(test_page_filter);
    RUN_TEST(test_log_json_local);
    RUN_TEST(test_logreq);
    RUN_TEST(test_csv_formula);
    RUN_TEST(test_log_csv);
    RUN_TEST(test_time_parse);
    RUN_TEST(test_time_before_build);
    RUN_TEST(test_time_unknown_zone);
    RUN_TEST(test_time_auto_drops_zone);
    RUN_TEST(test_time_auto_small_diff);
    RUN_TEST(test_settings_roundtrip);
    RUN_TEST(test_settings_merge_partial);
    RUN_TEST(test_settings_quiet_null);
    RUN_TEST(test_settings_bad_type);
    RUN_TEST(test_settings_clamped);
    RUN_TEST(test_api_settings_cab_roundtrip);
    RUN_TEST(test_api_settings_cab_bad);
    RUN_TEST(test_api_settings_wake_tap);
    RUN_TEST(test_api_settings_double_tap_wake);
    RUN_TEST(test_route_cabinet);
    RUN_TEST(test_routes_integ);
    RUN_TEST(test_api_cabinet_json_not_taught);
    RUN_TEST(test_api_cabinet_json_taught);
    RUN_TEST(test_api_cabinet_event_json);
    RUN_TEST(test_state_json_nulls);
    RUN_TEST(test_event_json_names);
    RUN_TEST(test_api_samples);
    return UNITY_END();
}
