#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "hy/api.h"
#include "hy/codec.h"
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_route_ids);
    RUN_TEST(test_route_table_complete);
    RUN_TEST(test_err_map);
    RUN_TEST(test_err_json);
    RUN_TEST(test_med_roundtrip);
    RUN_TEST(test_med_new_defaults);
    RUN_TEST(test_med_merge_partial);
    RUN_TEST(test_med_ignores_protected);
    RUN_TEST(test_med_bad_utf8);
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
    return UNITY_END();
}
