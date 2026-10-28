#include "ui_internal.h"

LV_FONT_DECLARE(hy_inter_17);
LV_FONT_DECLARE(hy_inter_22);
LV_FONT_DECLARE(hy_inter_b17);
LV_FONT_DECLARE(hy_inter_b22);
LV_FONT_DECLARE(hy_inter_b30);
LV_FONT_DECLARE(hy_inter_b52);

const hy_type_scale_t hy_type = {
    .status = &hy_inter_17,
    .status_bold = &hy_inter_b17,
    .body = &hy_inter_22,
    .body_bold = &hy_inter_b22,
    .time = &hy_inter_b30,
    .code = &hy_inter_b52,
};

static const uint32_t palette[10] = {
    0x4FD1C5, 0xF6C453, 0xA78BFA, 0x7CE38B, 0xF38BA8,
    0x60A5FA, 0xFB923C, 0xE879F9, 0xA3E635, 0xF87171,
};

lv_color_t hy_med_color(uint8_t idx)
{
    return lv_color_hex(palette[idx % 10]);
}
