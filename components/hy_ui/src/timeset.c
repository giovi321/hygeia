/* Set the time: rollers day, month, year, hour and minute in the current zone, and "Set". The same widget is
 * the Date and time sub-view of Settings and, in invalid mode, the CLOCK_INVALID root with its header and the
 * explanation. The default is the current time, or the build time while the clock is invalid. An impossible date
 * (31 Feb) is refused with the info toast. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "hy/clock.h"
#include "hy/tzlock.h"
#include "ui_internal.h"

static const char *TAG = "ui_timeset";

#define MARGIN     24
#define YEAR_FIRST 2026
#define YEAR_LAST  2099

typedef struct {
    lv_obj_t *day, *mon, *year, *hour, *min, *zone_line;
    bool invalid;
} inst_t;

static inst_t s_inst[2];   /* [0] the Settings sub-view, [1] the CLOCK_INVALID root */

static void tm_of(int64_t ts, struct tm *tm)
{
    time_t t = (time_t)ts;
    hy_tz_lock();   /* the process TZ is hy_clock's zone outside this lock */
    localtime_r(&t, tm);
    hy_tz_unlock();
}

void timeset_show(bool invalid_mode)
{
    inst_t *in = &s_inst[invalid_mode ? 1 : 0];
    const hy_clock_port_t *ck = hy_clock_port();
    /* While the clock is invalid the default is the build time rounded up to the minute: hy_clock_set refuses
     * anything before it. */
    int64_t ts = ck->valid(NULL) ? ck->now(NULL) : (hy_clock_build_epoch() + 59) / 60 * 60;
    struct tm tm;
    tm_of(ts, &tm);
    int y = tm.tm_year + 1900 - YEAR_FIRST;
    if (y < 0) {
        y = 0;
    } else if (y > YEAR_LAST - YEAR_FIRST) {
        y = YEAR_LAST - YEAR_FIRST;
    }
    lv_roller_set_selected(in->day, (uint32_t)(tm.tm_mday - 1), LV_ANIM_OFF);
    lv_roller_set_selected(in->mon, (uint32_t)tm.tm_mon, LV_ANIM_OFF);
    lv_roller_set_selected(in->year, (uint32_t)y, LV_ANIM_OFF);
    lv_roller_set_selected(in->hour, (uint32_t)tm.tm_hour, LV_ANIM_OFF);
    lv_roller_set_selected(in->min, (uint32_t)tm.tm_min, LV_ANIM_OFF);
    if (in->zone_line) {
        char zone[HY_ZONE_LEN], tz[HY_TZ_LEN], t[80];
        hy_clock_zone_copy(zone, sizeof zone);
        hy_clock_tz_copy(tz, sizeof tz);
        snprintf(t, sizeof t, HY_S_TIME_ZONE_LINE, zone[0] ? zone : tz);
        lv_label_set_text(in->zone_line, t);
    }
}

static void set_tap(void *ud)
{
    inst_t *in = ud;
    struct tm tm = {0};
    int day = (int)lv_roller_get_selected(in->day) + 1;
    int mon = (int)lv_roller_get_selected(in->mon);
    tm.tm_mday = day;
    tm.tm_mon = mon;
    tm.tm_year = (int)lv_roller_get_selected(in->year) + YEAR_FIRST - 1900;
    tm.tm_hour = (int)lv_roller_get_selected(in->hour);
    tm.tm_min = (int)lv_roller_get_selected(in->min);
    tm.tm_isdst = -1;
    hy_tz_lock();
    int hour = tm.tm_hour, minute = tm.tm_min;
    time_t t = mktime(&tm);   /* normalises 31 Feb into March; a time in a DST gap lands on another wall time */
    struct tm back;
    localtime_r(&t, &back);   /* the wall time the instant really has: mktime may leave tm as given */
    hy_tz_unlock();
    int64_t ts = (int64_t)t;
    if (back.tm_mday != day || back.tm_mon != mon) {
        toast_show_info(HY_S_TIME_BAD_DATE);
        return;
    }
    if (back.tm_hour != hour || back.tm_min != minute) {
        toast_show_info(HY_S_TIME_BAD_TIME);
        return;
    }
    uint32_t seq0 = hy_clock_sync_seq(NULL);
    esp_err_t err = hy_clock_set(ts, NULL, "device");
    ESP_LOGI(TAG, "set %04d-%02d-%02d %02d:%02d: %s", tm.tm_year + 1900, mon + 1, day, tm.tm_hour, tm.tm_min,
             esp_err_to_name(err));
    if (err == ESP_ERR_INVALID_ARG) {
        toast_show_info(HY_S_TIME_TOO_EARLY);
        return;
    }
    if (hy_clock_sync_seq(NULL) == seq0) {
        toast_show_info(HY_S_ERR_IO);   /* the RTC write failed: nothing changed */
        return;
    }
    /* the seq moved: the time is set even when err says the zone NVS write failed */
    ui_time_changed();   /* from the invalid root this also switches to Home */
    if (!in->invalid && ui_current_view() == HY_V_SET_TIME) {
        ui_nav_pop();
    }
}

void timeset_create(lv_obj_t *parent, bool invalid_mode)
{
    inst_t *in = &s_inst[invalid_mode ? 1 : 0];
    in->invalid = invalid_mode;
    int ry = 72;
    if (invalid_mode) {
        ui_view_title(parent, HY_S_TIME_TITLE);
        lv_obj_t *b = ui_label(parent, hy_type.status, HY_C_TEXT2, HY_S_TIME_BODY);
        lv_obj_set_width(b, HY_SCREEN - 2 * MARGIN);
        lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_pos(b, MARGIN, 40);
        in->zone_line = ui_label(parent, hy_type.status, HY_C_TEXT2, "");
        lv_obj_set_pos(in->zone_line, MARGIN, 90);
        ry = 124;
    } else {
        ui_view_title(parent, HY_S_SET_DATETIME);
    }

    char days[31 * 3 + 1], mins[60 * 3 + 1], hours[24 * 3 + 1], years[(YEAR_LAST - YEAR_FIRST + 1) * 5 + 1];
    size_t o = 0;
    for (int i = 1; i <= 31; i++) {
        o += (size_t)snprintf(days + o, sizeof days - o, "%s%d", i > 1 ? "\n" : "", i);
    }
    o = 0;
    for (int i = 0; i < 24; i++) {
        o += (size_t)snprintf(hours + o, sizeof hours - o, "%s%02d", i ? "\n" : "", i);
    }
    o = 0;
    for (int i = 0; i < 60; i++) {
        o += (size_t)snprintf(mins + o, sizeof mins - o, "%s%02d", i ? "\n" : "", i);
    }
    o = 0;
    for (int i = YEAR_FIRST; i <= YEAR_LAST; i++) {
        o += (size_t)snprintf(years + o, sizeof years - o, "%s%d", i > YEAR_FIRST ? "\n" : "", i);
    }

    int x = MARGIN;
    in->day = ui_roller(parent, days, LV_ROLLER_MODE_INFINITE, 66, hy_type.time, NULL);
    lv_obj_set_pos(in->day, x, ry);
    x += 66 + 12;
    in->mon = ui_roller(parent, HY_S_MONTHS, LV_ROLLER_MODE_INFINITE, 84, hy_type.time, NULL);
    lv_obj_set_pos(in->mon, x, ry);
    x += 84 + 12;
    in->year = ui_roller(parent, years, LV_ROLLER_MODE_NORMAL, 104, hy_type.time, NULL);
    lv_obj_set_pos(in->year, x, ry);
    x += 104 + 12;
    in->hour = ui_roller(parent, hours, LV_ROLLER_MODE_INFINITE, 66, hy_type.time, NULL);
    lv_obj_set_pos(in->hour, x, ry);
    x += 66 + 12;
    in->min = ui_roller(parent, mins, LV_ROLLER_MODE_INFINITE, 66, hy_type.time, NULL);
    lv_obj_set_pos(in->min, x, ry);

    lv_obj_t *b = ui_button(parent, HY_SCREEN - 36, 66, hy_type.body_bold, HY_S_SET_SET, set_tap, in);
    lv_obj_set_pos(b, 18, 346);
}

#if CONFIG_HY_DEBUG_CONSOLE
/* Turns the Settings sub-view's rollers as a finger would, then taps Set */
void timeset_debug_set(int year, int month, int day, int hour, int minute)
{
    inst_t *in = &s_inst[0];
    if (year < YEAR_FIRST || year > YEAR_LAST || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 ||
        hour > 23 || minute < 0 || minute > 59) {
        ESP_LOGE(TAG, "usage: qs time <2026-2099> <1-12> <1-31> <0-23> <0-59>");
        return;
    }
    lv_roller_set_selected(in->day, (uint32_t)(day - 1), LV_ANIM_OFF);
    lv_roller_set_selected(in->mon, (uint32_t)(month - 1), LV_ANIM_OFF);
    lv_roller_set_selected(in->year, (uint32_t)(year - YEAR_FIRST), LV_ANIM_OFF);
    lv_roller_set_selected(in->hour, (uint32_t)hour, LV_ANIM_OFF);
    lv_roller_set_selected(in->min, (uint32_t)minute, LV_ANIM_OFF);
    set_tap(in);
}
#endif
