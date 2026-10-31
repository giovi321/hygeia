/* The orientation follows the device. The motion task decides, this file applies in the LVGL task: never while the
   splash runs, a finger is down or the screen is off. The latest request wins. */
#include <stdbool.h>

#include "esp_log.h"
#include "hy/board.h"
#include "hy/motion_run.h"
#include "hy/store.h"
#include "ui_internal.h"

static const char *TAG = "rotate";

#define POLL_MS 50

static bool s_has_pending;
static hy_rot_t s_pending;
static bool s_locked;
static bool s_off;

static void save_rot(hy_rot_t r, bool lock)
{
    hy_settings_t s = *hy_settings();
    if (s.rot == (uint8_t)r && s.rot_lock == lock) {
        return;
    }
    s.rot = (uint8_t)r;
    s.rot_lock = lock;
    hy_settings_update(&s);
}

static void apply(hy_rot_t r, const char *how)
{
    hy_board_set_rotation((uint8_t)r);
    if (hy_board_rotation() != (uint8_t)r) {
        ESP_LOGW(TAG, "%u not applied", (unsigned)r * 90);   /* the panel write failed: the old one stays */
        hy_motion_set_current((hy_rot_t)hy_board_rotation());   /* tell the motion task what the panel shows */
        return;
    }
    hy_motion_set_current(r);
    save_rot(r, s_locked);
    ESP_LOGI(TAG, "%u (%s)", (unsigned)r * 90, how);
}

void rotate_request(hy_rot_t r)
{
    if (s_off || s_locked) {
        return;
    }
    if ((uint8_t)r == hy_board_rotation()) {
        s_has_pending = false;
        return;
    }
    s_pending = r;
    s_has_pending = true;
}

void rotate_poll(void)
{
    if (!s_has_pending || s_off) {
        return;
    }
    if (splash_remaining_ms() > 0) {
        return;
    }
    lv_indev_t *in = hy_board_indev();
    if (in && lv_indev_get_state(in) != LV_INDEV_STATE_RELEASED) {
        return;   /* a stroke is running: the rotation waits for the release */
    }
    hy_rot_t r = s_pending;
    s_has_pending = false;
    if ((uint8_t)r == hy_board_rotation()) {
        return;
    }
    apply(r, "auto");
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    rotate_poll();
}

void rotate_screen_state(hy_scr_t st)
{
    if (st == HY_SCR_OFF) {
        s_off = true;
        s_has_pending = false;
        hy_motion_enable(false);
    } else {
        s_off = false;
        hy_motion_enable(true);
    }
}

void rotate_set_lock(bool on)
{
    s_locked = on;
    if (on) {
        s_has_pending = false;
    }
    save_rot((hy_rot_t)hy_board_rotation(), on);
    hy_motion_set_lock(on);
    if (!on) {
        hy_motion_set_current((hy_rot_t)hy_board_rotation());
    }
}

bool rotate_locked(void)
{
    return s_locked;
}

void rotate_force(hy_rot_t r)
{
    s_has_pending = false;
    if ((uint8_t)r == hy_board_rotation()) {
        hy_motion_set_current(r);
        ESP_LOGI(TAG, "%u (forced, unchanged)", (unsigned)r * 90);
        return;
    }
    apply(r, "forced");
}

/* LVGL task. A shake does what a tap on Undo does, and only while the Undo toast shows. It never wakes the screen. */
void shake_event(void)
{
    if (toast_logged_visible()) {
        ESP_LOGI(TAG, "shake: undo");
        toast_undo_pressed();
    } else {
        ESP_LOGI(TAG, "shake: ignored");
    }
}

void rotate_init(void)
{
    s_locked = hy_settings()->rot_lock;
    s_off = false;
    s_has_pending = false;
    hy_motion_ui_t ui = {.rotate = rotate_request, .shake = shake_event};
    esp_err_t err = hy_motion_init(&ui, (hy_rot_t)hy_board_rotation());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hy_motion_init: %s: the screen will not follow the device", esp_err_to_name(err));
        return;
    }
    hy_motion_set_lock(s_locked);
    hy_motion_enable(true);   /* screen_init sets ON without calling apply() */
    lv_timer_create(poll_cb, POLL_MS, NULL);
}
