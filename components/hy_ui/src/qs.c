/* Quick settings: 2 x 2 tiles (Wi-Fi and Access point are placeholders until plan 4, Sound and Do not disturb
 * toggle the settings), the brightness slider, the battery line and the Settings and Web UI QR buttons. The view
 * slides down over the previous one in 200 ms. Closing is the nav rule: swipe up anywhere or the bottom edge. */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "hy/store.h"
#include "ui_internal.h"

static const char *TAG = "ui_qs";

#define MARGIN   18
#define GAP      12
#define TILE_W   216
#define TILE_H   96
#define SLIDE_MS 200

enum { T_WIFI, T_AP, T_SOUND, T_DND, T_N };

static lv_obj_t *s_root, *s_tile[T_N], *s_sub[T_N], *s_slider;

static void save(const hy_settings_t *s)
{
    esp_err_t err = hy_settings_update(s);
    ESP_LOGI(TAG, "settings saved: %s (sound %d, dnd %d, volume %u, brightness %u)", esp_err_to_name(err), s->sound,
             s->dnd, (unsigned)s->volume, (unsigned)s->brightness);
}

static void refresh(void)
{
    const hy_settings_t *st = hy_settings();
    lv_label_set_text(s_sub[T_WIFI], HY_S_OFF);
    lv_label_set_text(s_sub[T_AP], HY_S_OFF);
    lv_label_set_text(s_sub[T_SOUND], st->sound ? HY_S_QS_CHIME_ON : HY_S_QS_CHIME_OFF);
    lv_label_set_text(s_sub[T_DND], st->dnd ? HY_S_ON : HY_S_OFF);
    lv_obj_set_style_bg_color(s_tile[T_SOUND], lv_color_hex(st->sound ? HY_C_ON : HY_C_SURFACE2), 0);
    lv_obj_set_style_bg_color(s_tile[T_DND], lv_color_hex(st->dnd ? HY_C_ON : HY_C_SURFACE2), 0);
    lv_slider_set_value(s_slider, st->brightness, LV_ANIM_OFF);
}

static void toggle_sound(void)
{
    hy_settings_t s = *hy_settings();
    s.sound = !s.sound;
    save(&s);
    refresh();
}

static void toggle_dnd(void)
{
    hy_settings_t s = *hy_settings();
    s.dnd = !s.dnd;
    save(&s);
    refresh();
}

static void tile_tap(void *ud)
{
    switch ((int)(uintptr_t)ud) {
    case T_SOUND:
        toggle_sound();
        break;
    case T_DND:
        toggle_dnd();
        break;
    default:
        toast_show_info(HY_S_NOT_YET);   /* Wi-Fi and Access point: plan 4 */
        break;
    }
}

static void settings_tap(void *ud)
{
    (void)ud;
    ui_nav_push(HY_V_SETTINGS);
}

static void qr_tap(void *ud)
{
    (void)ud;
    toast_show_info(HY_S_NOT_YET);   /* the QR is added by plan 4 */
}

static void make_tile(int idx, const char *title)
{
    lv_obj_t *t = lv_obj_create(s_root);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, TILE_W, TILE_H);
    lv_obj_set_pos(t, MARGIN + (idx % 2) * (TILE_W + GAP), (idx / 2) * (TILE_H + GAP));
    lv_obj_set_style_radius(t, 22, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(HY_C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(t, false);
    if (idx == T_WIFI || idx == T_AP) {
        lv_obj_set_style_opa(t, LV_OPA_40, 0);
    }
    ui_on_tap(t, tile_tap, (void *)(uintptr_t)idx);
    lv_obj_t *l = ui_label(t, hy_type.body_bold, HY_C_TEXT, title);
    lv_obj_set_pos(l, 16, 18);
    s_sub[idx] = ui_label(t, hy_type.status, HY_C_TEXT2, "");
    lv_obj_set_pos(s_sub[idx], 16, 58);
    s_tile[idx] = t;
}

static void slider_live(int32_t v)
{
    settings_brightness_live(v);
}

static void slider_commit(int32_t v)
{
    settings_brightness_commit(v);
}

void qs_create(void)
{
    s_root = ui_view_box();
    ui_register_view(HY_V_QS, s_root);

    make_tile(T_WIFI, HY_S_QS_WIFI);
    make_tile(T_AP, HY_S_QS_AP);
    make_tile(T_SOUND, HY_S_QS_SOUND);
    make_tile(T_DND, HY_S_QS_DND);

    int y = 2 * (TILE_H + GAP) + 2;
    s_slider = ui_slider(s_root, HY_SCREEN - 2 * MARGIN, 10, 100, slider_live, slider_commit);
    lv_obj_set_pos(s_slider, MARGIN, y);

    y += 30 + 16;
    lv_obj_t *l = ui_label(s_root, hy_type.status, HY_C_TEXT2, HY_S_QS_BRIGHTNESS);
    lv_obj_set_pos(l, 24, y);
    lv_obj_t *r = ui_label(s_root, hy_type.status, HY_C_TEXT2, HY_S_QS_BATTERY);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -24, y);

    y += 21 + 13;
    int bw = (HY_SCREEN - 2 * MARGIN - GAP) / 2;
    lv_obj_t *b = ui_button(s_root, bw, 64, hy_type.status_bold, HY_S_QS_SETTINGS, settings_tap, NULL);
    lv_obj_set_pos(b, MARGIN, y);
    lv_obj_align(lv_obj_get_child(b, 0), LV_ALIGN_LEFT_MID, 16, 0);   /* left-aligned as in the mockup */
    b = ui_button(s_root, bw, 64, hy_type.status_bold, HY_S_QS_QR, qr_tap, NULL);
    lv_obj_set_pos(b, MARGIN + bw + GAP, y);
    lv_obj_align(lv_obj_get_child(b, 0), LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_opa(b, LV_OPA_40, 0);
}

static void slide_done(lv_anim_t *a)
{
    (void)a;
    ui_qs_slide_done();
}

void qs_show(void)
{
    refresh();
    if (!due_visible()) {
        lv_obj_move_foreground(s_root);   /* the sub-views are created after it: slide over them too */
    }
    lv_anim_delete(s_root, NULL);
    lv_obj_set_y(s_root, -HY_CONTENT_H);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_root);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, -HY_CONTENT_H, 0);
    lv_anim_set_duration(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, slide_done);
    lv_anim_start(&a);
}

#if CONFIG_HY_DEBUG_CONSOLE
/* The same handlers as a tap on the tile and a release of the slider */
void qs_debug(const char *arg)
{
    int n = 0, y, mo, d, h, mi;
    if (sscanf(arg, "time %d %d %d %d %d", &y, &mo, &d, &h, &mi) == 5) {
        timeset_debug_set(y, mo, d, h, mi);
    } else if (strcmp(arg, "sound") == 0) {
        toggle_sound();
    } else if (strcmp(arg, "dnd") == 0) {
        toggle_dnd();
    } else if (sscanf(arg, "bright %d", &n) == 1 && n >= 10 && n <= 100) {
        lv_slider_set_value(s_slider, n, LV_ANIM_OFF);
        settings_brightness_live(n);
        settings_brightness_commit(n);
    } else {
        ESP_LOGE(TAG, "usage: qs sound|dnd|bright <10-100>|time <y> <m> <d> <h> <mi>");
    }
}
#endif
