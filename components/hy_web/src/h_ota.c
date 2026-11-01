/* Firmware update: POST /api/ota with the application image (the .bin of the app, not the merged flash image) as the
 * raw request body.
 *
 * The httpd task checks the length against the update slot (413), the battery (409 "battery" on battery below 30 %)
 * and that the running image is already confirmed, then the upload detaches to the backup worker slot (h_backup.c):
 * other requests are served while it runs, and an upload, an export and an import exclude each other (409 "busy").
 *
 * The worker reads the body in 4 KB PSRAM chunks, with the server's body rules: two receive timeouts in a row or the
 * overall deadline end the upload with 408. The first chunk must hold the image header, the first segment header and
 * the application description of a hygeia image built for this chip; anything else answers 422 "not_hygeia" and
 * nothing is written to flash. The chunks then go to esp_ota_write in order (each sector is erased when it is
 * reached), esp_ota_end verifies the whole image (segments and the appended SHA-256; 422 "corrupt"), and the slot
 * becomes the boot slot. The answer is {"reboot_in_s":2} and the device restarts 2 s later.
 *
 * The new image boots pending verification. app_main confirms it once the store, the screen and the first scheduler
 * pass have run, within 150 s or it restarts; a reset before that (crash, watchdog, that deadline, power) makes the
 * bootloader start the previous slot. */

#include <stdatomic.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hy/fwimg.h"
#include "hy/net.h"
#include "hy/power_run.h"
#include "web_internal.h"

static const char *TAG = "web_ota";

#define OTA_CHUNK        4096
#define OTA_DEADLINE_US  (300 * 1000000LL)   /* the whole body; a stalled client ends sooner (two 5 s timeouts) */
#define OTA_MIN_BATT_PCT 30
#define OTA_RESTART_MS   2000
#define OTA_ACTIVITY     (64 * 1024)         /* restart the battery idle timer of the radio every 64 KB */

static atomic_bool s_restarting;

bool web_ota_restarting(void)
{
    return atomic_load(&s_restarting);
}

/* ---------------------------------------------------------------- httpd task */

static bool battery_low(void)
{
    hy_power_state_t ps;
    hy_power_state(&ps);
    /* an unknown percent (-1) on battery counts as low */
    return ps.pmu_ok && !ps.batt.raw.vbus && ps.batt.pct < OTA_MIN_BATT_PCT;
}

static bool running_unconfirmed(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t web_ota_handler(web_req_t *r)
{
    size_t len = r->req->content_len;
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (atomic_load(&s_restarting)) {
        return web_refuse(r, 409, "busy", NULL, "A firmware update finished, the device is restarting");
    }
    if (len == 0) {
        return web_refuse(r, 400, "invalid", "body", "Send the firmware file as the request body");
    }
    if (!next) {
        return web_refuse(r, 503, "storage", NULL, "No firmware slot to write to");
    }
    if (len > next->size) {
        return web_refuse(r, 413, "too_large", NULL, "The file is larger than the firmware slot");
    }
    if (running_unconfirmed()) {
        return web_refuse(r, 503, "busy", NULL, "The device is still checking its own firmware, try again shortly");
    }
    if (battery_low()) {
        return web_refuse(r, 409, "battery", NULL, "Battery below 30 %: connect USB before updating");
    }
    return web_ota_start(r, len);
}

/* ---------------------------------------------------------------- worker */

typedef enum { RD_OK = 0, RD_TIMEOUT, RD_SOCK, RD_ABORT } rd_t;

static rd_t recv_full(httpd_req_t *req, uint8_t *buf, size_t n, int64_t deadline)
{
    size_t got = 0;
    int timeouts = 0;
    while (got < n) {
        if (web_backup_aborting()) {
            return RD_ABORT;
        }
        if (esp_timer_get_time() > deadline) {
            return RD_TIMEOUT;
        }
        int k = httpd_req_recv(req, (char *)buf + got, n - got);
        if (k == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= 2) {
                return RD_TIMEOUT;
            }
            continue;
        }
        if (k <= 0) {
            return RD_SOCK;
        }
        timeouts = 0;
        got += (size_t)k;
    }
    return RD_OK;
}

static void answer(httpd_req_t *req, int status, const char *code, const char *msg)
{
    web_req_t r = {.req = req, .sess = -1};
    if (status == 503) {
        httpd_resp_set_hdr(req, "Retry-After", "2");
    }
    web_send_err(&r, status, code, NULL, msg);
}

/* The body stopped arriving or the server is stopping: answer when there is something to say, then close. */
static void read_failed(httpd_req_t *req, rd_t rd)
{
    if (rd == RD_TIMEOUT) {
        answer(req, 408, "timeout", "The file did not arrive in time");
    } else if (rd == RD_ABORT) {
        answer(req, 503, "busy", "The server is stopping");
    }
    ESP_LOGW(TAG, "upload ended: %s", rd == RD_TIMEOUT ? "timeout" : rd == RD_ABORT ? "server stopping" : "socket");
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
}

/* An error with part of the body still unread: read and drop the rest so the client sees the answer (a client that
 * is still sending often misses an answer followed by a close), then answer. */
static void refuse_rest(httpd_req_t *req, uint8_t *buf, size_t left, int64_t deadline, int status, const char *code,
                        const char *msg)
{
    while (left) {
        size_t n = left < OTA_CHUNK ? left : OTA_CHUNK;
        rd_t rd = recv_full(req, buf, n, deadline);
        if (rd != RD_OK) {
            read_failed(req, rd);
            return;
        }
        left -= n;
    }
    answer(req, status, code, msg);
}

/* The restart runs in the LVGL task, the only task that writes the store, so no medicine or log write is cut
 * halfway. Without the screen task (or when it is busy for a second) it restarts from here. */
static void restart_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "restarting into the new firmware");
    if (!web_ui_restart()) {
        esp_restart();
    }
}

static void schedule_restart(void)
{
    const esp_timer_create_args_t a = {.callback = restart_cb, .name = "ota_restart"};
    esp_timer_handle_t t = NULL;
    if (esp_timer_create(&a, &t) == ESP_OK && esp_timer_start_once(t, OTA_RESTART_MS * 1000ULL) == ESP_OK) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(OTA_RESTART_MS));   /* no timer: restart from the worker */
    restart_cb(NULL);
}

void web_ota_run(httpd_req_t *req, size_t len)
{
    int64_t t0 = esp_timer_get_time();
    int64_t deadline = t0 + OTA_DEADLINE_US;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    uint8_t *buf = heap_caps_malloc(OTA_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char version[32];
    hy_fwimg_t *w = NULL;
    hy_fwimg_res_t fr;

    if (!buf || !part || hy_fwimg_begin(len, NULL, &w) != HY_FWIMG_OK) {
        answer(req, 503, "busy", "Not enough memory for the update");
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
        goto out;
    }

    /* First chunk: checked before anything is written */
    size_t n = len < OTA_CHUNK ? len : OTA_CHUNK;
    rd_t rd = recv_full(req, buf, n, deadline);
    if (rd != RD_OK) {
        read_failed(req, rd);
        goto out;
    }
    size_t done = n;
    bool first = true;
    size_t next_activity = OTA_ACTIVITY;
    for (;;) {
        fr = hy_fwimg_write(w, buf, n);
        if (fr != HY_FWIMG_OK) {
            w = NULL;   /* the writer is freed on any error */
            if (fr == HY_FWIMG_NOT_HYGEIA) {
                refuse_rest(req, buf, len - done, deadline, 422, "not_hygeia", "This file is not a hygeia firmware image");
            } else if (fr == HY_FWIMG_SELF_PENDING) {
                refuse_rest(req, buf, len - done, deadline, 503, "busy",
                            "The device is still checking its own firmware, try again shortly");
            } else if (fr == HY_FWIMG_NO_MEM) {
                refuse_rest(req, buf, len - done, deadline, 503, "busy", "Not enough memory for the update");
            } else if (first) {
                refuse_rest(req, buf, len - done, deadline, 500, "flash", "The firmware slot could not be prepared");
            } else {
                refuse_rest(req, buf, len - done, deadline, 500, "flash", "Writing the firmware failed");
            }
            goto out;
        }
        if (first) {
            first = false;
            web_ui_toast_info("Updating firmware");
        }
        if (done == len) {
            break;
        }
        if (done >= next_activity) {
            hy_net_activity();   /* a long upload on battery must not hit the radio's idle limit */
            next_activity += OTA_ACTIVITY;
        }
        n = len - done < OTA_CHUNK ? len - done : OTA_CHUNK;
        rd = recv_full(req, buf, n, deadline);
        if (rd != RD_OK) {
            read_failed(req, rd);
            goto out;
        }
        done += n;
    }

    fr = hy_fwimg_finish(w, NULL, version);   /* frees the writer whatever the result */
    w = NULL;
    if (fr == HY_FWIMG_CORRUPT) {
        answer(req, 422, "corrupt", "The firmware file is damaged or incomplete");
        web_ui_toast_info("Firmware update failed");
        goto out;
    }
    if (fr == HY_FWIMG_NO_MEM) {
        answer(req, 503, "busy", "Not enough memory for the update");
        web_ui_toast_info("Firmware update failed");
        goto out;
    }
    if (fr != HY_FWIMG_OK) {
        answer(req, 500, "flash", "The firmware could not be finished");
        web_ui_toast_info("Firmware update failed");
        goto out;
    }

    atomic_store(&s_restarting, true);
    web_req_t r = {.req = req, .sess = -1};
    char body[32];
    snprintf(body, sizeof body, "{\"reboot_in_s\":%d}", OTA_RESTART_MS / 1000);
    web_send_json(&r, 200, body);
    ESP_LOGI(TAG, "firmware %s written to %s in %lld ms, restart in %d s (worker stack left %u bytes)", version,
             part->label, (long long)((esp_timer_get_time() - t0) / 1000), OTA_RESTART_MS / 1000,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    web_ui_toast_info("Firmware updated, restarting");
    schedule_restart();

out:
    hy_fwimg_abort(w);
    heap_caps_free(buf);
}
