/* Archive confirmation: opened by the Archive button on the medicine detail. Only the Archive button writes; both
 * buttons act on a still tap and never on a stroke that only woke the screen (ui_on_tap). The action runs in the
 * LVGL task. The scheduler drops the medicine's open dose and snooze at the refresh that HY_EV_MEDS_CHANGED already
 * triggers, so the due card hides or shows the next entry; the archive writes no log entry. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hy/actions.h"
#include "hy/sched_run.h"
#include "ui_internal.h"

static const char *TAG = "ui_archive";

static lv_obj_t *s_root, *s_title, *s_due;
static uint32_t s_med_id;
static uint32_t s_gen_seen;

static bool find_med(uint32_t id, hy_med_t *out)
{
    const hy_med_t *ms;
    size_t n = hy_meds(&ms);
    for (size_t i = 0; i < n; i++) {
        if (ms[i].id == id) {
            *out = ms[i];
            return true;
        }
    }
    return false;
}

/* An open or snoozed dose of this medicine is in the scheduler snapshot */
static bool has_open_dose(uint32_t id)
{
    if (hy_sched_generation() == 0) {
        return false;
    }
    const hy_snapshot_t *sn = hy_sched_snapshot();
    for (size_t i = 0; i < sn->out.n_open; i++) {
        if (sn->out.open[i].med_id == id) {
            return true;
        }
    }
    return false;
}

/* The amber line follows the snapshot: a dose that falls due, is snoozed or is handled while the confirmation is open
 * changes it before Archive can be tapped */
void medarchive_refresh_if_shown(void)
{
    if (s_root && ui_current_view() == HY_V_MED_ARCHIVE && hy_sched_generation() != s_gen_seen) {
        s_gen_seen = hy_sched_generation();
        lv_obj_set_hidden(s_due, !has_open_dose(s_med_id));
    }
}

void medarchive_show(uint32_t med_id)
{
    s_med_id = med_id;
    s_gen_seen = hy_sched_generation();
    hy_med_t m;
    char t[64];
    if (find_med(med_id, &m)) {
        snprintf(t, sizeof t, HY_S_ARCH_TITLE, m.name);
    } else {
        snprintf(t, sizeof t, HY_S_ARCH_TITLE, "");
    }
    lv_label_set_text(s_title, t);
    lv_obj_set_hidden(s_due, !has_open_dose(med_id));
}

static void cancel_cb(void *ud)
{
    (void)ud;
    if (ui_current_view() != HY_V_MED_ARCHIVE) {
        return;
    }
    ESP_LOGI(TAG, "archive: cancelled");
    ui_nav_pop();
}

void medarchive_confirm(void)
{
    if (ui_current_view() != HY_V_MED_ARCHIVE) {
        return;
    }
    hy_med_t m;
    char code[4] = "";
    if (find_med(s_med_id, &m)) {
        snprintf(code, sizeof code, "%s", m.code);
    }
    hy_err_t err = hy_act_med_archive(s_med_id, true);
    ESP_LOGI(TAG, "archive: med %u -> %s", (unsigned)s_med_id, hy_err_str(err));
    if (err != HY_OK) {
        toast_show_info(ui_err_text(err));
        return;
    }
    ui_nav_home();
    char b[48];
    snprintf(b, sizeof b, HY_S_ARCH_DONE, code);
    toast_show_info(b);
}

static void confirm_cb(void *ud)
{
    (void)ud;
    medarchive_confirm();
}

void medarchive_create(void)
{
    s_root = ui_view_box();
    ui_register_view(HY_V_MED_ARCHIVE, s_root);

    s_title = ui_view_title(s_root, "");
    lv_obj_set_width(s_title, 432);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_title, lv_font_get_line_height(hy_type.title));

    lv_obj_t *body = ui_label(s_root, hy_type.body, HY_C_TEXT, HY_S_ARCH_BODY);
    lv_obj_set_width(body, 432);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(body, 24, 52);

    s_due = ui_label(s_root, hy_type.status, HY_C_AMBER, HY_S_ARCH_DUE);
    lv_obj_set_width(s_due, 432);
    lv_obj_set_pos(s_due, 24, 206);
    lv_obj_set_hidden(s_due, true);

    lv_obj_t *cancel = ui_button(s_root, 444, 66, hy_type.body_bold, HY_S_CANCEL, cancel_cb, NULL);
    lv_obj_set_pos(cancel, 18, 250);
    lv_obj_t *ok = ui_button(s_root, 444, 66, hy_type.body_bold, HY_S_ARCH_CONFIRM, confirm_cb, NULL);
    lv_obj_set_pos(ok, 18, 336);
    lv_obj_set_style_text_color(lv_obj_get_child(ok, 0), lv_color_hex(HY_C_MISS), 0);
}
