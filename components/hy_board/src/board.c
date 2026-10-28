/* Board path: registry BSP path (waveshare/esp32_s3_touch_amoled_2_16 2.0.1: panel init, QSPI bus, touch
 * driver, I2C bus). The BSP display start function hardcodes 50-line PSRAM buffers, so this file calls the
 * BSP functions bsp_display_new() and bsp_touch_new() and registers the display with esp_lvgl_adapter
 * itself, to get 2 x 48-line RGB565 buffers in internal DMA RAM. Because the BSP start function is bypassed,
 * its 2-pixel area rounder is re-implemented here (rounder_cb). */

#include "hy/board.h"
#include "hy/power.h"

#include <inttypes.h>
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
    /* CO5300 needs even x1/y1 and odd x2/y2 (inclusive coordinates), so even widths and heights.
     * A partial flush may be taller than HY_BUF_LINES when it is narrower than the screen (LVGL fits
     * rows into the same buffer, e.g. 210 px wide -> 108 rows); the size limit is pixels, not rows. */
    bool odd = (area->x1 & 1) || (area->y1 & 1) || !(area->x2 & 1) || !(area->y2 & 1) || w > 480 ||
               (w * h) > (480 * HY_BUF_LINES);
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

/* AXP2101 registers. Source: XPowersLib (github lewisxhe/XPowersLib), commit
 * d6997586e68f65afd51baa775903df930db39821 (default branch, newer than tag v0.3.3):
 *   src/REG/AXP2101Constants.h: AXP2101_SLAVE_ADDRESS 0x34, XPOWERS_AXP2101_CHIP_ID 0x4A,
 *     XPOWERS_AXP2101_IC_TYPE 0x03, XPOWERS_AXP2101_COMMON_CONFIG 0x10, XPOWERS_AXP2101_PWRON_STATUS 0x20
 *   src/XPowersAXP2101.hpp: PWRON_STATUS bits: 0 PWRON key low, 1 IRQ pin low, 2 VBUS insert and good,
 *     3 battery voltage > 3.3 V when charged, 4 battery insert and good, 5 EN mode (PWRON always high);
 *     shutdown() = set bit 0 of COMMON_CONFIG ("turn off all power channels, only VRTC stays").
 * The library has no call that clears PWRON_STATUS. */
#define AXP_ADDR          0x34
#define AXP_REG_IC_TYPE   0x03
#define AXP_IC_TYPE_ID    0x4A
#define AXP_REG_COMMON    0x10
#define AXP_COMMON_PWROFF 0x01
#define AXP_REG_PWRON     0x20
#define BOOT_GPIO         0 /* BOOT key, spec Hardware table: BOOT GPIO 0 */

static i2c_master_dev_handle_t s_axp;
static uint8_t s_boot_pwron = 0xFF;
static int s_boot_reset = -1;
static int s_boot_would_off = -1;

static esp_err_t axp_read(uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(s_axp, &reg, 1, v, 1, 100);
}

static void axp_power_off(void)
{
    uint8_t v = 0;
    uint8_t buf[2] = {AXP_REG_COMMON, 0};
    if (axp_read(AXP_REG_COMMON, &v) != ESP_OK) {
        ESP_LOGE(TAG, "power off: read common config failed");
        return;
    }
    buf[1] = v | AXP_COMMON_PWROFF;
    ESP_LOGW(TAG, "powering off via AXP2101");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200)); /* let the USB Serial/JTAG FIFO drain before the rails drop */
    esp_err_t werr = i2c_master_transmit(s_axp, buf, sizeof buf, 100);
    ESP_LOGI(TAG, "power-off write: %s", esp_err_to_name(werr));
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(2000)); /* power is cut; if not, say so and carry on */
    ESP_LOGE(TAG, "still running after AXP2101 power-off write");
}

/* Decide before the display is initialised. Always logs the raw status and the reset reason. */
static void power_gate(void)
{
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bsp_i2c_get_handle(), &dcfg, &s_axp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "axp2101 add device: %s; power gate skipped", esp_err_to_name(err));
        return;
    }
    gpio_config_t gc = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&gc);
    vTaskDelay(pdMS_TO_TICKS(5));
    bool boot_held = gpio_get_level(BOOT_GPIO) == 0;
    uint8_t ic = 0, st = 0;
    if (axp_read(AXP_REG_IC_TYPE, &ic) != ESP_OK || axp_read(AXP_REG_PWRON, &st) != ESP_OK) {
        ESP_LOGE(TAG, "axp2101 read failed; power gate skipped");
        return;
    }
    if (ic != AXP_IC_TYPE_ID) {
        ESP_LOGE(TAG, "axp2101 ic_type=0x%02x, expected 0x%02x; power gate skipped (boot)", ic, AXP_IC_TYPE_ID);
        return;
    }
    esp_reset_reason_t rr = esp_reset_reason();
    hy_reset_kind_t kind = rr == ESP_RST_POWERON ? HY_RST_POWERON : HY_RST_OTHER;
    bool allowed = hy_power_on_allowed(st, kind);
    s_boot_pwron = st;
    s_boot_reset = (int)rr;
    s_boot_would_off = allowed ? 0 : 1;
    ESP_LOGI(TAG, "axp2101 ic_type=0x%02x (matches AXP2101)", ic);
    ESP_LOGI(TAG, "pwron_status=0x%02x reset_reason=%d (%s) power off: %s", st, (int)rr,
             kind == HY_RST_POWERON ? "power-on" : "other", allowed ? "no" : "yes");
    if (!allowed && boot_held) {
        ESP_LOGW(TAG, "gate bypassed (BOOT held)");
    } else if (!allowed) {
        /* Safe after flash and RTS resets: they report ESP_RST_USB (not power-on), so they keep running
         * even though the VBUS-insert bit stays latched (see design/hardware-notes.md, Power-on source). */
        ESP_LOGW(TAG, "power-on source is not the PWR key");
        axp_power_off();
    }
}

void hy_board_power_report(void)
{
    uint8_t now = 0xFF;
    if (s_axp) {
        axp_read(AXP_REG_PWRON, &now);
    }
    if (s_boot_would_off < 0) {
        ESP_LOGI(TAG, "power report: boot gate skipped (no AXP2101 match); now pwron_status=0x%02x", now);
        return;
    }
    ESP_LOGI(TAG, "power report: boot pwron_status=0x%02x reset_reason=%d power_off=%d; now pwron_status=0x%02x",
             s_boot_pwron, s_boot_reset, s_boot_would_off, now);
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
    power_gate();
    i2c_scan();

    esp_lv_adapter_config_t acfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    /* The LVGL task also runs the actions and the store (line buffers, JSON): more than the 8 KB default */
    acfg.task_stack_size = 12 * 1024;
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
