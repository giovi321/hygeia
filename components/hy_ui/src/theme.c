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

LV_FONT_DECLARE(lv_font_montserrat_14);

/* Symbol glyphs (backspace, OK, shift arrow) come from Montserrat 14 as the fallback of the Inter key font. LVGL
 * places a fallback glyph on the primary font's baseline, which is 27 px below the top of Inter 26's 33 px line, so
 * an 11 px tall symbol sits 5 px below the line (and key) centre. This copy of Montserrat shifts each symbol's
 * y offset so the glyph's box is centred on the primary line. */
static lv_font_t s_symbol_font;

static bool symbol_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t next)
{
    if (!lv_font_get_glyph_dsc_fmt_txt(font, dsc, letter, next)) {
        return false;
    }
    /* twice the centre height above the baseline: line height - 2 * baseline of the primary font */
    const int32_t line_c2 = hy_type.title->line_height - 2 * hy_type.title->base_line;
    dsc->ofs_y = (int16_t)(dsc->ofs_y + (line_c2 - (2 * dsc->ofs_y + (int32_t)dsc->box_h)) / 2);
    return true;
}

void hy_key_font_init(lv_font_t *out)
{
    s_symbol_font = lv_font_montserrat_14;
    s_symbol_font.get_glyph_dsc = symbol_glyph_dsc;
    *out = *hy_type.title;
    out->fallback = &s_symbol_font;
}

static const uint32_t palette[10] = {
    0x4FD1C5, 0xF6C453, 0xA78BFA, 0x7CE38B, 0xF38BA8,
    0x60A5FA, 0xFB923C, 0xE879F9, 0xA3E635, 0xF87171,
};

lv_color_t hy_med_color(uint8_t idx)
{
    return lv_color_hex(palette[idx % 10]);
}
