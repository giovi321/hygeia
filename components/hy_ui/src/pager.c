/* Pager: Home, Overview and History side by side in one strip. A page change animates the strip's x offset
 * 200 ms ease-out after the release (no drag-follow: navigation decides on release). Page dots 12 px above
 * the bottom of the screen. */

#include "esp_log.h"
#include "ui_internal.h"

static const char *TAG = "ui_pager";

#define PAGES      3
#define ANIM_MS    200
#define DOT        8
#define DOT_ON_W   22
#define DOT_GAP    8
#define DOT_BOTTOM 12

static lv_obj_t *s_root, *s_strip, *s_pages[PAGES], *s_dots[PAGES];
static int s_idx;

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static void set_dots(void)
{
    for (int i = 0; i < PAGES; i++) {
        bool on = i == s_idx;
        lv_obj_set_width(s_dots[i], on ? DOT_ON_W : DOT);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(on ? HY_C_TEXT : HY_C_DOT), 0);
    }
}

lv_obj_t *pager_create(lv_obj_t *parent)
{
    s_root = box(parent);
    lv_obj_set_size(s_root, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_pos(s_root, 0, 0);

    s_strip = box(s_root);
    lv_obj_set_size(s_strip, HY_SCREEN * PAGES, HY_CONTENT_H);
    lv_obj_set_pos(s_strip, 0, 0);
    for (int i = 0; i < PAGES; i++) {
        s_pages[i] = box(s_strip);
        lv_obj_set_size(s_pages[i], HY_SCREEN, HY_CONTENT_H);
        lv_obj_set_pos(s_pages[i], i * HY_SCREEN, 0);
    }

    lv_obj_t *dots = box(s_root);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, DOT);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots, DOT_GAP, 0);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -DOT_BOTTOM);
    for (int i = 0; i < PAGES; i++) {
        s_dots[i] = box(dots);
        lv_obj_set_size(s_dots[i], DOT, DOT);
        lv_obj_set_style_radius(s_dots[i], DOT / 2, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
    }
    s_idx = 0;
    set_dots();
    return s_root;
}

lv_obj_t *pager_page(int idx)
{
    return idx >= 0 && idx < PAGES ? s_pages[idx] : NULL;
}

int pager_index(void)
{
    return s_idx;
}

static void x_exec(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

void pager_show(int idx, bool animate)
{
    if (idx < 0 || idx >= PAGES) {
        return;
    }
    int32_t target = -idx * HY_SCREEN;
    int32_t from = lv_obj_get_x(s_strip);
    if (idx != s_idx) {
        ESP_LOGI(TAG, "page %d -> %d%s", s_idx, idx, animate ? " (200 ms)" : "");
    }
    s_idx = idx;
    set_dots();
    lv_anim_delete(s_strip, x_exec);
    if (!animate || from == target) {
        lv_obj_set_x(s_strip, target);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_strip);
    lv_anim_set_exec_cb(&a, x_exec);
    lv_anim_set_values(&a, from, target);
    lv_anim_set_duration(&a, ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}
