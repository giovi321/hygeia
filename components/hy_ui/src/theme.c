#include "ui_internal.h"

LV_FONT_DECLARE(hy_inter_14);
LV_FONT_DECLARE(hy_inter_17);
LV_FONT_DECLARE(hy_inter_b17);
LV_FONT_DECLARE(hy_inter_22);
LV_FONT_DECLARE(hy_inter_b22);
LV_FONT_DECLARE(hy_inter_b26);
LV_FONT_DECLARE(hy_inter_b30);
LV_FONT_DECLARE(hy_inter_b38);
LV_FONT_DECLARE(hy_inter_b44);
LV_FONT_DECLARE(hy_inter_b52);
LV_FONT_DECLARE(hy_inter_b96);
LV_FONT_DECLARE(hy_inter_l120);

/* The one place that picks fonts (fonts/generate.sh makes them) */
const hy_type_scale_t hy_type = {
    .small = &hy_inter_14,
    .status = &hy_inter_17,
    .status_bold = &hy_inter_b17,
    .body = &hy_inter_22,
    .body_bold = &hy_inter_b22,
    .title = &hy_inter_b26,
    .time = &hy_inter_b30,
    .count = &hy_inter_b38,
    .code_m = &hy_inter_b44,
    .code = &hy_inter_b52,
    .code_l = &hy_inter_b96,
    .clock = &hy_inter_l120,
};

static const uint32_t palette[10] = {
    0x4FD1C5, 0xF6C453, 0xA78BFA, 0x7CE38B, 0xF38BA8,
    0x60A5FA, 0xFB923C, 0xE879F9, 0xA3E635, 0xF87171,
};

lv_color_t hy_med_color(uint8_t idx)
{
    return lv_color_hex(palette[idx % 10]);
}
