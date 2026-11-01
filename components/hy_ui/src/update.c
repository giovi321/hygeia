/* Firmware update view (Settings > About > Check for update).
 * - Shows the state of hy_update: checking, up to date, available (with a confirm page), downloading with a bar,
 *   verifying, restarting, or a short sentence for the error. One button at the bottom follows the state.
 * - hy_update calls back from its own task; the callback only queues one lv_async_call under the board lock. A 1 s
 *   timer repeats the sync so a dropped call never leaves the view stale.
 * - An install that is not on screen (started from the web UI, or after leaving this view) is told by toasts: it
 *   started, it failed, it finished and the device restarts. Leaving the view does not stop a download. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "hy/board.h"
#include "hy/update.h"
#include "ui_internal.h"

static const char *TAG = "ui_update";

#define BTN_H        66
#define BTN_Y        (HY_CONTENT_H - BTN_H - 18)
#define COL_W        (HY_SCREEN - 2 * 24)
#define BAR_W        400
#define BAR_H        12
#define BAR_BG       0x24242B
#define BAR_FG       0xD0646B
#define RECENT_S     60

typedef enum { ACT_NONE, ACT_CHECK, ACT_INSTALL } act_t;

static lv_obj_t *s_root, *s_col, *s_l1, *s_l2, *s_bar, *s_btn, *s_btn_text;
static lv_obj_t *s_conf, *s_conf_text;
static hy_fw_status_t s_shown_st;
static bool s_have;                 /* s_shown_st is what the widgets show */
static bool s_confirm;              /* the confirm page is open */
static bool s_want_check;           /* the next show starts a check (opened from About) */
static bool s_installing;           /* an install was seen running: its outcome gets a toast when not on screen */
static bool s_async_queued;
static act_t s_act;

#if CONFIG_HY_DEBUG_CONSOLE
static bool s_forced;
static hy_fw_status_t s_forced_st;
#endif

static int64_t now_s(void)
{
    return esp_timer_get_time() / 1000000;
}

static bool on_screen(void)
{
    return s_root && ui_current_view() == HY_V_UPDATE;
}

static const char *err_text(hy_fw_err_t e)
{
    switch (e) {
    case HY_FWE_NO_NETWORK:
        return HY_S_UPD_E_NO_NETWORK;
    case HY_FWE_NO_INTERNET:
        return HY_S_UPD_E_NO_INTERNET;
    case HY_FWE_TLS:
        return HY_S_UPD_E_TLS;
    case HY_FWE_NO_RELEASE:
        return HY_S_UPD_E_NO_RELEASE;
    case HY_FWE_BAD_RELEASE:
    case HY_FWE_MISMATCH:
    case HY_FWE_CHECKSUM:
    case HY_FWE_CORRUPT:
        return HY_S_UPD_E_INVALID;
    case HY_FWE_FLASH:
        return HY_S_UPD_E_FLASH;
    case HY_FWE_TIMEOUT:
        return HY_S_UPD_E_TIMEOUT;
    case HY_FWE_BATTERY:
        return HY_S_UPD_E_BATTERY;
    case HY_FWE_MEMORY:
        return HY_S_UPD_E_MEMORY;
    case HY_FWE_STALE:
        return HY_S_UPD_E_STALE;
    case HY_FWE_BUSY:
    case HY_FWE_SELF_PENDING:
    default:
        return HY_S_UPD_E_BUSY;
    }
}

static bool same(const hy_fw_status_t *a, const hy_fw_status_t *b)
{
    return a->state == b->state && a->err == b->err && a->pct == b->pct && a->installed_release == b->installed_release &&
           strcmp(a->installed, b->installed) == 0 && strcmp(a->latest, b->latest) == 0;
}

static void set_button(act_t act, const char *text)
{
    s_act = act;
    if (act == ACT_NONE) {
        lv_obj_set_hidden(s_btn, true);
        return;
    }
    lv_label_set_text(s_btn_text, text);
    lv_obj_center(s_btn_text);
    lv_obj_set_hidden(s_btn, false);
}

static void set_lines(const char *a, uint32_t a_color, const char *b)
{
    lv_label_set_text(s_l1, a);
    lv_obj_set_style_text_color(s_l1, lv_color_hex(a_color), 0);
    lv_label_set_text(s_l2, b);
    lv_obj_set_hidden(s_l2, b[0] == '\0');
}

static void render(const hy_fw_status_t *st)
{
    char a[96], b[96];
    a[0] = b[0] = '\0';
    uint32_t color = HY_C_TEXT;
    bool bar = false;
    act_t act = ACT_NONE;
    const char *btn = HY_S_UPD_CHECK_AGAIN;
    if (st->state != HY_FWS_AVAILABLE) {
        s_confirm = false;
    }
    switch (st->state) {
    case HY_FWS_CHECKING:
        snprintf(a, sizeof a, "%s", HY_S_UPD_CHECKING);
        break;
    case HY_FWS_UP_TO_DATE:
        snprintf(a, sizeof a, "%s", HY_S_UPD_LATEST);
        snprintf(b, sizeof b, HY_S_UPD_VERSION, st->installed);
        act = ACT_CHECK;
        break;
    case HY_FWS_AVAILABLE:
        snprintf(a, sizeof a, HY_S_UPD_AVAILABLE, st->latest);
        snprintf(b, sizeof b, st->installed_release ? HY_S_UPD_INSTALLED : HY_S_UPD_INSTALLED_DEV, st->installed);
        act = ACT_INSTALL;
        btn = HY_S_UPD_INSTALL;
        break;
    case HY_FWS_DOWNLOADING:
        snprintf(a, sizeof a, HY_S_UPD_DOWNLOADING, (unsigned)st->pct);
        bar = true;
        break;
    case HY_FWS_VERIFYING:
        snprintf(a, sizeof a, "%s", HY_S_UPD_VERIFYING);
        break;
    case HY_FWS_RESTARTING:
        snprintf(a, sizeof a, "%s", HY_S_UPD_RESTARTING);
        break;
    case HY_FWS_FAILED:
        snprintf(a, sizeof a, "%s", err_text(st->err));
        color = HY_C_AMBER;
        act = ACT_CHECK;
        break;
    case HY_FWS_IDLE:
    default:
        snprintf(a, sizeof a, HY_S_UPD_VERSION, st->installed);
        act = ACT_CHECK;
        break;
    }
    set_lines(a, color, b);
    if (bar) {
        lv_bar_set_value(s_bar, st->pct, LV_ANIM_OFF);
    }
    lv_obj_set_hidden(s_bar, !bar);
    if (st->state == HY_FWS_AVAILABLE) {
        char c[128];
        snprintf(c, sizeof c, HY_S_UPD_CONFIRM, st->latest);
        lv_label_set_text(s_conf_text, c);
    }
    lv_obj_set_hidden(s_conf, !s_confirm);
    lv_obj_set_hidden(s_col, s_confirm);
    if (s_confirm) {
        set_button(ACT_NONE, "");
    } else {
        set_button(act, btn);
    }
    s_shown_st = *st;
    s_have = true;
}

/* The status the screen follows: the real one, or the forced one of the debug command */
static void current(hy_fw_status_t *st)
{
#if CONFIG_HY_DEBUG_CONSOLE
    if (s_forced) {
        *st = s_forced_st;
        return;
    }
#endif
    hy_update_status(st);
}

static void toasts(const hy_fw_status_t *st)
{
    bool running = st->state == HY_FWS_DOWNLOADING || st->state == HY_FWS_VERIFYING;
    if (running) {
        if (!s_installing) {
            s_installing = true;
            if (!on_screen()) {
                toast_show_info(HY_S_UPD_TOAST_START);
            }
        }
        return;
    }
    if (!s_installing) {
        return;
    }
    s_installing = false;
    if (st->state == HY_FWS_RESTARTING) {
        if (!on_screen()) {
            toast_show_info(HY_S_UPD_TOAST_DONE);
        }
    } else if (st->state == HY_FWS_FAILED && !on_screen()) {
        toast_show_info(HY_S_UPD_TOAST_FAIL);
    }
}

static void sync(void)
{
    hy_fw_status_t st;
    current(&st);
#if CONFIG_HY_DEBUG_CONSOLE
    if (!s_forced)
#endif
        toasts(&st);
    if (on_screen() && (!s_have || !same(&s_shown_st, &st) || s_confirm == lv_obj_is_hidden(s_conf))) {
        render(&st);
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    sync();
}

static void async_cb(void *arg)
{
    (void)arg;
    s_async_queued = false;
    sync();
}

/* hy_update subscriber: the update task. Does not block for long and keeps at most one call queued. */
static void upd_changed(const hy_fw_status_t *st, void *ctx)
{
    (void)st;
    (void)ctx;
    if (!hy_board_lock(50)) {
        return;   /* the 1 s timer catches up */
    }
    if (!s_async_queued && lv_async_call(async_cb, NULL) == LV_RESULT_OK) {
        s_async_queued = true;
    }
    hy_board_unlock();
}

void upd_ui_init(void)
{
    if (hy_update_subscribe(upd_changed, NULL) < 0) {
        ESP_LOGW(TAG, "no update subscription slot: the screens follow the 1 s timer");
    }
    lv_timer_create(timer_cb, 1000, NULL);
}

static void check_now(void)
{
    hy_fw_err_t e = hy_update_check();
    ESP_LOGI(TAG, "check: %s", e == HY_FWE_NONE ? "started" : hy_fw_err_code(e));
    sync();
}

static void btn_tap(void *ud)
{
    (void)ud;
    if (s_act == ACT_CHECK) {
        check_now();
    } else if (s_act == ACT_INSTALL) {
        s_confirm = true;
        sync();
    }
}

static void cancel_tap(void *ud)
{
    (void)ud;
    s_confirm = false;
    sync();
}

static void install_tap(void *ud)
{
    (void)ud;
    hy_fw_status_t st;
    hy_update_status(&st);
    s_confirm = false;
    hy_fw_err_t e = hy_update_install(st.latest);
    ESP_LOGI(TAG, "install %s: %s", st.latest, e == HY_FWE_NONE ? "started" : hy_fw_err_code(e));
    sync();
}

void upd_view_open(void)
{
    s_want_check = true;
    ui_nav_push(HY_V_UPDATE);
}

void upd_view_show(void)
{
    if (s_want_check) {
        s_want_check = false;
        hy_fw_status_t st;
        hy_update_status(&st);
        bool busy = st.state == HY_FWS_CHECKING || st.state == HY_FWS_DOWNLOADING || st.state == HY_FWS_VERIFYING ||
                    st.state == HY_FWS_RESTARTING;
        bool recent = (st.state == HY_FWS_UP_TO_DATE || st.state == HY_FWS_AVAILABLE) && st.checked_s > 0 &&
                      now_s() - st.checked_s < RECENT_S;
#if CONFIG_HY_DEBUG_CONSOLE
        busy = busy || s_forced;
#endif
        if (!busy && !recent) {
            hy_fw_err_t e = hy_update_check();
            ESP_LOGI(TAG, "check on open: %s", e == HY_FWE_NONE ? "started" : hy_fw_err_code(e));
        }
    }
    s_have = false;
    sync();
}

void upd_view_hide(hy_view_t next)
{
    if (next != HY_V_QS) {   /* quick settings slides over this view: the confirm page stays */
        s_confirm = false;
    }
}

void upd_view_create(void)
{
    s_root = ui_view_box();
    ui_register_view(HY_V_UPDATE, s_root);
    ui_view_title(s_root, HY_S_UPD_TITLE);

    s_col = ui_plain(s_root);
    lv_obj_set_size(s_col, COL_W, 200);
    lv_obj_set_pos(s_col, 24, 80);
    lv_obj_set_flex_flow(s_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_col, 10, 0);
    s_l1 = ui_label(s_col, hy_type.body, HY_C_TEXT, "");
    lv_obj_set_width(s_l1, COL_W);
    lv_label_set_long_mode(s_l1, LV_LABEL_LONG_MODE_WRAP);
    s_l2 = ui_label(s_col, hy_type.body, HY_C_TEXT2, "");
    lv_obj_set_width(s_l2, COL_W);
    lv_label_set_long_mode(s_l2, LV_LABEL_LONG_MODE_WRAP);

    s_bar = lv_bar_create(s_root);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, BAR_W, BAR_H);
    lv_obj_set_pos(s_bar, (HY_SCREEN - BAR_W) / 2, 140);
    lv_obj_set_style_radius(s_bar, BAR_H / 2, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(BAR_BG), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bar, BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(BAR_FG), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_hidden(s_bar, true);

    s_btn = ui_button(s_root, HY_SCREEN - 36, BTN_H, hy_type.body_bold, HY_S_UPD_CHECK_AGAIN, btn_tap, NULL);
    lv_obj_set_pos(s_btn, 18, BTN_Y);
    s_btn_text = lv_obj_get_child(s_btn, 0);
    lv_obj_set_hidden(s_btn, true);

    s_conf = ui_plain(s_root);
    lv_obj_set_size(s_conf, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_pos(s_conf, 0, 0);
    s_conf_text = ui_label(s_conf, hy_type.body, HY_C_TEXT, "");
    lv_obj_set_width(s_conf_text, COL_W);
    lv_label_set_long_mode(s_conf_text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(s_conf_text, 24, 80);
    lv_obj_t *cancel = ui_button(s_conf, 210, BTN_H, hy_type.body_bold, HY_S_CANCEL, cancel_tap, NULL);
    lv_obj_set_pos(cancel, 18, BTN_Y);
    lv_obj_t *inst = ui_button(s_conf, 210, BTN_H, hy_type.body_bold, HY_S_UPD_INSTALL, install_tap, NULL);
    lv_obj_set_pos(inst, HY_SCREEN - 18 - 210, BTN_Y);
    lv_obj_set_hidden(s_conf, true);
}

#if CONFIG_HY_DEBUG_CONSOLE
void upd_view_debug(const char *arg)
{
    while (*arg == ' ') {
        arg++;
    }
    char word[16] = "", rest[24] = "";
    sscanf(arg, "%15s %23s", word, rest);
    if (strcmp(word, "auto") == 0) {
        s_forced = false;
        s_have = false;
        sync();
        return;
    }
    hy_fw_status_t f = {0};
    snprintf(f.installed, sizeof f.installed, "v0.4.0");
    snprintf(f.latest, sizeof f.latest, "v0.5.0");
    f.installed_release = true;
    f.checked_s = 1;
    bool confirm = false;
    if (strcmp(word, "checking") == 0) {
        f.state = HY_FWS_CHECKING;
        f.latest[0] = '\0';
    } else if (strcmp(word, "latest") == 0) {
        f.state = HY_FWS_UP_TO_DATE;
        snprintf(f.latest, sizeof f.latest, "v0.4.0");
    } else if (strcmp(word, "available") == 0 || strcmp(word, "confirm") == 0) {
        f.state = HY_FWS_AVAILABLE;
        confirm = word[0] == 'c';
    } else if (strcmp(word, "dev") == 0) {
        f.state = HY_FWS_AVAILABLE;
        f.installed_release = false;
        snprintf(f.installed, sizeof f.installed, "dev-a88c56d");
    } else if (strcmp(word, "downloading") == 0) {
        f.state = HY_FWS_DOWNLOADING;
        f.pct = rest[0] ? (uint8_t)atoi(rest) : 42;
    } else if (strcmp(word, "verifying") == 0) {
        f.state = HY_FWS_VERIFYING;
    } else if (strcmp(word, "restarting") == 0) {
        f.state = HY_FWS_RESTARTING;
    } else if (strcmp(word, "failed") == 0) {
        f.state = HY_FWS_FAILED;
        f.err = HY_FWE_NO_INTERNET;
        for (int e = HY_FWE_NONE + 1; e < HY_FWE_COUNT && rest[0]; e++) {
            if (strcmp(rest, hy_fw_err_code((hy_fw_err_t)e)) == 0) {
                f.err = (hy_fw_err_t)e;
            }
        }
    } else {
        ESP_LOGE(TAG, "upd: unknown state \"%s\"", word);
        return;
    }
    s_forced = true;
    s_forced_st = f;
    s_confirm = confirm;
    s_have = false;
    if (ui_current_view() != HY_V_UPDATE) {
        ui_nav_push(HY_V_UPDATE);
    } else {
        sync();
    }
}
#endif
