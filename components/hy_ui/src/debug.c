#include "sdkconfig.h"

#if CONFIG_HY_DEBUG_CONSOLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "hy/actions.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "hy/occ.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "hy/tzlock.h"
#include "hy/ui.h"
#include "ui_internal.h"

static const char *TAG = "ui_debug";

enum { J_TAP, J_UNDO, J_LOGS, J_SEED4, J_SHOT, J_REMINDTEST, J_PERF };
typedef struct {
    int kind;
    char arg[24];
} job_t;

/* ---- jobs (run in the LVGL task) ---- */

static void job_logs(void)
{
    const hy_clock_port_t *ck = hy_clock_port();
    if (!ck->valid(NULL)) {
        ESP_LOGW(TAG, "logs: clock invalid");
        return;
    }
    time_t now = (time_t)ck->now(NULL);
    struct tm tm;
    hy_tz_lock();                 /* the process TZ is hy_clock's zone outside this lock */
    localtime_r(&now, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_mday -= 1;
    tm.tm_isdst = -1;
    int64_t from = (int64_t)mktime(&tm);
    hy_tz_unlock();

    hy_log_t *buf = heap_caps_malloc(128 * sizeof(hy_log_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "logs: no memory");
        return;
    }
    size_t n = hy_store_port()->query_log(NULL, from, (int64_t)now + 1, 0, buf, 128);
    const hy_med_t *ms;
    size_t nm = hy_meds(&ms);
    ESP_LOGI(TAG, "logs: %u entries since yesterday 00:00 local", (unsigned)n);
    static const char *st[] = {"taken", "skipped", "missed"};
    static const char *src[] = {"screen", "web", "mqtt", "auto"};
    for (size_t i = 0; i < n; i++) {
        const char *code = "?";
        for (size_t k = 0; k < nm; k++) {
            if (ms[k].id == buf[i].med_id) {
                code = ms[k].code;
            }
        }
        char t[32], s[32] = "free";
        hy_format_local(buf[i].ts, buf[i].tz, t, sizeof t);
        if (buf[i].slot) {
            hy_format_local(buf[i].slot, buf[i].tz, s, sizeof s);
        }
        ESP_LOGI(TAG, "log id=%u med=%s status=%s source=%s local=%s slot=%s delta=%d",
                 (unsigned)buf[i].id, code, st[buf[i].status % 3], src[buf[i].source % 4], t, s,
                 (int)buf[i].delta_min);
    }
    heap_caps_free(buf);
}

/* Debug medicine codes: Z (Zinc), C (Calcium), T (Test), U (Test 2) and the bulk codes of "seed4 <n>" */
static bool is_bulk_code(const char *c)
{
    return strlen(c) == 2 && (c[0] == 'Z' || c[0] == 'Y');
}

static bool is_debug_code(const char *c)
{
    return strcmp(c, "Z") == 0 || strcmp(c, "C") == 0 || strcmp(c, "T") == 0 || strcmp(c, "U") == 0 ||
           is_bulk_code(c);
}

static bool is_seed4_code(const char *c)
{
    return strcmp(c, "Z") == 0 || strcmp(c, "C") == 0 || is_bulk_code(c);
}

static bool is_test_code(const char *c)
{
    return strcmp(c, "T") == 0 || strcmp(c, "U") == 0;
}

/* The medicine to reuse for a debug code: the one with that code (active first, then archived), else an
 * archived debug medicine, renamed (the table holds 50 medicines including archived ones), else a new one. */
static void debug_slot(const char *code, hy_med_t *out)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    const hy_med_t *pick = NULL;
    for (size_t i = 0; i < n && !pick; i++) {
        if (!ms[i].archived && strcmp(ms[i].code, code) == 0) {
            pick = &ms[i];
        }
    }
    for (size_t i = 0; i < n && !pick; i++) {
        if (ms[i].archived && strcmp(ms[i].code, code) == 0) {
            pick = &ms[i];
        }
    }
    for (size_t i = 0; i < n && !pick; i++) {
        if (ms[i].archived && is_debug_code(ms[i].code)) {
            pick = &ms[i];
        }
    }
    if (pick) {
        *out = *pick;
        return;
    }
    memset(out, 0, sizeof *out);
    hy_med_defaults(out);
    out->id = 0;
}

/* Fixed daily debug medicine with the given times (ascending), active from today */
static hy_err_t upsert_debug(const char *code, const char *name, uint8_t color, const uint16_t *times,
                             uint8_t n_times, uint16_t order, uint16_t late, uint16_t missed, uint32_t *id)
{
    hy_med_t m;
    debug_slot(code, &m);
    snprintf(m.code, sizeof m.code, "%s", code);
    snprintf(m.name, sizeof m.name, "%s", name);
    m.color = color;
    m.type = HY_SCHED_FIXED;
    memset(m.times, 0, sizeof m.times);
    memcpy(m.times, times, n_times * sizeof times[0]);
    m.n_times = n_times;
    m.weekdays = 0x7F;
    m.early_window_min = 60;
    m.late_after_min = late;
    m.missed_after_min = missed;
    m.min_interval_min = 0;
    m.pills_enabled = false;
    m.active_from = hy_clock_today_ymd();
    m.active_until = 0;
    m.archived = false;
    m.order = order;
    hy_err_t err = hy_act_med_upsert(&m);
    if (id) {
        *id = m.id;
    }
    return err;
}

static size_t active_count(void)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms), k = 0;
    for (size_t i = 0; i < n; i++) {
        k += !ms[i].archived;
    }
    return k;
}

static void archive_codes(bool (*match)(const char *), const char *what)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    uint32_t ids[HY_MAX_MEDS];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (!ms[i].archived && match(ms[i].code)) {
            ids[k++] = ms[i].id;
        }
    }
    for (size_t i = 0; i < k; i++) {
        hy_err_t err = hy_act_med_archive(ids[i], true);
        ESP_LOGI(TAG, "%s: archive %u: %s", what, (unsigned)ids[i], hy_err_str(err));
    }
}

/* Local minute of the day (0..1439) of ts in the clock zone */
static uint16_t local_minute(int64_t ts)
{
    time_t t = (time_t)ts;
    struct tm tm;
    hy_tz_lock();                 /* the process TZ is hy_clock's zone outside this lock */
    localtime_r(&t, &tm);
    hy_tz_unlock();
    return (uint16_t)(tm.tm_hour * 60 + tm.tm_min);
}

static void sort_times(uint16_t *t, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        uint16_t c = t[i];
        size_t j = i;
        while (j > 0 && t[j - 1] > c) {
            t[j] = t[j - 1];
            j--;
        }
        t[j] = c;
    }
}

/* "seed4": adds Z (Zinc), the 4th medicine; "seed4 5" also adds C (Calcium) for a 3rd row, which scrolls.
 * "seed4 <n>" (n >= 6) fills up to n active medicines (at most 50) with Z1..Z9, ZA..ZZ, Z0, then Y1.., four
 * daily times 6 h apart each, the first 60 to 107 min from now: no dose is open while measuring.
 * "seed4 off" archives all of them. */
static void job_seed4(const char *arg)
{
    if (strcmp(arg, "off") == 0) {
        archive_codes(is_seed4_code, "seed4 off");
        return;
    }
    int target = atoi(arg);
    if (target >= 6) {
        if (target > HY_MAX_MEDS) {
            target = HY_MAX_MEDS;
        }
        const hy_clock_port_t *ck = hy_clock_port();
        if (!ck->valid(NULL)) {
            ESP_LOGW(TAG, "seed4: clock invalid");
            return;
        }
        static const char second[] = "123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0";
        uint16_t first = (uint16_t)((local_minute(ck->now(NULL)) + 60) % 1440);
        size_t k = 0;
        bool full = false;
        for (const char *p = "ZY"; *p && !full && (int)active_count() < target; p++) {
            for (const char *q = second; *q && !full && (int)active_count() < target; q++, k++) {
                char code[3] = {*p, *q, 0};
                uint16_t times[4];
                for (int i = 0; i < 4; i++) {
                    times[i] = (uint16_t)((first + k + i * 360) % 1440);
                }
                sort_times(times, 4);
                hy_err_t err = upsert_debug(code, "Zinc", (uint8_t)(k % 10), times, 4, (uint16_t)(100 + k),
                                            30, 120, NULL);
                if (err != HY_OK) {
                    ESP_LOGW(TAG, "seed4: %s: %s", code, hy_err_str(err));
                    full = err == HY_E_FULL;
                }
            }
        }
        ESP_LOGI(TAG, "seed4: %u active medicines", (unsigned)active_count());
        return;
    }
    static const uint16_t z_time = 20 * 60, c_time = 21 * 60;
    hy_err_t err = upsert_debug("Z", "Zinc", 3, &z_time, 1, 3, 30, 120, NULL);
    ESP_LOGI(TAG, "seed4: upsert Z: %s", hy_err_str(err));
    if (strcmp(arg, "5") == 0) {
        err = upsert_debug("C", "Calcium", 4, &c_time, 1, 4, 30, 120, NULL);
        ESP_LOGI(TAG, "seed4: upsert C: %s", hy_err_str(err));
    }
}

/* "remindtest <minutes> [count] [miss2]": T "Test" (colour 9), with count 2 also U "Test 2" (colour 8),
 * fixed daily at now + minutes rounded down to the minute, active from today; miss2: late after 1 min,
 * missed after 2. "remindtest off" archives both. */
static void job_remindtest(const char *arg)
{
    if (strcmp(arg, "off") == 0) {
        archive_codes(is_test_code, "remindtest off");
        return;
    }
    char extra[8] = "";
    int minutes = -1, count = 1;
    int n = sscanf(arg, "%d %d %7s", &minutes, &count, extra);
    if (n < 1 || minutes < 0 || minutes > 1439 || count < 1 || count > 2 ||
        (n == 3 && strcmp(extra, "miss2") != 0)) {
        ESP_LOGE(TAG, "usage: remindtest <minutes> [count 1|2] [miss2] | remindtest off");
        return;
    }
    bool miss2 = n == 3;
    const hy_clock_port_t *ck = hy_clock_port();
    if (!ck->valid(NULL)) {
        ESP_LOGW(TAG, "remindtest: clock invalid");
        return;
    }
    uint16_t minute = local_minute(ck->now(NULL) + (int64_t)minutes * 60);
    uint16_t late = miss2 ? 1 : 30, missed = miss2 ? 2 : 120;
    uint32_t id = 0;
    hy_err_t err = upsert_debug("T", "Test", 9, &minute, 1, 90, late, missed, &id);
    ESP_LOGI(TAG, "remindtest: T at %02u:%02u (late %u, missed %u min): %s, id %u", minute / 60, minute % 60,
             late, missed, hy_err_str(err), (unsigned)id);
    if (count == 2) {
        err = upsert_debug("U", "Test 2", 8, &minute, 1, 91, late, missed, &id);
        ESP_LOGI(TAG, "remindtest: U at %02u:%02u: %s, id %u", minute / 60, minute % 60, hy_err_str(err),
                 (unsigned)id);
    }
}

/* "perf": one forced evaluation and one home_refresh, timed */
static void job_perf(void)
{
    hy_sched_run_invalidate();
    int64_t t0 = esp_timer_get_time();
    hy_sched_run_tick();
    int64_t t1 = esp_timer_get_time();
    home_refresh(false);
    int64_t t2 = esp_timer_get_time();
    uint32_t q_us = hy_sched_run_query_us();
    ESP_LOGI(TAG, "perf: %u active medicines, %u logs: evaluation %u.%03u ms (log query %u.%03u ms), "
             "home_refresh %u.%03u ms",
             (unsigned)active_count(), (unsigned)hy_sched_snapshot()->in.n_logs,
             (unsigned)((t1 - t0) / 1000), (unsigned)((t1 - t0) % 1000), (unsigned)(q_us / 1000),
             (unsigned)(q_us % 1000), (unsigned)((t2 - t1) / 1000), (unsigned)((t2 - t1) % 1000));
    ESP_LOGI(TAG, "perf: heap internal free %u (min %u), PSRAM free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void emit_line(const uint8_t *p, size_t n, uint32_t *crc)
{
    char out[80];
    size_t o = 0;
    *crc = esp_rom_crc32_le(*crc, p, n);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) {
            v |= (uint32_t)p[i + 1] << 8;
        }
        if (i + 2 < n) {
            v |= p[i + 2];
        }
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? b64[v & 63] : '=';
    }
    out[o++] = '\n';
    out[o] = 0;
    fputs(out, stdout);
}

static void job_shot(void)
{
    const uint32_t w = HY_SCREEN, h = HY_SCREEN;
    const size_t size = (size_t)w * h * 2;
    uint8_t *mem = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM);
    if (!mem) {
        ESP_LOGE(TAG, "shot: no PSRAM for %u bytes", (unsigned)size);
        return;
    }
    lv_draw_buf_t db;
    lv_draw_buf_init(&db, w, h, LV_COLOR_FORMAT_RGB565, w * 2, mem, size);
    lv_result_t r = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &db);
    if (r != LV_RESULT_OK) {
        ESP_LOGE(TAG, "shot: snapshot failed (%d)", (int)r);
        heap_caps_free(mem);
        return;
    }
    uint32_t stride = db.header.stride;
    uint32_t crc = 0;
    printf("\nSHOT_BEGIN %u %u RGB565\n", (unsigned)w, (unsigned)h);
    /* 48 raw bytes per line; rows are w*2 = 960 bytes, a multiple of 48 */
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = db.data + (size_t)y * stride;
        for (uint32_t x = 0; x < w * 2; x += 48) {
            emit_line(row + x, 48, &crc);
        }
    }
    printf("SHOT_END %08x\n", (unsigned)crc);
    fflush(stdout);
    heap_caps_free(mem);
}

static void run_job(void *arg)
{
    job_t *j = arg;
    switch (j->kind) {
    case J_TAP: {
        uint32_t id;
        if (home_med_id_by_code(j->arg, &id)) {
            ESP_LOGI(TAG, "tap %s", j->arg);
            home_tap_med(id);
        } else {
            ESP_LOGW(TAG, "tap: no active medicine with code \"%s\"", j->arg);
        }
        break;
    }
    case J_UNDO:
        if (!toast_undo_pressed()) {
            ESP_LOGW(TAG, "undo: no Undo toast visible");
        }
        break;
    case J_LOGS:
        job_logs();
        break;
    case J_SEED4:
        job_seed4(j->arg);
        break;
    case J_SHOT:
        job_shot();
        break;
    case J_REMINDTEST:
        job_remindtest(j->arg);
        break;
    case J_PERF:
        job_perf();
        break;
    }
    free(j);
}

static void post(int kind, const char *arg)
{
    job_t *j = calloc(1, sizeof *j);
    if (!j) {
        return;
    }
    j->kind = kind;
    if (arg) {
        snprintf(j->arg, sizeof j->arg, "%s", arg);
    }
    if (!hy_board_lock(1000)) {
        ESP_LOGE(TAG, "lvgl lock timeout");
        free(j);
        return;
    }
    if (lv_async_call(run_job, j) != LV_RESULT_OK) {
        free(j);
    }
    hy_board_unlock();
}

void hy_ui_debug_tap(const char *code) { post(J_TAP, code); }
void hy_ui_debug_undo(void) { post(J_UNDO, NULL); }
void hy_ui_debug_logs(void) { post(J_LOGS, NULL); }
void hy_ui_debug_seed4(const char *arg) { post(J_SEED4, arg); }
void hy_ui_debug_shot(void) { post(J_SHOT, NULL); }
void hy_ui_debug_remindtest(const char *arg) { post(J_REMINDTEST, arg); }
void hy_ui_debug_perf(void) { post(J_PERF, NULL); }

#endif /* CONFIG_HY_DEBUG_CONSOLE */
