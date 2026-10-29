/* Settings and its sub-views: Time zone (region and city rollers), Sound (volume slider, Play chime), Display
 * (brightness slider) and About. Date and time is timeset.c, Wi-Fi is a placeholder until plan 4.
 * The list scrolls and consumes its scroll strokes; every row acts on a still tap only. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "hy/audio.h"
#include "hy/board.h"
#include "hy/clock.h"
#include "hy/occ.h"
#include "hy/store.h"
#include "hy/tzdb.h"
#include "ui_internal.h"

static const char *TAG = "ui_settings";

#define MARGIN 18
#define ROW_H  72
#define ROW_W  (HY_SCREEN - 2 * MARGIN)
#define MAX_REGIONS 16

enum { R_ZONE, R_DATETIME, R_SOUND, R_DISPLAY, R_WIFI, R_ABOUT, R_N };

static lv_obj_t *s_root, *s_list, *s_val[R_N];
static lv_obj_t *s_zone_root, *s_region, *s_city;
static lv_obj_t *s_sound_root, *s_vol_lbl, *s_vol_slider;
static lv_obj_t *s_disp_root, *s_bright_lbl, *s_bright_slider;
static lv_obj_t *s_about_root, *s_about[4];
static const char *s_regions[MAX_REGIONS];
static size_t s_nreg, s_city_first, s_city_n;

/* ---- shared setting changes ---- */

static void save(const hy_settings_t *s)
{
    esp_err_t err = hy_settings_update(s);
    ESP_LOGI(TAG, "settings saved: %s (sound %d, dnd %d, volume %u, brightness %u)", esp_err_to_name(err), s->sound,
             s->dnd, (unsigned)s->volume, (unsigned)s->brightness);
}

void settings_brightness_live(int32_t pct)
{
    idle_brightness_stop();
    hy_board_set_brightness((uint8_t)pct);
}

void settings_brightness_commit(int32_t pct)
{
    hy_settings_t s = *hy_settings();
    s.brightness = (uint8_t)pct;
    save(&s);
}

static void volume_live(int32_t pct)
{
    hy_audio_set_volume((uint8_t)pct);
    lv_label_set_text_fmt(s_vol_lbl, HY_S_SET_VOLUME, (unsigned)pct);
}

static void volume_commit(int32_t pct)
{
    hy_settings_t s = *hy_settings();
    s.volume = (uint8_t)pct;
    save(&s);
}

static void display_live(int32_t pct)
{
    settings_brightness_live(pct);
    lv_label_set_text_fmt(s_bright_lbl, HY_S_SET_BRIGHT, (unsigned)pct);
}

static void play_tap(void *ud)
{
    (void)ud;
    hy_audio_chime();   /* plays regardless of the Sound and Do not disturb toggles */
}

/* ---- time zone ---- */

/* Etc/GMT+5 follows the IANA sign convention and means UTC-5; those entries show as "GMT+5" in the city roller. */
static bool s_need_change;   /* no stored zone matched: Set waits for a roller turn */

static void build_cities(size_t region)
{
    const char *reg = s_regions[region];
    size_t rl = strlen(reg), total = 0, n = 0, first = 0, count = hy_tzdb_count();
    for (size_t i = 0; i < count; i++) {
        const char *iana;
        hy_tzdb_at(i, &iana, NULL);
        if (strncmp(iana, reg, rl) == 0 && iana[rl] == '/') {
            if (n == 0) {
                first = i;
            }
            n++;
            total += strlen(iana) - rl;   /* the name after the slash plus one separator */
        }
    }
    char *buf = malloc(total + 1);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for the city list");
        s_city_n = 0;   /* the roller keeps its old options: Set must not map them to the new region */
        return;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const char *iana;
        hy_tzdb_at(first + i, &iana, NULL);
        const char *c = iana + rl + 1;
        if (i) {
            buf[o++] = '\n';
        }
        for (; *c; c++) {
            buf[o++] = *c == '_' ? ' ' : *c;
        }
    }
    buf[o] = '\0';
    lv_roller_set_options(s_city, buf, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(s_city, 0, LV_ANIM_OFF);
    s_city_first = first;
    s_city_n = n;
    free(buf);
}

static void city_changed_cb(lv_event_t *e)
{
    (void)e;
    s_need_change = false;
}

static void region_changed_cb(lv_event_t *e)
{
    (void)e;
    s_need_change = false;
    build_cities(lv_roller_get_selected(s_region));
}

static void select_zone(const char *iana_in)
{
    const char *iana = iana_in;
    s_need_change = false;
    if (!iana[0]) {
        /* only a POSIX string is stored: find the first table zone with the same rules */
        char tz[HY_TZ_LEN];
        hy_clock_tz_copy(tz, sizeof tz);
        s_need_change = true;
        for (size_t i = 0; i < hy_tzdb_count(); i++) {
            const char *n, *p;
            hy_tzdb_at(i, &n, &p);
            if (strcmp(p, tz) == 0) {
                iana = n;
                s_need_change = false;
                break;
            }
        }
    }
    size_t ri = 0;
    const char *slash = strchr(iana, '/');
    if (slash) {
        for (size_t i = 0; i < s_nreg; i++) {
            if (strlen(s_regions[i]) == (size_t)(slash - iana) && strncmp(iana, s_regions[i], (size_t)(slash - iana)) == 0) {
                ri = i;
                break;
            }
        }
    }
    lv_roller_set_selected(s_region, (uint32_t)ri, LV_ANIM_OFF);
    build_cities(ri);
    for (size_t i = 0; i < s_city_n; i++) {
        const char *n;
        hy_tzdb_at(s_city_first + i, &n, NULL);
        if (strcmp(n, iana) == 0) {
            lv_roller_set_selected(s_city, (uint32_t)i, LV_ANIM_OFF);
            break;
        }
    }
}

esp_err_t settings_set_zone(const char *iana)
{
    const char *posix = hy_tzdb_posix(iana);
    if (!posix) {
        ESP_LOGE(TAG, "zone: %s is not in the table", iana);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = hy_clock_set_zone(iana, posix);
    ESP_LOGI(TAG, "zone set to %s (%s): %s", iana, posix, esp_err_to_name(err));
    if (err == ESP_OK) {
        ui_time_changed();
    }
    return err;
}

static void zone_set_tap(void *ud)
{
    (void)ud;
    uint32_t c = lv_roller_get_selected(s_city);
    if (s_need_change || c >= s_city_n) {
        return;
    }
    const char *iana;
    hy_tzdb_at(s_city_first + c, &iana, NULL);
    esp_err_t err = settings_set_zone(iana);
    if (err == ESP_OK) {
        ui_nav_pop();
    } else if (err == ESP_ERR_INVALID_STATE) {
        toast_show_info(HY_S_ERR_CLOCK);
    } else {
        toast_show_info(HY_S_ERR_IO);
    }
}

static void zone_create(void)
{
    s_zone_root = ui_view_box();
    ui_register_view(HY_V_SET_ZONE, s_zone_root);
    ui_view_title(s_zone_root, HY_S_SET_ZONE);

    s_nreg = hy_tzdb_regions(s_regions, MAX_REGIONS);
    char opts[MAX_REGIONS * 12];
    size_t o = 0;
    for (size_t i = 0; i < s_nreg; i++) {
        o += (size_t)snprintf(opts + o, sizeof opts - o, "%s%s", i ? "\n" : "", s_regions[i]);
    }
    s_region = ui_roller(s_zone_root, opts, LV_ROLLER_MODE_NORMAL, 150, hy_type.body_bold, region_changed_cb);
    lv_obj_set_pos(s_region, MARGIN, 60);
    s_city = ui_roller(s_zone_root, "-", LV_ROLLER_MODE_NORMAL, HY_SCREEN - 2 * MARGIN - 150 - 12, hy_type.body_bold, city_changed_cb);
    lv_obj_set_pos(s_city, MARGIN + 150 + 12, 60);
    lv_obj_t *b = ui_button(s_zone_root, ROW_W, 66, hy_type.body_bold, HY_S_SET_SET, zone_set_tap, NULL);
    lv_obj_set_pos(b, MARGIN, 346);
}

/* ---- sound, display ---- */

static void sound_create(void)
{
    s_sound_root = ui_view_box();
    ui_register_view(HY_V_SET_SOUND, s_sound_root);
    ui_view_title(s_sound_root, HY_S_SET_SOUND);
    s_vol_lbl = ui_label(s_sound_root, hy_type.body, HY_C_TEXT, "");
    lv_obj_set_pos(s_vol_lbl, 24, 72);
    s_vol_slider = ui_slider(s_sound_root, ROW_W, 0, 100, volume_live, volume_commit);
    lv_obj_set_pos(s_vol_slider, MARGIN, 116);
    lv_obj_t *b = ui_button(s_sound_root, ROW_W, 66, hy_type.body_bold, HY_S_SET_PLAY, play_tap, NULL);
    lv_obj_set_pos(b, MARGIN, 176);
}

static void display_create(void)
{
    s_disp_root = ui_view_box();
    ui_register_view(HY_V_SET_DISPLAY, s_disp_root);
    ui_view_title(s_disp_root, HY_S_SET_DISPLAY);
    s_bright_lbl = ui_label(s_disp_root, hy_type.body, HY_C_TEXT, "");
    lv_obj_set_pos(s_bright_lbl, 24, 72);
    s_bright_slider = ui_slider(s_disp_root, ROW_W, 10, 100, display_live, settings_brightness_commit);
    lv_obj_set_pos(s_bright_slider, MARGIN, 116);
}

/* ---- about ---- */

static void fmt_bytes(size_t b, char *out, size_t len)
{
    const size_t mib = 1024u * 1024u;
    if (b < mib) {
        snprintf(out, len, HY_S_KB, (unsigned)((b + 1023) / 1024));
    } else {
        unsigned tenths = (unsigned)(((uint64_t)b * 10 + mib / 2) / mib);
        snprintf(out, len, HY_S_MB, tenths / 10, tenths % 10);
    }
}

static void about_refresh(void)
{
    char t[96], a[24], b[24];
    lv_label_set_text_fmt(s_about[0], HY_S_ABOUT_VERSION, esp_app_get_description()->version);

    char src[24] = "";
    int64_t at = 0;
    hy_clock_last_sync(src, sizeof src, &at);
    const hy_clock_port_t *ck = hy_clock_port();
    if (src[0] && at > 0 && ck->valid(NULL)) {
        int64_t min = (ck->now(NULL) - at) / 60;
        ui_fmt_dur(min < 0 ? 0 : min, a, sizeof a);
        snprintf(t, sizeof t, HY_S_ABOUT_SYNCED, a, src);
    } else {
        snprintf(t, sizeof t, HY_S_ABOUT_UNSYNCED);
    }
    lv_label_set_text(s_about[1], t);

    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) == ESP_OK) {
        fmt_bytes(used, a, sizeof a);
        fmt_bytes(total, b, sizeof b);
        snprintf(t, sizeof t, HY_S_ABOUT_STORAGE, a, b);
    } else {
        snprintf(t, sizeof t, HY_S_ABOUT_STORAGE, HY_S_DASH, HY_S_DASH);
    }
    lv_label_set_text(s_about[2], t);

    const hy_med_t *ms;
    size_t n = hy_meds(&ms), act = 0, arch = 0;
    for (size_t i = 0; i < n; i++) {
        if (ms[i].archived) {
            arch++;
        } else {
            act++;
        }
    }
    lv_label_set_text_fmt(s_about[3], HY_S_ABOUT_MEDS, (unsigned)act, (unsigned)arch);
}

static void about_create(void)
{
    s_about_root = ui_view_box();
    ui_register_view(HY_V_ABOUT, s_about_root);
    ui_view_title(s_about_root, HY_S_SET_ABOUT);
    for (int i = 0; i < 4; i++) {
        s_about[i] = ui_label(s_about_root, hy_type.body, i ? HY_C_TEXT2 : HY_C_TEXT, "");
        lv_obj_set_pos(s_about[i], 24, 64 + i * 44);
    }
}

/* ---- the list ---- */

static void row_tap(void *ud)
{
    switch ((int)(uintptr_t)ud) {
    case R_ZONE:
        ui_nav_push(HY_V_SET_ZONE);
        break;
    case R_DATETIME:
        ui_nav_push(HY_V_SET_TIME);
        break;
    case R_SOUND:
        ui_nav_push(HY_V_SET_SOUND);
        break;
    case R_DISPLAY:
        ui_nav_push(HY_V_SET_DISPLAY);
        break;
    case R_ABOUT:
        ui_nav_push(HY_V_ABOUT);
        break;
    default:
        toast_show_info(HY_S_NOT_YET);   /* Wi-Fi: plan 4 */
        break;
    }
}

static void list_scroll_cb(lv_event_t *e)
{
    (void)e;
    ui_input_consume();   /* a stroke that scrolls the list must not also navigate */
}

static void make_row(int idx, const char *title)
{
    lv_obj_t *r = lv_obj_create(s_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, ROW_W, ROW_H);
    lv_obj_set_style_radius(r, 22, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(HY_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(HY_C_SURFACE2), LV_STATE_PRESSED);
    lv_obj_set_scrollable(r, false);
    lv_obj_set_scroll_chain(r, true);
    if (idx == R_WIFI) {
        lv_obj_set_style_opa(r, LV_OPA_40, 0);
    }
    ui_on_tap(r, row_tap, (void *)(uintptr_t)idx);
    lv_obj_t *t = ui_label(r, hy_type.body, HY_C_TEXT, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 22, 0);
    s_val[idx] = ui_label(r, hy_type.status, HY_C_TEXT2, "");
    lv_obj_set_width(s_val[idx], 250);
    lv_label_set_long_mode(s_val[idx], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_val[idx], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_val[idx], LV_ALIGN_RIGHT_MID, -22, 0);
}

static void list_refresh(void)
{
    const hy_settings_t *st = hy_settings();
    char t[48], tz[HY_TZ_LEN];
    char zone[HY_ZONE_LEN];
    hy_clock_zone_copy(zone, sizeof zone);
    hy_clock_tz_copy(tz, sizeof tz);
    lv_label_set_text(s_val[R_ZONE], zone[0] ? zone : tz);

    const hy_clock_port_t *ck = hy_clock_port();
    if (ck->valid(NULL)) {
        hy_format_local(ck->now(NULL), tz, t, sizeof t);
    } else {
        snprintf(t, sizeof t, "%s", HY_S_DASH);
    }
    lv_label_set_text(s_val[R_DATETIME], t);
    lv_label_set_text_fmt(s_val[R_SOUND], HY_S_SET_VOLUME, (unsigned)st->volume);
    lv_label_set_text_fmt(s_val[R_DISPLAY], HY_S_SET_BRIGHT, (unsigned)st->brightness);
    lv_label_set_text(s_val[R_WIFI], HY_S_OFF);
    lv_label_set_text(s_val[R_ABOUT], esp_app_get_description()->version);
}

/* The Date and time value follows the clock while the list is open */
static void clock_row_cb(lv_timer_t *t)
{
    (void)t;
    if (ui_current_view() != HY_V_SETTINGS) {
        return;
    }
    char tz[HY_TZ_LEN], b[48];
    hy_clock_tz_copy(tz, sizeof tz);
    const hy_clock_port_t *ck = hy_clock_port();
    if (ck->valid(NULL)) {
        hy_format_local(ck->now(NULL), tz, b, sizeof b);
    } else {
        snprintf(b, sizeof b, "%s", HY_S_DASH);
    }
    if (strcmp(lv_label_get_text(s_val[R_DATETIME]), b) != 0) {
        lv_label_set_text(s_val[R_DATETIME], b);
    }
}

void settings_show(hy_view_t v)
{
    const hy_settings_t *st = hy_settings();
    char zone[HY_ZONE_LEN];
    switch (v) {
    case HY_V_SETTINGS:
        list_refresh();
        lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
        break;
    case HY_V_SET_ZONE:
        hy_clock_zone_copy(zone, sizeof zone);
        select_zone(zone);
        break;
    case HY_V_SET_SOUND:
        lv_slider_set_value(s_vol_slider, st->volume, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_vol_lbl, HY_S_SET_VOLUME, (unsigned)st->volume);
        break;
    case HY_V_SET_DISPLAY:
        lv_slider_set_value(s_bright_slider, st->brightness, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_bright_lbl, HY_S_SET_BRIGHT, (unsigned)st->brightness);
        break;
    case HY_V_ABOUT:
        about_refresh();
        break;
    default:
        break;
    }
}

void settings_create(void)
{
    s_root = ui_view_box();
    ui_register_view(HY_V_SETTINGS, s_root);
    ui_view_title(s_root, HY_S_SET_TITLE);

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
    lv_obj_set_scroll_elastic(s_list, false);
    lv_obj_set_scroll_momentum(s_list, false);
    lv_obj_add_event_cb(s_list, list_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    make_row(R_ZONE, HY_S_SET_ZONE);
    make_row(R_DATETIME, HY_S_SET_DATETIME);
    make_row(R_SOUND, HY_S_SET_SOUND);
    make_row(R_DISPLAY, HY_S_SET_DISPLAY);
    make_row(R_WIFI, HY_S_SET_WIFI);
    make_row(R_ABOUT, HY_S_SET_ABOUT);

    lv_timer_create(clock_row_cb, 1000, NULL);
    zone_create();
    sound_create();
    display_create();
    about_create();
}

#if CONFIG_HY_DEBUG_CONSOLE
void settings_debug_log(void)
{
    const hy_settings_t *s = hy_settings();
    ESP_LOGI(TAG, "settings: sound %d dnd %d volume %u brightness %u snooze %u min", s->sound, s->dnd,
             (unsigned)s->volume, (unsigned)s->brightness, (unsigned)s->snooze_min);
}
#endif
