/* Board path: registry BSP path (waveshare/esp32_s3_touch_amoled_2_16 2.0.1: panel init, QSPI bus, touch
 * driver, I2C bus). The BSP display start function hardcodes 50-line PSRAM buffers, so this file calls the
 * BSP functions bsp_display_new() and bsp_touch_new() and registers the display with esp_lvgl_adapter
 * itself, to get 2 x 48-line RGB565 buffers in internal DMA RAM. Because the BSP start function is bypassed,
 * its 2-pixel area rounder is re-implemented here (rounder_cb). */

#include "hy/board.h"
#include "hy/nav.h"
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
#include "esp_attr.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lv_adapter.h"

static const char *TAG = "hy_board";

#define HY_BUF_LINES 48
#define HY_FLUSH_LOG_FIRST 6

static lv_display_t *s_disp;
static bool s_ready;          /* set only at the end of a successful init */
static esp_err_t s_init_err = ESP_OK; /* first init failure, returned on later calls */
static lv_indev_t *s_indev;
static esp_lcd_panel_handle_t s_panel;
static void (*volatile s_touch_hook)(void);
static void touch_isr(esp_lcd_touch_handle_t tp, void *ctx);
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
    uint8_t now = 0;
    esp_err_t err = s_axp ? axp_read(AXP_REG_PWRON, &now) : ESP_ERR_INVALID_STATE;
    char now_txt[48];
    if (err == ESP_OK) {
        snprintf(now_txt, sizeof now_txt, "0x%02x", now);
    } else {
        snprintf(now_txt, sizeof now_txt, "read failed: %s", esp_err_to_name(err));
    }
    if (s_boot_would_off < 0) {
        ESP_LOGI(TAG, "power report: boot gate skipped (no AXP2101 match); now pwron_status=%s", now_txt);
        return;
    }
    ESP_LOGI(TAG, "power report: boot pwron_status=0x%02x reset_reason=%d power_off=%d; now pwron_status=%s",
             s_boot_pwron, s_boot_reset, s_boot_would_off, now_txt);
}

/* ---- PMU access after the gate. Register facts: XPowersLib (MIT, lewisxhe) commit
 * d6997586e68f65afd51baa775903df930db39821: src/REG/AXP2101Constants.h (addresses), src/XPowersAXP2101.hpp
 * (disableTSPinMeasure, enableBattVoltageMeasure, setChargerConstantCurr, setChargeTargetVoltage,
 * clearIrqStatus, readRegisterH5L8) and src/XPowersParams.hpp (XPOWERS_AXP2101_CHG_CUR_150MA = 6,
 * XPOWERS_AXP2101_CHG_VOL_4V2 = 3). */
#define AXP_REG_TS_PIN_CTRL 0x50 /* XPOWERS_AXP2101_TS_PIN_CTRL */
#define AXP_REG_IPRECHG     0x61 /* XPOWERS_AXP2101_IPRECHG_SET */
#define AXP_REG_ICC_CHG     0x62 /* XPOWERS_AXP2101_ICC_CHG_SET: bits 4..0 constant current code */
#define AXP_REG_ITERM_CHG   0x63 /* XPOWERS_AXP2101_ITERM_CHG_SET_CTRL */
#define AXP_REG_CV_CHG      0x64 /* XPOWERS_AXP2101_CV_CHG_VOL_SET: bits 2..0 target voltage code */
#define AXP_CHG_CUR_150MA   0x06
#define AXP_CHG_VOL_4V2     0x03

static esp_err_t axp_write(uint8_t reg, uint8_t v)
{
    uint8_t buf[2] = {reg, v};
    return i2c_master_transmit(s_axp, buf, sizeof buf, 100);
}

/* v = (v & and_mask) | or_mask, written only when it changes; logged either way */
static esp_err_t axp_rmw(uint8_t reg, uint8_t and_mask, uint8_t or_mask)
{
    uint8_t v = 0;
    esp_err_t err = axp_read(reg, &v);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pmu: reg 0x%02x read: %s", reg, esp_err_to_name(err));
        return err;
    }
    uint8_t nv = (uint8_t)((v & and_mask) | or_mask);
    if (nv == v) {
        ESP_LOGI(TAG, "pmu: reg 0x%02x 0x%02x unchanged", reg, v);
        return ESP_OK;
    }
    err = axp_write(reg, nv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pmu: reg 0x%02x write 0x%02x: %s", reg, nv, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "pmu: reg 0x%02x 0x%02x -> 0x%02x", reg, v, nv);
    return ESP_OK;
}

static bool pmu_present(void)
{
    return s_axp != NULL && s_boot_would_off >= 0;
}

esp_err_t hy_board_pmu_charger_setup(void)
{
    if (!pmu_present()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    /* disableTSPinMeasure: TS_PIN_CTRL &= 0xF0, | 0x10; then clear ADC_CHANNEL_CTRL bit 1 */
    if ((err = axp_rmw(AXP_REG_TS_PIN_CTRL, 0xF0, 0x10)) != ESP_OK) {
        return err;
    }
    if ((err = axp_rmw(HY_AXP_REG_ADC_CTRL, (uint8_t)~0x02u, 0x00)) != ESP_OK) {
        return err;
    }
    /* setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2): CV_CHG_VOL_SET &= 0xF8, | 3 */
    if ((err = axp_rmw(AXP_REG_CV_CHG, 0xF8, AXP_CHG_VOL_4V2)) != ESP_OK) {
        return err;
    }
    /* setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_150MA): ICC_CHG_SET &= 0xE0, | 6 */
    return axp_rmw(AXP_REG_ICC_CHG, 0xE0, AXP_CHG_CUR_150MA);
}

esp_err_t hy_board_pmu_init(void)
{
    if (!pmu_present()) {
        ESP_LOGE(TAG, "pmu: no AXP2101, setup skipped");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t first = ESP_OK, err;
    uint8_t en[3];
    hy_axp_irq_enable_mask(en);
    for (int i = 0; i < 3; i++) {
        err = axp_rmw((uint8_t)(HY_AXP_REG_INTEN1 + i), 0x00, en[i]);
        first = first != ESP_OK ? first : err;
    }
    /* clearIrqStatus: 0xFF to INTSTS1..3 (write 1 to clear) */
    for (int i = 0; i < 3; i++) {
        err = axp_write((uint8_t)(HY_AXP_REG_INTSTS1 + i), 0xFF);
        first = first != ESP_OK ? first : err;
    }
    ESP_LOGI(TAG, "pmu: irq status cleared");
    uint8_t kt = 0;
    err = axp_read(HY_AXP_REG_KEY_TIMING, &kt);
    if (err == ESP_OK) {
        err = axp_rmw(HY_AXP_REG_KEY_TIMING, 0x00, hy_axp_key_timing(kt));
    }
    first = first != ESP_OK ? first : err;
    /* enableBattVoltageMeasure: ADC_CHANNEL_CTRL bit 0 */
    err = axp_rmw(HY_AXP_REG_ADC_CTRL, 0xFF, 0x01);
    first = first != ESP_OK ? first : err;
#if CONFIG_HY_PMU_CHARGER_SETUP
    err = hy_board_pmu_charger_setup();
    first = first != ESP_OK ? first : err;
#else
    ESP_LOGI(TAG, "pmu: charger setup off (CONFIG_HY_PMU_CHARGER_SETUP)");
#endif
    uint8_t c[4] = {0};
    bool ok = true;
    for (int i = 0; i < 4 && ok; i++) {
        ok = axp_read((uint8_t)(AXP_REG_IPRECHG + i), &c[i]) == ESP_OK;
    }
    if (ok) {
        ESP_LOGI(TAG, "pmu: charger 0x61=0x%02x 0x62=0x%02x 0x63=0x%02x 0x64=0x%02x", c[0], c[1], c[2], c[3]);
    }
    return first;
}

uint32_t hy_board_pmu_irq(void)
{
    if (!pmu_present()) {
        return 0;
    }
    uint8_t sts[3];
    for (int i = 0; i < 3; i++) {
        if (axp_read((uint8_t)(HY_AXP_REG_INTSTS1 + i), &sts[i]) != ESP_OK) {
            return 0;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (sts[i] && axp_write((uint8_t)(HY_AXP_REG_INTSTS1 + i), sts[i]) != ESP_OK) {
            return 0;
        }
    }
    return hy_axp_decode_irq(sts);
}

esp_err_t hy_board_pmu_read(hy_batt_raw_t *out)
{
    if (!pmu_present()) {
        return ESP_ERR_INVALID_STATE;
    }
    static const uint8_t regs[5] = {HY_AXP_REG_STATUS1, HY_AXP_REG_STATUS2, HY_AXP_REG_BAT_PERCENT,
                                    HY_AXP_REG_ADC_VBAT_H, HY_AXP_REG_ADC_VBAT_L};
    uint8_t v[5];
    for (int i = 0; i < 5; i++) {
        esp_err_t err = axp_read(regs[i], &v[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    hy_axp_decode_status(v[0], v[1], v[2], (uint16_t)(((v[3] & 0x1F) << 8) | v[4]), out);
    return ESP_OK;
}

void hy_board_pmu_power_off(void)
{
    if (!s_axp) {
        ESP_LOGE(TAG, "power off: no AXP2101 device");
        return;
    }
    axp_power_off();
}

#if CONFIG_HY_DEBUG_CONSOLE
void hy_board_pmu_dump(void)
{
    static const uint8_t regs[] = {HY_AXP_REG_STATUS1, HY_AXP_REG_STATUS2, HY_AXP_REG_BAT_PERCENT,
                                   HY_AXP_REG_ADC_VBAT_H, HY_AXP_REG_ADC_VBAT_L, HY_AXP_REG_INTEN1,
                                   HY_AXP_REG_INTEN2, HY_AXP_REG_INTEN3, HY_AXP_REG_PWROFF_EN,
                                   HY_AXP_REG_KEY_TIMING, HY_AXP_REG_ADC_CTRL, AXP_REG_TS_PIN_CTRL,
                                   AXP_REG_IPRECHG, AXP_REG_ICC_CHG, AXP_REG_ITERM_CHG, AXP_REG_CV_CHG};
    uint8_t v[sizeof regs];
    if (!pmu_present()) {
        ESP_LOGE(TAG, "pmu: no AXP2101");
        return;
    }
    for (size_t i = 0; i < sizeof regs; i++) {
        esp_err_t err = axp_read(regs[i], &v[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pmu: read failed at reg 0x%02x: %s", regs[i], esp_err_to_name(err));
            return;
        }
    }
    hy_batt_raw_t r;
    hy_axp_decode_status(v[0], v[1], v[2], (uint16_t)(((v[3] & 0x1F) << 8) | v[4]), &r);
    ESP_LOGI(TAG, "pmu: status1=0x%02x (vbus good %d, battery %d) status2=0x%02x (charger %d) 0xA4=0x%02x (%d%%) %u mV",
             v[0], r.vbus, r.present, v[1], v[1] & 0x07, v[2], r.pct, (unsigned)r.mv);
    ESP_LOGI(TAG, "pmu: inten 0x%02x 0x%02x 0x%02x, 0x22=0x%02x 0x27=0x%02x 0x30=0x%02x 0x50=0x%02x",
             v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
    ESP_LOGI(TAG, "pmu: charger 0x61=0x%02x 0x62=0x%02x 0x63=0x%02x 0x64=0x%02x", v[12], v[13], v[14], v[15]);
}
#endif

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
    s_panel = panel;

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
    touch_cfg.callbacks.on_interrupt = touch_isr;
    s_indev = esp_lv_adapter_register_touch(&touch_cfg);
    if (!s_indev) {
        ESP_LOGE(TAG, "register touch failed");
        return ESP_FAIL;
    }
    lv_indev_add_event_cb(s_indev, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_indev_set_long_press_time(s_indev, HY_LONG_PRESS_MS);

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

void hy_board_display_power(bool on)
{
    if (!s_panel) {
        return;
    }
    esp_err_t e1, e2;
    if (on) {
        e1 = esp_lcd_panel_disp_sleep(s_panel, false);
        e2 = esp_lcd_panel_disp_on_off(s_panel, true);
    } else {
        e1 = esp_lcd_panel_disp_on_off(s_panel, false);
        e2 = esp_lcd_panel_disp_sleep(s_panel, true);
    }
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGE(TAG, "display power %s: %s, %s", on ? "on" : "off", esp_err_to_name(e1), esp_err_to_name(e2));
    } else {
        ESP_LOGI(TAG, "display power %s", on ? "on" : "off");
    }
}

/* Runs inside the LVGL adapter's touch ISR (callbacks.on_interrupt); the adapter's handling is unchanged */
static void IRAM_ATTR touch_isr(esp_lcd_touch_handle_t tp, void *ctx)
{
    (void)tp;
    (void)ctx;
    void (*fn)(void) = s_touch_hook;
    if (fn) {
        fn();
    }
}

void hy_board_set_touch_isr_hook(void (*fn)(void))
{
    s_touch_hook = fn;
}

lv_display_t *hy_board_display(void)
{
    return s_ready ? s_disp : NULL;
}

lv_indev_t *hy_board_indev(void)
{
    return s_ready ? s_indev : NULL;
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
