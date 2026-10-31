/* QR codes and the access point screen.
 *
 * qr_block draws a QR code as dark modules (#000000) on a light square (#EDEDF0) with a 12 px quiet zone around the
 * 220 px code. The code is a monochrome canvas: its buffer comes from lv_malloc (the PSRAM-preferring LVGL
 * allocator) and is about 6 KB. The views build it when they show and delete it when they leave.
 *
 * HY_V_AP_INFO has two pages. 1: "Join the device's Wi-Fi", the Wi-Fi QR code with the network name and password
 * below it. 2: "Then open", the QR code of the access point's address and that address. Next goes to page 2, Done
 * leaves the view. Page 2 is also what the Web UI QR view shows while the access point is on. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_memory_utils.h"
#include "ui_internal.h"

static const char *TAG = "ui_qr";

#define QR_PX    220
#define QR_QUIET 12
#define QR_BOX   (QR_PX + 2 * QR_QUIET)
#define BTN_W    288
#define BTN_H    66
#define BTN_Y    356

_Static_assert(QR_BOX == QR_BLOCK_H, "QR_BLOCK_H is the size of the light square");

lv_obj_t *qr_block(lv_obj_t *parent, const char *text, int32_t y)
{
    lv_obj_t *box = ui_plain(parent);
    lv_obj_set_size(box, QR_BOX, QR_BOX);
    lv_obj_set_pos(box, (HY_SCREEN - QR_BOX) / 2, y);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xEDEDF0), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);

    lv_obj_t *q = lv_qrcode_create(box);
    lv_qrcode_set_size(q, QR_PX);
    lv_qrcode_set_dark_color(q, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(q, lv_color_hex(0xEDEDF0));
    lv_obj_set_pos(q, QR_QUIET, QR_QUIET);
    lv_result_t res = lv_qrcode_update(q, text, (uint32_t)strlen(text));
    if (res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "QR code does not fit (%u bytes)", (unsigned)strlen(text));
    }

    static bool logged;
    if (!logged) {
        logged = true;
        const lv_draw_buf_t *b = lv_canvas_get_draw_buf(q);
        if (b) {
            ESP_LOGI(TAG, "QR buffer %u bytes in %s", (unsigned)b->data_size,
                     esp_ptr_external_ram(b->data) ? "PSRAM" : "internal RAM");
        }
    }
    return box;
}

/* ---- the access point screen ---- */

static lv_obj_t *s_root, *s_title, *s_dyn, *s_btn, *s_btn_text;
static int s_page;   /* 0 join, 1 open */

/* The address block shared with the Web UI QR view: the code of the access point's page and the address under it */
void apinfo_open_fill(lv_obj_t *holder, int32_t y)
{
    qr_block(holder, HY_S_AP_URL, y);
    lv_obj_t *l = ui_label(holder, hy_type.body, HY_C_TEXT, HY_S_AP_URL);
    lv_obj_set_width(l, HY_SCREEN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, 0, y + QR_BLOCK_H + 10);
}

static void fill(void)
{
    lv_obj_clean(s_dyn);
    if (s_page == 0) {
        hy_net_status_t st;
        net_ui_status(&st);
        char pass[64], qr[128];
        if (hy_net_ap_pass_get(pass) != ESP_OK) {
            pass[0] = '\0';
        }
        if (hy_net_wifi_qr(st.ap_ssid, pass, qr, sizeof qr) < 0) {
            qr[0] = '\0';
        }
        lv_label_set_text(s_title, HY_S_AP_JOIN);
        lv_label_set_text(s_btn_text, HY_S_NEXT);
        qr_block(s_dyn, qr, 44);
        char line[96];
        snprintf(line, sizeof line, HY_S_AP_NAME, st.ap_ssid);
        lv_obj_t *l = ui_label(s_dyn, hy_type.body, HY_C_TEXT, line);
        lv_obj_set_width(l, HY_SCREEN);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, 0, 44 + QR_BLOCK_H + 6);
        snprintf(line, sizeof line, HY_S_AP_PASS, pass);
        l = ui_label(s_dyn, hy_type.body, HY_C_TEXT, line);
        lv_obj_set_width(l, HY_SCREEN);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, 0, 44 + QR_BLOCK_H + 36);
        memset(pass, 0, sizeof pass);
        memset(qr, 0, sizeof qr);
        memset(line, 0, sizeof line);
    } else {
        lv_label_set_text(s_title, HY_S_AP_OPEN);
        lv_label_set_text(s_btn_text, HY_S_DONE);
        apinfo_open_fill(s_dyn, 44);
    }
}

static void btn_tap(void *ud)
{
    (void)ud;
    if (s_page == 0) {
        s_page = 1;
        fill();
    } else {
        ui_nav_pop();
    }
}

void apinfo_show(void)
{
    fill();   /* a view pulled back from quick settings still has its page: fill rebuilds the code */
}

void apinfo_hide(hy_view_t next)
{
    if (next == HY_V_QS) {
        return;   /* quick settings slides over this view: it stays as it is and comes back to the same page */
    }
    lv_obj_clean(s_dyn);
    s_page = 0;
}

void apinfo_create(void)
{
    s_root = ui_view_box();
    ui_register_view(HY_V_AP_INFO, s_root);
    s_title = ui_view_title(s_root, HY_S_AP_JOIN);
    s_dyn = ui_plain(s_root);
    lv_obj_set_size(s_dyn, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_pos(s_dyn, 0, 0);
    s_btn = ui_button(s_root, BTN_W, BTN_H, hy_type.body_bold, HY_S_NEXT, btn_tap, NULL);
    lv_obj_set_pos(s_btn, (HY_SCREEN - BTN_W) / 2, BTN_Y);
    s_btn_text = lv_obj_get_child(s_btn, 0);
}
