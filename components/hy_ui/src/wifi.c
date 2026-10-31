/* The network as the screens see it, and Settings > Wi-Fi.
 *
 * net_ui_refresh reads the network status and updates everything that shows it: the top bar icon, the idle line,
 * the quick settings tiles, the Settings rows, the Web UI QR view and the clock-invalid root. It runs from the 1 s
 * tick and, as soon as hy_net reports a change, from the net task through hy_net_subscribe and lv_async_call (the
 * LVGL lock is taken with a short timeout and the call is dropped when it is not free: the tick catches up).
 *
 * Settings > Wi-Fi: a three-way mode row, the saved networks (tap one to connect to it), the access point and
 * address rows, the paired browsers with "Forget all" (which asks on a second page) and the keep-on-battery switch.
 * Everything runs in the LVGL task unless noted. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hy/board.h"
#include "hy/store.h"
#include "hy/web.h"
#include "ui_internal.h"

static const char *TAG = "ui_wifi";

#define MARGIN   18
#define ROW_W    (HY_SCREEN - 2 * MARGIN)
#define MODE_W   140
#define MODE_GAP 12
#define NET_ROW_H 56

/* ---- the status the screens show ---- */

static hy_net_status_t s_st;       /* the last status handed to the screens */
static bool s_have;
static hy_net_state_t s_prev_state = HY_NS_OFF;
static bool s_hint_pending;        /* the failing hint waits for the screen to be on */
static bool s_async_queued;        /* LVGL lock protects it */
#if CONFIG_HY_DEBUG_CONSOLE
static bool s_forced;
static hy_net_status_t s_force;
#endif

void net_ui_status(hy_net_status_t *out)
{
#if CONFIG_HY_DEBUG_CONSOLE
    if (s_forced) {
        *out = s_force;
        return;
    }
#endif
    hy_net_status(out);
}

bool net_is_up(const hy_net_status_t *st)
{
    return st->state == HY_NS_CONNECTED || st->state == HY_NS_AP;
}

bool net_ip_text(const hy_net_status_t *st, char *out, size_t len)
{
    if (!net_is_up(st) || st->ip == 0) {
        out[0] = '\0';
        return false;
    }
    const uint8_t *b = (const uint8_t *)&st->ip;   /* network order: the first octet is the first byte */
    snprintf(out, len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return true;
}

bool net_web_url(const hy_net_status_t *st, char *out, size_t len)
{
    char ip[20];
    if (!net_ip_text(st, ip, sizeof ip)) {
        out[0] = '\0';
        return false;
    }
    snprintf(out, len, HY_S_WEB_URL, ip);
    return true;
}

/* The state in a word: the Wi-Fi tile's sub-line and the Settings row's value */
void net_state_text(const hy_net_status_t *st, char *out, size_t len)
{
    const char *t;
    switch (st->state) {
    case HY_NS_PAUSED:
        t = HY_S_NET_PAUSED;
        break;
    case HY_NS_CONNECTING:
        t = HY_S_NET_CONNECTING;
        break;
    case HY_NS_CONNECTED:
        t = st->ssid[0] ? st->ssid : HY_S_NET_CONNECTING;
        break;
    case HY_NS_FAILING:
        t = HY_S_NET_FAILING;
        break;
    case HY_NS_AP:
        t = HY_S_NET_AP;
        break;
    default:
        t = HY_S_NET_OFF;
        break;
    }
    snprintf(out, len, "%s", t);
}

void net_idle_text(const hy_net_status_t *st, char *out, size_t len)
{
    switch (st->state) {
    case HY_NS_CONNECTING:
        snprintf(out, len, "%s", HY_S_IDLE_NET_CONNECTING);
        break;
    case HY_NS_CONNECTED:
        snprintf(out, len, HY_S_IDLE_NET_UP, st->ssid);
        break;
    case HY_NS_FAILING:
        snprintf(out, len, "%s", HY_S_IDLE_NET_FAILING);
        break;
    case HY_NS_AP:
        snprintf(out, len, "%s", HY_S_IDLE_NET_AP);
        break;
    default:
        snprintf(out, len, "%s", HY_S_IDLE_NET_OFF);
        break;
    }
}

static bool same_look(const hy_net_status_t *a, const hy_net_status_t *b)
{
    return a->state == b->state && a->saved == b->saved && a->ip == b->ip && a->ap_clients == b->ap_clients &&
           strcmp(a->ssid, b->ssid) == 0 && strcmp(a->host, b->host) == 0 && strcmp(a->ap_ssid, b->ap_ssid) == 0;
}

void net_ui_refresh(void)
{
    hy_net_status_t st;
    net_ui_status(&st);
    topbar_net_refresh(&st);   /* also follows the signal strength */

    /* The failing hint shows once per failing spell, when the screen is on */
    if (st.state == HY_NS_FAILING && s_prev_state != HY_NS_FAILING && st.saved == HY_NET_CLIENT) {
        s_hint_pending = true;
    }
    if (st.state != HY_NS_FAILING) {
        s_hint_pending = false;
    }
    s_prev_state = st.state;
    if (s_hint_pending && !idle_active()) {
        s_hint_pending = false;
        toast_show_info(HY_S_NET_HINT);
    }

    if (s_have && same_look(&s_st, &st)) {
        s_st = st;
        return;
    }
    s_st = st;
    s_have = true;
    idle_status_refresh();
    qs_net_refresh();
    settings_values_refresh();
    webqr_net_changed();
    timeset_net_changed();
}

static void async_cb(void *arg)
{
    (void)arg;
    s_async_queued = false;
    net_ui_refresh();
}

/* hy_net_subscribe callback: the net task. Does not block for long and keeps at most one call queued. */
static void net_changed(const hy_net_status_t *st, void *ctx)
{
    (void)st;
    (void)ctx;
    if (!hy_board_lock(50)) {
        return;
    }
    if (!s_async_queued && lv_async_call(async_cb, NULL) == LV_RESULT_OK) {
        s_async_queued = true;
    }
    hy_board_unlock();
}

void net_ui_init(void)
{
    int slot = hy_net_subscribe(net_changed, NULL);
    if (slot < 0) {
        ESP_LOGW(TAG, "no network subscription slot: the screens follow the 1 s tick");
    }
}

/* ---- Settings > Wi-Fi ---- */

static lv_obj_t *s_root, *s_list, *s_mode[3], *s_net_row[HY_NET_MAX_KNOWN], *s_net_tag[HY_NET_MAX_KNOWN];
static lv_obj_t *s_saved_hdr, *s_none, *s_ap_val, *s_addr_val, *s_br_lbl, *s_forget_btn, *s_keep_sw, *s_confirm;
static hy_known_net_t s_known[HY_NET_MAX_KNOWN];
static size_t s_nknown;

static const hy_net_mode_t k_modes[3] = {HY_NET_OFF, HY_NET_CLIENT, HY_NET_AP};

static void set_mode(hy_net_mode_t m, const char *ssid)
{
    esp_err_t err = hy_net_set_mode(m, ssid);
    ESP_LOGI(TAG, "mode %d%s: %s", (int)m, ssid ? " (a saved network first)" : "", esp_err_to_name(err));
    if (err != ESP_OK) {
        toast_show_info(HY_S_NET_ERR);
    }
    net_ui_refresh();
}

static void list_scroll_cb(lv_event_t *e)
{
    (void)e;
    ui_input_consume();   /* a stroke that scrolls the list must not also navigate */
}

static void mode_tap(void *ud)
{
    set_mode(k_modes[(int)(uintptr_t)ud], NULL);
    wifi_values_refresh();
}

static void net_tap(void *ud)
{
    size_t i = (size_t)(uintptr_t)ud;
    if (i < s_nknown) {
        set_mode(HY_NET_CLIENT, s_known[i].ssid);
        wifi_values_refresh();
    }
}

static void ap_row_tap(void *ud)
{
    (void)ud;
    ui_nav_push(HY_V_AP_INFO);
}

static void forget_ask_tap(void *ud)
{
    (void)ud;
    if (hy_web_paired_count() == 0) {
        return;
    }
    lv_obj_set_hidden(s_confirm, false);
}

static void forget_cancel_tap(void *ud)
{
    (void)ud;
    lv_obj_set_hidden(s_confirm, true);
}

static void forget_do_tap(void *ud)
{
    (void)ud;
    esp_err_t err = hy_web_forget_all();
    ESP_LOGI(TAG, "forget paired browsers: %s", esp_err_to_name(err));
    lv_obj_set_hidden(s_confirm, true);
    if (err == ESP_OK) {
        toast_show_ok(HY_S_WIFI_FORGOTTEN);
    } else {
        toast_show_info(HY_S_ERR_IO);
    }
    wifi_values_refresh();
}

static void keep_tap(void *ud)
{
    (void)ud;
    hy_settings_t s = *hy_settings();
    s.wifi_keep_batt = !s.wifi_keep_batt;
    esp_err_t err = hy_settings_update(&s);
    hy_net_set_keep_batt(s.wifi_keep_batt);   /* the RAM copy changes even when the save fails */
    if (err != ESP_OK) {
        toast_show_info(HY_S_ERR_IO);
    }
    wifi_values_refresh();
}

/* Everything the view shows, from the status, the saved networks, the paired browsers and the settings */
void wifi_values_refresh(void)
{
    if (!s_root) {
        return;
    }
    const hy_net_status_t *st = &s_st;
    hy_net_status_t now;
    if (!s_have) {
        net_ui_status(&now);
        st = &now;
    }
    for (int i = 0; i < 3; i++) {
        bool cur = st->saved == k_modes[i];
        lv_obj_set_style_border_width(s_mode[i], cur ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_mode[i], lv_color_hex(HY_C_TEXT), 0);
        lv_obj_set_style_bg_color(s_mode[i], lv_color_hex(cur ? HY_C_ON : HY_C_SURFACE2), 0);
    }
    s_nknown = hy_net_known(s_known);
    for (size_t i = 0; i < HY_NET_MAX_KNOWN; i++) {
        bool used = i < s_nknown;
        lv_obj_set_hidden(s_net_row[i], !used);
        if (!used) {
            continue;
        }
        lv_label_set_text(lv_obj_get_child(s_net_row[i], 0), s_known[i].ssid);
        bool conn = st->state == HY_NS_CONNECTED && strcmp(st->ssid, s_known[i].ssid) == 0;
        lv_label_set_text(s_net_tag[i], conn ? HY_S_WIFI_CONNECTED : "");
    }
    lv_obj_set_hidden(s_none, s_nknown != 0);
    lv_label_set_text(s_ap_val, st->state == HY_NS_AP ? HY_S_NET_AP_ON : HY_S_OFF);
    char url[40];
    char shown[48];
    if (net_web_url(st, url, sizeof url)) {
        char ip[20];
        net_ip_text(st, ip, sizeof ip);
        snprintf(shown, sizeof shown, HY_S_WEB_URL_SHOWN, ip);
    } else {
        snprintf(shown, sizeof shown, "%s", HY_S_DASH);
    }
    lv_label_set_text(s_addr_val, shown);
    size_t paired = hy_web_paired_count();
    lv_label_set_text_fmt(s_br_lbl, HY_S_WIFI_BROWSERS, (unsigned)paired);
    lv_obj_set_style_opa(s_forget_btn, paired ? LV_OPA_COVER : LV_OPA_40, 0);
    if (hy_settings()->wifi_keep_batt) {
        lv_obj_add_state(s_keep_sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s_keep_sw, LV_STATE_CHECKED);
    }
}

void wifi_show(void)
{
    lv_obj_set_hidden(s_confirm, true);
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    wifi_values_refresh();
}

static lv_obj_t *section_label(const char *text)
{
    lv_obj_t *l = ui_label(s_list, hy_type.status, HY_C_TEXT2, text);
    lv_obj_set_width(l, ROW_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

void wifi_create(void)
{
    s_root = ui_view_box();
    ui_register_view(HY_V_SET_WIFI, s_root);
    ui_view_title(s_root, HY_S_SET_WIFI);

    s_list = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, HY_SCREEN, HY_CONTENT_H - 44);
    lv_obj_set_pos(s_list, 0, 44);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_list, 12, 0);
    lv_obj_set_style_pad_left(s_list, MARGIN, 0);
    lv_obj_set_style_pad_bottom(s_list, MARGIN, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    ui_scroller(s_list);
    lv_obj_add_event_cb(s_list, list_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);

    /* the mode row: three buttons, the saved mode outlined */
    lv_obj_t *modes = ui_plain(s_list);
    lv_obj_set_size(modes, ROW_W, 66);
    static const char *const mode_text[3] = {HY_S_WIFI_MODE_OFF, HY_S_WIFI_MODE_CLIENT, HY_S_WIFI_MODE_AP};
    for (int i = 0; i < 3; i++) {
        s_mode[i] = ui_button(modes, MODE_W, 66, hy_type.status_bold, mode_text[i], mode_tap, (void *)(uintptr_t)i);
        lv_obj_set_pos(s_mode[i], i * (MODE_W + MODE_GAP), 0);
        lv_obj_set_style_border_side(s_mode[i], LV_BORDER_SIDE_FULL, 0);
    }

    s_saved_hdr = section_label(HY_S_WIFI_SAVED);
    for (size_t i = 0; i < HY_NET_MAX_KNOWN; i++) {
        s_net_tag[i] = ui_list_row(s_list, NET_ROW_H, "", net_tap, (void *)(uintptr_t)i);
        s_net_row[i] = lv_obj_get_parent(s_net_tag[i]);
        lv_obj_t *name = lv_obj_get_child(s_net_row[i], 0);
        lv_obj_set_width(name, 260);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(s_net_tag[i], 130);
        lv_obj_set_style_text_color(s_net_tag[i], lv_color_hex(HY_C_OK), 0);
    }
    s_none = section_label(HY_S_WIFI_NONE);
    section_label(HY_S_WIFI_ADD_WEB);

    s_ap_val = ui_list_row(s_list, 66, HY_S_WIFI_AP_ROW, ap_row_tap, NULL);
    s_addr_val = ui_list_row(s_list, 66, HY_S_WIFI_ADDR, NULL, NULL);
    lv_obj_set_clickable(lv_obj_get_parent(s_addr_val), false);

    /* paired browsers: the count and Forget all */
    lv_obj_t *br = ui_list_row(s_list, 72, "", NULL, NULL);
    lv_obj_t *brow = lv_obj_get_parent(br);
    lv_obj_set_clickable(brow, false);
    lv_obj_set_hidden(br, true);
    s_br_lbl = lv_obj_get_child(brow, 0);
    s_forget_btn = ui_button(brow, 150, 56, hy_type.status_bold, HY_S_WIFI_FORGET, forget_ask_tap, NULL);
    lv_obj_align(s_forget_btn, LV_ALIGN_RIGHT_MID, -8, 0);

    /* keep Wi-Fi on battery: the whole row toggles, the switch shows the state */
    lv_obj_t *kv = ui_list_row(s_list, 66, HY_S_WIFI_KEEP, keep_tap, NULL);
    lv_obj_t *krow = lv_obj_get_parent(kv);
    lv_obj_set_hidden(kv, true);
    lv_obj_t *kt = lv_obj_get_child(krow, 0);
    lv_obj_set_width(kt, 320);
    lv_label_set_long_mode(kt, LV_LABEL_LONG_MODE_DOTS);
    s_keep_sw = lv_switch_create(krow);
    lv_obj_set_size(s_keep_sw, 56, 32);
    lv_obj_set_clickable(s_keep_sw, false);
    lv_obj_set_style_bg_color(s_keep_sw, lv_color_hex(HY_C_FILL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_keep_sw, lv_color_hex(HY_C_OK), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_keep_sw, lv_color_hex(HY_C_TEXT), LV_PART_KNOB);
    lv_obj_align(s_keep_sw, LV_ALIGN_RIGHT_MID, -22, 0);

    /* the second page of Forget all */
    s_confirm = ui_plain(s_root);
    lv_obj_set_size(s_confirm, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_pos(s_confirm, 0, 0);
    lv_obj_set_style_bg_color(s_confirm, lv_color_hex(HY_C_BG), 0);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_confirm, true);   /* absorbs the touches meant for the list underneath */
    lv_obj_t *q = ui_label(s_confirm, hy_type.title, HY_C_TEXT, HY_S_WIFI_FORGET_Q);
    lv_obj_set_width(q, HY_SCREEN - 2 * 24);
    lv_label_set_long_mode(q, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(q, 24, 100);
    lv_obj_t *b = ui_button(s_confirm, ROW_W, 66, hy_type.body_bold, HY_S_CANCEL, forget_cancel_tap, NULL);
    lv_obj_set_pos(b, MARGIN, 220);
    b = ui_button(s_confirm, ROW_W, 66, hy_type.body_bold, HY_S_WIFI_FORGET_DO, forget_do_tap, NULL);
    lv_obj_set_style_bg_color(b, lv_color_hex(HY_C_MISS), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(HY_C_MISS), LV_STATE_PRESSED);
    lv_obj_set_pos(b, MARGIN, 298);
    lv_obj_set_hidden(s_confirm, true);
}

#if CONFIG_HY_DEBUG_CONSOLE
/* `ui net <state>`: the screens show this state whatever the radio does (the real status is untouched, so the
 * buttons still act on the real network). `auto` hands control back. */
void net_ui_debug(const char *arg)
{
    hy_net_status_t f = {0};
    f.saved = HY_NET_CLIENT;
    snprintf(f.host, sizeof f.host, "hygeia");
    snprintf(f.ap_ssid, sizeof f.ap_ssid, "hygeia-A1B2");
    f.ip = 0x1701A8C0;   /* 192.168.1.23 in network order */
    if (strcmp(arg, "auto") == 0) {
        s_forced = false;
    } else if (strcmp(arg, "off") == 0) {
        f.saved = HY_NET_OFF;
        f.state = HY_NS_OFF;
        f.ip = 0;
    } else if (strcmp(arg, "paused") == 0) {
        f.state = HY_NS_PAUSED;
        f.ip = 0;
    } else if (strcmp(arg, "connecting") == 0) {
        f.state = HY_NS_CONNECTING;
        f.ip = 0;
    } else if (strcmp(arg, "connected") == 0 || strcmp(arg, "weak") == 0) {
        f.state = HY_NS_CONNECTED;
        f.rssi = arg[0] == 'w' ? -80 : -55;
        snprintf(f.ssid, sizeof f.ssid, "HomeNet");
    } else if (strcmp(arg, "failing") == 0) {
        f.state = HY_NS_FAILING;
        f.ip = 0;
    } else if (strcmp(arg, "ap") == 0) {
        f.saved = HY_NET_AP;
        f.state = HY_NS_AP;
        f.ip = 0x0104A8C0;   /* 192.168.4.1 */
        f.ap_clients = 1;
    } else {
        ESP_LOGE(TAG, "usage: ui net off|paused|connecting|connected|weak|failing|ap|auto");
        return;
    }
    if (strcmp(arg, "auto") != 0) {
        s_force = f;
        s_forced = true;
    }
    ESP_LOGI(TAG, "net display: %s", s_forced ? arg : "real status");
    s_have = false;   /* this refresh reaches every screen even when the look is the same */
    net_ui_refresh();
}
#endif
