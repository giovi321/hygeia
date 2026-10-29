/* Root view when the storage partition cannot be mounted or the store cannot be opened */

#include "ui_internal.h"

#define TEXT_W 400

lv_obj_t *storage_down_create(lv_obj_t *parent)
{
    lv_obj_t *v = lv_obj_create(parent);
    lv_obj_remove_style_all(v);
    lv_obj_set_size(v, HY_SCREEN, HY_CONTENT_H);
    lv_obj_set_scrollable(v, false);
    lv_obj_set_clickable(v, false);
    lv_obj_set_flex_flow(v, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(v, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(v, 12, 0);
    lv_obj_set_style_pad_bottom(v, HY_BAR_H, 0);   /* centred on the screen, not only the content */

    lv_obj_t *t = lv_label_create(v);
    lv_obj_set_style_text_font(t, hy_type.title, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(HY_C_TEXT), 0);
    lv_label_set_text(t, HY_S_STORAGE_TITLE);

    lv_obj_t *b = lv_label_create(v);
    lv_obj_set_width(b, TEXT_W);
    lv_obj_set_style_text_font(b, hy_type.status, 0);
    lv_obj_set_style_text_color(b, lv_color_hex(HY_C_TEXT2), 0);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(b, HY_S_STORAGE_BODY);
    return v;
}
