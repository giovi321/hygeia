/* Board path: registry BSP path (waveshare/esp32_s3_touch_amoled_2_16 2.0.1: panel init, QSPI bus, touch
 * driver, I2C bus). The BSP display start function hardcodes 50-line PSRAM buffers, so this file calls the
 * BSP functions bsp_display_new() and bsp_touch_new() and registers the display with esp_lvgl_adapter
 * itself, to get 2 x 48-line RGB565 buffers in internal DMA RAM. Because the BSP start function is bypassed,
 * its 2-pixel area rounder is re-implemented here (rounder_cb). */

#include "hy/board.h"

#include <inttypes.h>
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lv_adapter.h"

static const char *TAG = "hy_board";

#define HY_BUF_LINES 48
#define HY_FLUSH_LOG_FIRST 6

static lv_display_t *s_disp;
static bool s_ready;          /* set only at the end of a successful init */
static esp_err_t s_init_err = ESP_OK; /* first init failure, returned on later calls */
static lv_indev_t *s_indev;
static lv_display_flush_cb_t s_orig_flush;
static uint32_t s_flush_count;
static int64_t s_verbose_until_us;

static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    a->x1 = (a->x1 >> 1) << 1;
    a->y1 = (a->y1 >> 1) << 1;
    a->x2 = ((a->x2 >> 1) << 1) + 1;
    a->y2 = ((a->y2 >> 1) << 1) + 1;
}

static void flush_wrap(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    bool odd = (area->x1 & 1) || (area->y1 & 1) || !(area->x2 & 1) || !(area->y2 & 1) || w > 480 ||
               h > HY_BUF_LINES + 2;
    if (s_flush_count < HY_FLUSH_LOG_FIRST || odd || esp_timer_get_time() < s_verbose_until_us) {
        ESP_LOGI(TAG, "flush #%" PRIu32 " x=%d..%d y=%d..%d (%dx%d)%s", s_flush_count, (int)area->x1,
                 (int)area->x2, (int)area->y1, (int)area->y2, w, h, odd ? " ODD" : "");
    }
    s_flush_count++;
    s_orig_flush(disp, area, px);
}

static void touch_cb(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_event_get_target(e), &p);
    ESP_LOGI(TAG, "touch x=%d y=%d", (int)p.x, (int)p.y);
}

static void i2c_scan(void)
{
    char found[160] = "";
    size_t n = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bsp_i2c_get_handle(), addr, 20) == ESP_OK) {
            n += snprintf(found + n, sizeof found - n, " 0x%02x", addr);
        }
    }
    ESP_LOGI(TAG, "i2c devices:%s", n ? found : " none");
}

static esp_err_t board_init_once(void);

esp_err_t hy_board_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (s_init_err != ESP_OK) {
        return s_init_err; /* a failed init is not retried and not reported as OK */
    }
    esp_err_t err = board_init_once();
    if (err != ESP_OK) {
        s_init_err = err;
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

static esp_err_t board_init_once(void)
{
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c init: %s", esp_err_to_name(err));
        return err;
    }
    i2c_scan();

    esp_lv_adapter_config_t acfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    err = esp_lv_adapter_init(&acfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl adapter init: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    const bsp_display_config_t dcfg = {
        .max_transfer_sz = BSP_LCD_H_RES * (HY_BUF_LINES + 2) * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    err = bsp_display_new(&dcfg, &panel, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init: %s", esp_err_to_name(err));
        return err;
    }

    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = HY_BUF_LINES,
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
            .mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .te_sync = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };
    s_disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "register display failed");
        return ESP_FAIL;
    }
    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "display %dx%d RGB565, 2 x %d lines = %d bytes each, internal DMA RAM used %u bytes",
             BSP_LCD_H_RES, BSP_LCD_V_RES, HY_BUF_LINES, BSP_LCD_H_RES * HY_BUF_LINES * 2,
             (unsigned)(free_before - free_after));
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    s_orig_flush = lv_display_get_flush_cb(s_disp);
    lv_display_set_flush_cb(s_disp, flush_wrap);

    bsp_display_cfg_t tcfg = {
        .touch_flags = {.swap_xy = 1, .mirror_x = 0, .mirror_y = 1},
    };
    esp_lcd_touch_handle_t tp = NULL;
    err = bsp_touch_new(&tcfg, &tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch init: %s", esp_err_to_name(err));
        return err;
    }
    esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_disp, tp);
    s_indev = esp_lv_adapter_register_touch(&touch_cfg);
    if (!s_indev) {
        ESP_LOGE(TAG, "register touch failed");
        return ESP_FAIL;
    }
    lv_indev_add_event_cb(s_indev, touch_cb, LV_EVENT_PRESSED, NULL);

    err = esp_lv_adapter_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl adapter start: %s", esp_err_to_name(err));
        return err;
    }
    bsp_display_brightness_set(80);

    ESP_LOGI(TAG, "heap free: internal %u, dma %u, psram %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "board ready, brightness 80%%");
    return ESP_OK;
}

lv_display_t *hy_board_display(void)
{
    return s_ready ? s_disp : NULL;
}

i2c_master_bus_handle_t hy_board_i2c(void)
{
    return bsp_i2c_get_handle();
}

void hy_board_set_brightness(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    bsp_display_brightness_set(pct);
}

bool hy_board_lock(uint32_t timeout_ms)
{
    /* the BSP lock wrapper returns the esp_err_t as a bool (inverted), so call the adapter directly */
    return esp_lv_adapter_lock(timeout_ms == 0 ? -1 : (int32_t)timeout_ms) == ESP_OK;
}

void hy_board_unlock(void)
{
    esp_lv_adapter_unlock();
}

static void square_cb(lv_event_t *e)
{
    lv_obj_t *sq = lv_event_get_target(e);
    lv_obj_set_style_bg_color(sq, lv_color_white(), 0);
    ESP_LOGI(TAG, "selftest square touched: %s", (const char *)lv_event_get_user_data(e));
}

void hy_board_selftest(void)
{
    static const struct {
        lv_align_t align;
        const char *name;
        uint32_t color;
    } sq[4] = {
        {LV_ALIGN_TOP_LEFT, "top-left", 0xE04040},
        {LV_ALIGN_TOP_RIGHT, "top-right", 0x40C040},
        {LV_ALIGN_BOTTOM_LEFT, "bottom-left", 0x4060E0},
        {LV_ALIGN_BOTTOM_RIGHT, "bottom-right", 0xE0C040},
    };
    s_verbose_until_us = esp_timer_get_time() + 3000000; /* log every flush for 3 s */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *o = lv_obj_create(scr);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, 40, 40);
        lv_obj_align(o, sq[i].align, 0, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(sq[i].color), 0);
        lv_obj_set_clickable(o, true);
        lv_obj_add_event_cb(o, square_cb, LV_EVENT_PRESSED, (void *)sq[i].name);
    }
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, "board test");
    lv_obj_set_style_text_color(l, lv_color_hex(0xEDEDF0), 0);
    lv_obj_center(l);
    ESP_LOGI(TAG, "selftest screen built");
}
