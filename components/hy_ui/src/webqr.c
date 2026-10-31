/* Web UI QR (quick settings): what the person needs to open the web UI from a phone.
 * - Wi-Fi client connected: the QR code of http://<ip>/, the address and <host>.local under it.
 * - Access point on: the two pages of the access point screen (join the Wi-Fi, then open the address), Next and Done.
 * - Otherwise: a line saying the network must be on first, and buttons that turn on Wi-Fi or the access point.
 * The view follows network changes while it shows (webqr_net_changed); its QR code exists only while it shows. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "ui_internal.h"

static const char *TAG = "ui_webqr";

#define BTN_H 66

static lv_obj_t *s_root, *s_title, *s_dyn, *s_msg, *s_b_wifi, *s_b_ap, *s_b_done, *s_b_done_text;
static int s_ap_page;   /* access point mode: 0 join, 1 open */

static void start_mode(hy_net_mode_t m)
{
    esp_err_t err = hy_net_set_mode(m, NULL);
    ESP_LOGI(TAG, "mode %d: %s", (int)m, esp_err_to_name(err));
    if (err != ESP_OK) {
        toast_show_info(HY_S_NET_ERR);
    }
    net_ui_refresh();
}

static void wifi_tap(void *ud)
{
    (void)ud;
    start_mode(HY_NET_CLIENT);
}

static void ap_tap(void *ud)
{
    (void)ud;
    start_mode(HY_NET_AP);
}

static void fill(void);

static void done_tap(void *ud)
{
    (void)ud;
    if (s_ap_page == 0) {
        s_ap_page = 1;
        fill();
    } else {
        ui_nav_pop();
    }
}

static lv_obj_t *centred(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *text, int32_t y)
{
    lv_obj_t *l = ui_label(parent, f, color, text);
    lv_obj_set_width(l, HY_SCREEN - 2 * 24);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, 24, y);
    return l;
}

static void fill(void)
{
    hy_net_status_t st;
    net_ui_status(&st);
    lv_obj_clean(s_dyn);
    lv_obj_set_hidden(s_msg, true);
    lv_obj_set_hidden(s_b_wifi, true);
    lv_obj_set_hidden(s_b_ap, true);
    lv_obj_set_hidden(s_b_done, true);
    if (st.state != HY_NS_AP) {
        s_ap_page = 0;
    }
    lv_label_set_text(s_title, HY_S_WEB_TITLE);
    char url[48], ip[20], line[48];
    if (st.state == HY_NS_CONNECTED && net_web_url(&st, url, sizeof url) && net_ip_text(&st, ip, sizeof ip)) {
        qr_block(s_dyn, url, 44);
        snprintf(line, sizeof line, HY_S_WEB_URL_SHOWN, ip);
        centred(s_dyn, hy_type.body, HY_C_TEXT, line, 44 + QR_BLOCK_H + 10);
        snprintf(line, sizeof line, HY_S_WEB_LOCAL, st.host);
        centred(s_dyn, hy_type.body, HY_C_TEXT2, line, 44 + QR_BLOCK_H + 42);
    } else if (st.state == HY_NS_AP) {
        if (s_ap_page == 0) {
            lv_label_set_text(s_title, HY_S_AP_JOIN);
            lv_label_set_text(s_b_done_text, HY_S_NEXT);
            apinfo_join_fill(s_dyn, 44);
        } else {
            lv_label_set_text(s_title, HY_S_AP_OPEN);
            lv_label_set_text(s_b_done_text, HY_S_DONE);
            apinfo_open_fill(s_dyn, 44);
        }
        lv_obj_set_hidden(s_b_done, false);
    } else {
        const char *msg = HY_S_WEB_NEED_NET;
        bool trying = st.saved == HY_NET_CLIENT && (st.state == HY_NS_CONNECTING || st.state == HY_NS_FAILING);
        if (trying) {
            msg = st.state == HY_NS_FAILING ? HY_S_NET_FAILING : HY_S_NET_CONNECTING;
        }
        lv_label_set_text(s_msg, msg);
        lv_obj_set_hidden(s_msg, false);
        lv_obj_set_hidden(s_b_ap, false);
        lv_obj_set_hidden(s_b_wifi, trying);
        lv_obj_set_y(s_b_ap, trying ? 170 : 250);
    }
}

void webqr_show(void)
{
    fill();
}

void webqr_hide(hy_view_t next)
{
    if (next != HY_V_QS) {   /* quick settings slides over this view: it keeps its code until it comes back */
        lv_obj_clean(s_dyn);
        s_ap_page = 0;
    }
}

void webqr_net_changed(void)
{
    if (s_root && ui_current_view() == HY_V_WEB_QR) {
        fill();
    }
}

void webqr_create(void)
{
    s_root = ui_view_box();
    ui_register_view(HY_V_WEB_QR, s_root);
    s_title = ui_view_title(s_root, HY_S_WEB_TITLE);
    s_dyn = ui_plain(s_root);
    lv_obj_set_size(s_dyn, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_pos(s_dyn, 0, 0);

    s_msg = ui_label(s_root, hy_type.body, HY_C_TEXT, "");
    lv_obj_set_width(s_msg, HY_SCREEN - 2 * 24);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_msg, 24, 80);

    s_b_wifi = ui_button(s_root, HY_SCREEN - 36, BTN_H, hy_type.body_bold, HY_S_WEB_WIFI_BTN, wifi_tap, NULL);
    lv_obj_set_pos(s_b_wifi, 18, 170);
    s_b_ap = ui_button(s_root, HY_SCREEN - 36, BTN_H, hy_type.body_bold, HY_S_WEB_AP_BTN, ap_tap, NULL);
    lv_obj_set_pos(s_b_ap, 18, 250);
    s_b_done = ui_button(s_root, 288, BTN_H, hy_type.body_bold, HY_S_DONE, done_tap, NULL);
    lv_obj_set_pos(s_b_done, (HY_SCREEN - 288) / 2, 356);
    s_b_done_text = lv_obj_get_child(s_b_done, 0);
    lv_obj_set_hidden(s_msg, true);
    lv_obj_set_hidden(s_b_wifi, true);
    lv_obj_set_hidden(s_b_ap, true);
    lv_obj_set_hidden(s_b_done, true);
}
