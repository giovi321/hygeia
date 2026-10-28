#include "sdkconfig.h"

#if CONFIG_HY_DEBUG_CONSOLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "hy/actions.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "hy/occ.h"
#include "hy/store.h"
#include "hy/tzlock.h"
#include "hy/ui.h"
#include "ui_internal.h"

static const char *TAG = "ui_debug";

enum { J_TAP, J_UNDO, J_LOGS, J_SEED4, J_SHOT };
typedef struct {
    int kind;
    char arg[8];
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
        ESP_LOGI(TAG, "log id=%u med=%s status=%s local=%s slot=%s delta=%d",
                 (unsigned)buf[i].id, code, st[buf[i].status % 3], t, s, (int)buf[i].delta_min);
    }
    heap_caps_free(buf);
}

static void seed_one(const char *code, const char *name, uint8_t color, uint16_t minute, uint16_t order)
{
    hy_med_t m;
    memset(&m, 0, sizeof m);
    hy_med_defaults(&m);
    m.id = 0;
    strcpy(m.code, code);
    strcpy(m.name, name);
    m.color = color;
    m.type = HY_SCHED_FIXED;
    m.times[0] = minute;
    m.n_times = 1;
    m.order = order;
    hy_err_t err = hy_act_med_upsert(&m);
    ESP_LOGI(TAG, "seed4: upsert %s err %d (id %u)", code, (int)err, (unsigned)m.id);
}

/* seed4: adds Z (Zinc), the 4th medicine. "seed4 5" also adds C (Calcium) for a 3rd row, which scrolls.
 * "seed4 off" archives Z and C again. */
static void job_seed4(const char *arg)
{
    if (strcmp(arg, "off") == 0) {
        const hy_med_t *ms;
        size_t n = hy_meds(&ms);
        uint32_t ids[HY_MAX_MEDS];
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            if (!ms[i].archived && (strcmp(ms[i].code, "Z") == 0 || strcmp(ms[i].code, "C") == 0)) {
                ids[k++] = ms[i].id;
            }
        }
        for (size_t i = 0; i < k; i++) {
            ESP_LOGI(TAG, "seed4 off: archive %u: err %d", (unsigned)ids[i], (int)hy_act_med_archive(ids[i], true));
        }
        return;
    }
    seed_one("Z", "Zinc", 3, 20 * 60, 3);
    if (strcmp(arg, "5") == 0) {
        seed_one("C", "Calcium", 4, 21 * 60, 4);
    }
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

#endif /* CONFIG_HY_DEBUG_CONSOLE */
