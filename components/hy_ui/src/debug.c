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
#include "hy/gap.h"
#include "hy/occ.h"
#include "hy/sched_run.h"
#include "hy/store.h"
#include "hy/tzlock.h"
#include "hy/ui.h"
#include "ui_internal.h"

static const char *TAG = "ui_debug";

enum { J_TAP, J_UNDO, J_LOGS, J_SEED4, J_SHOT, J_REMINDTEST, J_PERF, J_GESTURE, J_NAV, J_STOREFAIL, J_IDLE, J_TAPSTROKE, J_DUE, J_HISTORY, J_DETAIL, J_NOTE, J_EARLIER, J_QS, J_SETTZ, J_SETTINGS, J_GAPS, J_POWEROFF, J_SCR, J_MF, J_ARCHIVE, J_LIMITS };
typedef struct {
    int kind;
    char arg[96];
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
        ESP_LOGI(TAG, "log id=%u med=%s status=%s source=%s local=%s slot=%s delta=%d note=\"%s\"",
                 (unsigned)buf[i].id, code, st[buf[i].status % 3], src[buf[i].source % 4], t, s,
                 (int)buf[i].delta_min, buf[i].note);
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
 * archived debug medicine, renamed (the table holds 100 medicines including archived ones), else a new one. */
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

static void archive_codes(bool (*match)(const char *), const char *what)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    uint32_t ids[HY_MAX_MEDS];
    size_t k = 0;
    for (size_t i = 0; i < n && k < HY_MAX_MEDS; i++) {
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

/* Largest stored order + 1, as for a new medicine */
static uint16_t next_order(void)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    uint16_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (ms[i].order >= o) {
            o = (uint16_t)(ms[i].order + 1);
        }
    }
    return o;
}

static bool code_in_use(const char *code)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(ms[i].code, code) == 0) {
            return true;
        }
    }
    return false;
}

/* "seed4 arch <n>": adds n new archived example medicines (fresh ids, codes X1.. that no stored record uses), so
 * "50 active plus 50 archived" can be set up. Stops at the stored limit. */
static void seed_archived(int count)
{
    static const char second[] = "123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0";
    int added = 0;
    for (const char *p = "XW"; *p && added < count; p++) {
        for (const char *q = second; *q && added < count; q++) {
            char code[3] = {*p, *q, 0};
            if (code_in_use(code)) {
                continue;
            }
            hy_med_t m;
            memset(&m, 0, sizeof m);
            hy_med_defaults(&m);
            m.id = 0;
            snprintf(m.code, sizeof m.code, "%s", code);
            snprintf(m.name, sizeof m.name, "Zinc");
            m.color = (uint8_t)(added % 10);
            m.type = HY_SCHED_FIXED;
            m.times[0] = 8 * 60;
            m.n_times = 1;
            m.active_from = hy_clock_today_ymd();
            m.archived = true;
            m.order = next_order();
            hy_err_t err = hy_act_med_upsert(&m);
            if (err != HY_OK) {
                ESP_LOGW(TAG, "seed4 arch: %s: %s", code, hy_err_str(err));
                added = count;
                break;
            }
            added++;
        }
    }
    const hy_med_t *all;
    size_t stored = hy_meds(&all);
    ESP_LOGI(TAG, "seed4 arch: %u active, %u stored", (unsigned)hy_meds_active_count(), (unsigned)stored);
}

/* "seed4": adds Z (Zinc), the 4th medicine; "seed4 5" also adds C (Calcium) for a 3rd row, which scrolls.
 * "seed4 <n>" (n >= 6) fills up to n active medicines (at most 50) with Z1..Z9, ZA..ZZ, Z0, then Y1.., four
 * daily times 6 h apart each, the first 60 to 107 min from now: no dose is open while measuring.
 * "seed4 off" archives all of them. "seed4 arch <n>" adds n archived Zinc records (see seed_archived). */
static void job_seed4(const char *arg)
{
    if (strncmp(arg, "arch", 4) == 0 && (arg[4] == 0 || arg[4] == ' ')) {
        int count = atoi(arg + 4);
        if (count < 1) {
            ESP_LOGE(TAG, "usage: seed4 arch <n>");
            return;
        }
        seed_archived(count);
        return;
    }
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
        for (const char *p = "ZY"; *p && !full && (int)hy_meds_active_count() < target; p++) {
            for (const char *q = second; *q && !full && (int)hy_meds_active_count() < target; q++, k++) {
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
        ESP_LOGI(TAG, "seed4: %u active medicines", (unsigned)hy_meds_active_count());
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

/* "archive <code> [yes]": the archive confirmation for the active medicine; yes presses Archive */
static void job_archive(const char *arg)
{
    char code[4] = "", yes[8] = "";
    sscanf(arg, "%3s %7s", code, yes);
    if (!code[0]) {
        ESP_LOGE(TAG, "usage: archive <code> [yes]");
        return;
    }
    uint32_t id;
    if (!home_med_id_by_code(code, &id)) {
        ESP_LOGW(TAG, "no active medicine with code \"%s\"", code);
        return;
    }
    ui_nav_reset();
    ui_open_detail(id);
    ui_nav_push(HY_V_MED_ARCHIVE);
    if (strcmp(yes, "yes") == 0) {
        medarchive_confirm();
    }
}

/* "limits": the active and stored counts against the limits */
static void job_limits(void)
{
    const hy_med_t *ms;
    size_t stored = hy_meds(&ms);
    ESP_LOGI(TAG, "limits: active %u of %d, stored %u of %d", (unsigned)hy_meds_active_count(), HY_MAX_MEDS,
             (unsigned)stored, HY_MAX_STORED);
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
             (unsigned)hy_meds_active_count(), (unsigned)hy_sched_snapshot()->in.n_logs,
             (unsigned)((t1 - t0) / 1000), (unsigned)((t1 - t0) % 1000), (unsigned)(q_us / 1000),
             (unsigned)(q_us % 1000), (unsigned)((t2 - t1) / 1000), (unsigned)((t2 - t1) % 1000));
    uint32_t view_us, lvgl_us;
    home_refresh_timing(&view_us, &lvgl_us);
    ESP_LOGI(TAG, "perf: home_refresh split: tile views %u.%03u ms, LVGL updates %u.%03u ms",
             (unsigned)(view_us / 1000), (unsigned)(view_us % 1000), (unsigned)(lvgl_us / 1000),
             (unsigned)(lvgl_us % 1000));
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

/* The top bar and the overlays live on lv_layer_top, which a snapshot of the screen leaves out: blend a
 * snapshot of the top layer over the RGB565 screen image. */
static void overlay_top_layer(lv_draw_buf_t *dst)
{
    const uint32_t w = HY_SCREEN, h = HY_SCREEN;
    const size_t size = (size_t)w * h * 4;
    uint8_t *mem = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM);
    if (!mem) {
        ESP_LOGW(TAG, "shot: no PSRAM for the top layer, left out");
        return;
    }
    lv_draw_buf_t top;
    lv_draw_buf_init(&top, w, h, LV_COLOR_FORMAT_ARGB8888, w * 4, mem, size);
    if (lv_snapshot_take_to_draw_buf(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888, &top) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "shot: top layer snapshot failed, left out");
        heap_caps_free(mem);
        return;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = top.data + (size_t)y * top.header.stride;
        uint8_t *row = dst->data + (size_t)y * dst->header.stride;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = src + x * 4;   /* B G R A */
            uint32_t a = p[3];
            if (!a) {
                continue;
            }
            uint16_t v = (uint16_t)(row[x * 2] | (row[x * 2 + 1] << 8));
            uint32_t r = ((v >> 11) & 0x1F) * 255 / 31, g = ((v >> 5) & 0x3F) * 255 / 63, b = (v & 0x1F) * 255 / 31;
            r = (p[2] * a + r * (255 - a)) / 255;
            g = (p[1] * a + g * (255 - a)) / 255;
            b = (p[0] * a + b * (255 - a)) / 255;
            v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            row[x * 2] = (uint8_t)v;
            row[x * 2 + 1] = (uint8_t)(v >> 8);
        }
    }
    heap_caps_free(mem);
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
    overlay_top_layer(&db);
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

/* "gesture <name> [consumed]": the same path as a classified touch stroke. With "consumed" a real stroke of that
 * direction is simulated (edge strokes start in their edge zone) and a scrolling widget under the finger asks
 * for it right after the press, as SCROLL_BEGIN does: an edge stroke must still navigate, any other stays. */
static void job_gesture(const char *arg)
{
    char name[12] = "", opt[12] = "";
    sscanf(arg, "%11s %11s", name, opt);
    static const struct { const char *n; int16_t x0, y0, x1, y1; } strokes[] = {
        {"up", 240, 300, 240, 150}, {"down", 240, 150, 240, 300}, {"left", 300, 240, 150, 240},
        {"right", 150, 240, 300, 240}, {"edgeup", 240, 460, 240, 300}, {"edgedown", 240, 10, 240, 150},
        {"edgeright", 10, 240, 150, 240},
    };
    if (strcmp(name, "hold") == 0) {
        input_debug_hold(300, 240);
        return;
    }
    if (strcmp(name, "release") == 0) {
        /* the end point of the named stroke, from the same start as "hold" (300,240): left ends at 150,240 */
        for (size_t k = 0; k < sizeof strokes / sizeof strokes[0]; k++) {
            if (strcmp(opt, strokes[k].n) == 0) {
                input_debug_release(strokes[k].x1, strokes[k].y1);
                return;
            }
        }
        input_debug_release(300, 240);
        return;
    }
    if (strcmp(opt, "consumed") == 0) {
        for (size_t k = 0; k < sizeof strokes / sizeof strokes[0]; k++) {
            if (strcmp(name, strokes[k].n) == 0) {
                input_debug_swipe(strokes[k].x0, strokes[k].y0, strokes[k].x1, strokes[k].y1, 150, true);
                return;
            }
        }
        ESP_LOGE(TAG, "usage: gesture <up|down|left|right|edgeup|edgedown|edgeright> consumed");
        return;
    }
    for (int g = HY_G_TAP; g <= HY_G_EDGE_RIGHT; g++) {
        if (strcmp(name, ui_gesture_name((hy_gesture_t)g)) == 0) {
            ESP_LOGI(TAG, "gesture %s", name);
            ui_nav_gesture((hy_gesture_t)g);
            return;
        }
    }
    ESP_LOGE(TAG, "usage: gesture <tap|long|left|right|up|down|edgedown|edgeup|edgeright> [consumed]");
}

/* "due slide|skip|snooze|next|prev|flash": the same functions the knob, the Skip tap and the swipes call */
static void job_due(const char *arg)
{
    if (strcmp(arg, "slide") == 0) {
        due_slide_complete();
    } else if (strcmp(arg, "skip") == 0) {
        due_skip();
    } else if (strcmp(arg, "snooze") == 0) {
        due_snooze();
    } else if (strcmp(arg, "next") == 0) {
        due_next();
    } else if (strcmp(arg, "prev") == 0) {
        due_prev();
    } else if (strcmp(arg, "drag") == 0 || strcmp(arg, "drop") == 0) {
        due_drag_debug(arg[2] == 'o');
    } else if (strcmp(arg, "flash") == 0) {
        due_flash_debug();
    } else {
        ESP_LOGE(TAG, "usage: due <slide|skip|snooze|next|prev|flash|drag|drop>");
    }
}

/* "history toggle|tap <code>|dbl <code>|hold <code>|perf": the range toggle and the row taps through the input layer.
 * dbl sends two strokes 0 ms apart so the second one is a double tap by hy_gesture_double. */
static void hold_release(lv_timer_t *t)
{
    (void)t;
    history_debug_press(false);
    ESP_LOGI(TAG, "history hold: released");
}

static void job_history(const char *arg)
{
    char cmd[8] = "", code[4] = "";
    sscanf(arg, "%7s %3s", cmd, code);
    uint32_t id = 0;
    bool row = code[0] && home_med_id_by_code(code, &id);
    if (strcmp(cmd, "toggle") == 0) {
        history_toggle_range();
    } else if (strcmp(cmd, "tap") == 0 || strcmp(cmd, "dbl") == 0) {
        if (code[0] && !row) {
            ESP_LOGW(TAG, "history: no active medicine with code \"%s\"", code);
            return;
        }
        int n = strcmp(cmd, "dbl") == 0 ? 2 : 1;
        for (int i = 0; i < n; i++) {
            input_debug_stroke(240, 240, 242, 241, i ? 0 : 80);
            history_tap(id);
        }
    } else if (strcmp(cmd, "hold") == 0) {
        /* a row tap, then a finger held on the page for 500 ms: the 300 ms timer waits and fires after the release */
        if (!row) {
            ESP_LOGW(TAG, "history hold: needs a medicine code");
            return;
        }
        input_debug_stroke(240, 240, 242, 241, 80);
        history_tap(id);
        history_debug_press(true);
        lv_timer_t *t = lv_timer_create(hold_release, 500, NULL);
        lv_timer_set_repeat_count(t, 1);
    } else if (strcmp(cmd, "perf") == 0) {
        uint32_t v, l, o;
        history_refresh();
        history_timing(&v, &l);
        overview_refresh();
        overview_timing(&o);
        ESP_LOGI(TAG, "history perf: marks %u us, lvgl %u us (no-op when unchanged); overview %u us", (unsigned)v,
                 (unsigned)l, (unsigned)o);
    } else {
        ESP_LOGE(TAG, "usage: history <toggle|tap [code]|dbl [code]|hold <code>|perf>");
    }
}

/* "detail <code>", "note <code> [text]", "earlier <code> [hh:mm]": the views a long press and their buttons open.
 * The nav stack goes back to the root first so repeated commands do not pile up. With a text or a time the
 * Log button's function runs too; without, the view stays open for a shot. */
static bool open_detail_of(const char *code)
{
    uint32_t id;
    if (!home_med_id_by_code(code, &id)) {
        ESP_LOGW(TAG, "no active medicine with code \"%s\"", code);
        return false;
    }
    ui_nav_reset();
    ui_open_detail(id);
    return true;
}

static void job_detail(const char *arg)
{
    char code[4] = "", opt[8] = "";
    sscanf(arg, "%3s %7s", code, opt);
    if (!code[0]) {
        ESP_LOGE(TAG, "usage: detail <code> [scroll]");
        return;
    }
    if (open_detail_of(code) && strcmp(opt, "scroll") == 0) {
        detail_debug_scroll();
    }
}

static void job_note(const char *arg)
{
    char code[4] = "";
    sscanf(arg, "%3s", code);
    if (!code[0]) {
        ESP_LOGE(TAG, "usage: note <code> [text]");
        return;
    }
    const char *text = strchr(arg, ' ');
    while (text && *text == ' ') {
        text++;
    }
    if (!open_detail_of(code)) {
        return;
    }
    ui_nav_push(HY_V_NOTE);
    if (text && strcmp(text, "!wake") == 0) {
        /* the dimmed screen: a stroke wakes it, then the OK key fires on that stroke. Nothing may be logged. Then
         * a normal stroke and the OK key: that one logs. */
        note_debug_text("wake test");
        ui_nav_set_idle(true);
        input_debug_stroke(240, 240, 242, 241, 80);
        note_debug_ok();
        input_debug_stroke(240, 240, 242, 241, 80);
        note_debug_ok();
    } else if (text && *text) {
        note_debug_text(text);
        note_submit();
    }
}

static void job_earlier(const char *arg)
{
    char code[4] = "";
    int h = -1, m = -1;
    int n = sscanf(arg, "%3s %d:%d", code, &h, &m);
    if (n < 1 || (n > 1 && n < 3)) {
        ESP_LOGE(TAG, "usage: earlier <code> [hh:mm]");
        return;
    }
    if (!open_detail_of(code)) {
        return;
    }
    ui_nav_push(HY_V_EARLIER);
    if (n == 3) {
        earlier_debug_set(h, m);
        earlier_submit();
    }
}

static void job_qs(const char *arg)
{
    static const struct { const char *name; hy_view_t v; } views[] = {
        {"qs", HY_V_QS}, {"settings", HY_V_SETTINGS}, {"zone", HY_V_SET_ZONE}, {"time", HY_V_SET_TIME},
        {"sound", HY_V_SET_SOUND}, {"display", HY_V_SET_DISPLAY}, {"about", HY_V_ABOUT},
        {"invalid", HY_V_CLOCK_INVALID},   /* the root's layout; the root itself follows the clock */
    };
    if (strncmp(arg, "view ", 5) == 0) {
        for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
            if (strcmp(arg + 5, views[i].name) == 0) {
                ui_nav_push(views[i].v);
                return;
            }
        }
        ESP_LOGE(TAG, "qs view: unknown view \"%s\"", arg + 5);
    } else if (arg[0]) {
        qs_debug(arg);
    } else {
        ui_nav_push(HY_V_QS);
    }
}

static void job_settz(const char *iana)
{
    esp_err_t err = settings_set_zone(iana);
    ESP_LOGI(TAG, "settz %s: %s", iana, esp_err_to_name(err));
}

static void job_gaps(void)
{
    enum { GAPS_MAX = 64 };
    static hy_gap_t g[GAPS_MAX + 1];      /* one more than shown: tells whether more exist */
    static uint8_t why[GAPS_MAX + 1];
    if (!hy_clock_valid()) {
        ESP_LOGW(TAG, "gaps: clock invalid");
        return;
    }
    int64_t now = (int64_t)time(NULL);
    size_t n = hy_store_port()->query_gaps(NULL, now - 30 * 86400LL, now + 1, g, why, GAPS_MAX + 1);
    bool more = n > GAPS_MAX;
    if (more) {
        n = GAPS_MAX;
    }
    char tz[HY_TZ_LEN];
    hy_clock_tz_copy(tz, sizeof tz);
    for (size_t i = 0; i < n; i++) {
        char a[32], b[32];
        hy_format_local(g[i].start, tz, a, sizeof a);
        hy_format_local(g[i].end, tz, b, sizeof b);
        ESP_LOGI(TAG, "gap %u: %s .. %s (%s) %lld s", (unsigned)(i + 1), a, b,
                 hy_gap_reason_str((hy_gap_reason_t)why[i]), (long long)(g[i].end - g[i].start));
    }
    ESP_LOGI(TAG, "gaps: %u in the last 30 days%s", (unsigned)n, more ? " (first 64 shown, more exist)" : "");
}

static void run_job(void *arg)
{
    job_t *j = arg;
    nav_host_warm_all();   /* the commands below poke widgets of views that may not be created yet */
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
    case J_GESTURE:
        job_gesture(j->arg);
        break;
    case J_NAV:
        ui_nav_log();
        break;
    case J_IDLE:
        if (strcmp(j->arg, "off") != 0) {
            idle_enter();
        } else {
            idle_wake();
        }
        break;
    case J_TAPSTROKE: {
        /* a still 80 ms stroke through the input layer, then the tile's SHORT_CLICKED handler */
        uint32_t id;
        if (!home_med_id_by_code(j->arg, &id)) {
            ESP_LOGW(TAG, "tapstroke: no active medicine with code \"%s\"", j->arg);
            break;
        }
        input_debug_stroke(240, 240, 242, 241, 80);
        home_tile_click(id);
        break;
    }
    case J_DUE:
        job_due(j->arg);
        break;
    case J_HISTORY:
        job_history(j->arg);
        break;
    case J_DETAIL:
        job_detail(j->arg);
        break;
    case J_NOTE:
        job_note(j->arg);
        break;
    case J_EARLIER:
        job_earlier(j->arg);
        break;
    case J_QS:
        job_qs(j->arg);
        break;
    case J_SETTZ:
        job_settz(j->arg);
        break;
    case J_SETTINGS:
        settings_debug_log();
        break;
    case J_GAPS:
        job_gaps();
        break;
    case J_POWEROFF:
        poweroff_debug(j->arg);
        break;
    case J_SCR:
        screen_debug(j->arg);
        break;
    case J_MF:
        medform_debug(j->arg);
        break;
    case J_ARCHIVE:
        job_archive(j->arg);
        break;
    case J_LIMITS:
        job_limits();
        break;
    case J_STOREFAIL:
        ui_set_storage_down(strcmp(j->arg, "off") != 0);
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
    /* Queued first (the lock is free while LVGL is paused), resumed after: a tick in LVGL's first pass may pause it
     * again, and a job queued after that would wait for the next wake */
    if (!hy_board_lock(1000)) {
        ESP_LOGE(TAG, "lvgl lock timeout");
        free(j);
        return;
    }
    if (lv_async_call(run_job, j) != LV_RESULT_OK) {
        free(j);
    }
    hy_board_unlock();
    hy_power_resume();   /* the LVGL task is paused while the screen sleeps: the job must run now */
}

void hy_ui_debug_tap(const char *code) { post(J_TAP, code); }
void hy_ui_debug_undo(void) { post(J_UNDO, NULL); }
void hy_ui_debug_logs(void) { post(J_LOGS, NULL); }
void hy_ui_debug_seed4(const char *arg) { post(J_SEED4, arg); }
void hy_ui_debug_shot(void) { post(J_SHOT, NULL); }
void hy_ui_debug_remindtest(const char *arg) { post(J_REMINDTEST, arg); }
void hy_ui_debug_perf(void) { post(J_PERF, NULL); }
void hy_ui_debug_gesture(const char *name) { post(J_GESTURE, name); }
void hy_ui_debug_nav(void) { post(J_NAV, NULL); }
void hy_ui_debug_storefail(const char *arg) { post(J_STOREFAIL, arg); }
void hy_ui_debug_idle(const char *arg) { post(J_IDLE, arg); }
void hy_ui_debug_tapstroke(const char *code) { post(J_TAPSTROKE, code); }
void hy_ui_debug_due(const char *arg) { post(J_DUE, arg); }
void hy_ui_debug_history(const char *arg) { post(J_HISTORY, arg); }
void hy_ui_debug_detail(const char *arg) { post(J_DETAIL, arg); }
void hy_ui_debug_note(const char *arg) { post(J_NOTE, arg); }
void hy_ui_debug_earlier(const char *arg) { post(J_EARLIER, arg); }
void hy_ui_debug_qs(const char *arg) { post(J_QS, arg); }
void hy_ui_debug_settz(const char *iana) { post(J_SETTZ, iana); }
void hy_ui_debug_settings(void) { post(J_SETTINGS, NULL); }
void hy_ui_debug_gaps(void) { post(J_GAPS, NULL); }
void hy_ui_debug_poweroff(const char *arg) { post(J_POWEROFF, arg); }
void hy_ui_debug_scr(const char *arg) { post(J_SCR, arg); }
void hy_ui_debug_mf(const char *arg) { post(J_MF, arg); }
void hy_ui_debug_archive(const char *arg) { post(J_ARCHIVE, arg); }
void hy_ui_debug_limits(void) { post(J_LIMITS, NULL); }

#endif /* CONFIG_HY_DEBUG_CONSOLE */
