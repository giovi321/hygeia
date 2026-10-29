/* Top bar on lv_layer_top: time left; crossed Wi-Fi arcs (Wi-Fi is off in this firmware), the charging bolt
 * and the battery percent right; the pull-down handle above the middle on Home. Nothing here takes touches. */

#include <stdio.h>
#include <string.h>

#include "hy/battery.h"
#include "hy/clock.h"
#include "hy/power_run.h"
#include "ui_internal.h"

#define PAD_X     24
#define WIFI_W    30
#define WIFI_H    24
#define WIFI_GAP  16
#define BOLT_W    10
#define BOLT_H    16
#define BOLT_GAP  6
#define GRAB_W    44
#define GRAB_H    5
#define GRAB_Y    6

static lv_obj_t *s_bar, *s_time, *s_wifi, *s_batt, *s_bolt, *s_grab;
static char s_time_txt[32];
/* what the battery widgets show now; a refresh with the same values does no LVGL work */
static char s_batt_txt[16], s_est_txt[16];
static bool s_batt_red, s_batt_bolt, s_batt_est, s_batt_known, s_batt_shown;

static const lv_point_precise_t slash_pts[] = {{3, 1}, {27, 23}};
static const lv_point_precise_t bolt_pts[] = {{8, 0}, {1, 10}, {9, 16}};

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    return o;
}

static void wifi_arc(lv_obj_t *parent, int32_t d)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style_all(a);
    lv_obj_set_clickable(a, false);
    lv_obj_set_size(a, d, d);
    /* concentric arcs whose centre is the bottom middle of the icon */
    lv_obj_set_pos(a, (WIFI_W - d) / 2, WIFI_H - d / 2 - 2);
    lv_arc_set_bg_angles(a, 225, 315);
    lv_obj_set_style_arc_width(a, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(HY_C_TEXT2), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
}

/* The top bar is the first child of lv_layer_top. Overlays that must cover it (idle clock, due card fade)
 * are created on lv_layer_top after topbar_create, or on lv_layer_sys. */
void topbar_create(void)
{
    s_bar = plain(lv_layer_top());
    lv_obj_set_size(s_bar, HY_SCREEN, HY_BAR_H);
    lv_obj_set_pos(s_bar, 0, 0);

    s_time = lv_label_create(s_bar);
    lv_obj_set_style_text_font(s_time, hy_type.time, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(HY_C_TEXT), 0);
    lv_label_set_text(s_time, HY_S_TIME_UNSET);
    lv_obj_align(s_time, LV_ALIGN_LEFT_MID, PAD_X, 0);

    s_batt = lv_label_create(s_bar);
    lv_obj_set_style_text_font(s_batt, hy_type.body, 0);
    lv_obj_set_style_text_color(s_batt, lv_color_hex(HY_C_TEXT2), 0);
    lv_label_set_text(s_batt, HY_S_BATTERY_NONE);
    lv_obj_align(s_batt, LV_ALIGN_RIGHT_MID, -PAD_X, 0);

    s_wifi = plain(s_bar);
    lv_obj_set_size(s_wifi, WIFI_W, WIFI_H);
    wifi_arc(s_wifi, 30);
    wifi_arc(s_wifi, 14);
    lv_obj_t *slash = lv_line_create(s_wifi);
    lv_line_set_points(slash, slash_pts, 2);
    lv_obj_set_style_line_width(slash, 3, 0);
    lv_obj_set_style_line_color(slash, lv_color_hex(HY_C_TEXT2), 0);
    lv_obj_set_style_line_rounded(slash, true, 0);
    lv_obj_align_to(s_wifi, s_batt, LV_ALIGN_OUT_LEFT_MID, -WIFI_GAP, 0);

    s_bolt = plain(s_bar);
    lv_obj_set_size(s_bolt, BOLT_W, BOLT_H);
    lv_obj_t *bl = lv_line_create(s_bolt);
    lv_line_set_points(bl, bolt_pts, 3);
    lv_obj_set_style_line_width(bl, 2, 0);
    lv_obj_set_style_line_color(bl, lv_color_hex(HY_C_TEXT), 0);
    lv_obj_set_style_line_rounded(bl, true, 0);
    lv_obj_set_hidden(s_bolt, true);

    s_grab = plain(s_bar);
    lv_obj_set_size(s_grab, GRAB_W, GRAB_H);
    lv_obj_set_pos(s_grab, (HY_SCREEN - GRAB_W) / 2, GRAB_Y);
    lv_obj_set_style_radius(s_grab, 3, 0);
    lv_obj_set_style_bg_color(s_grab, lv_color_hex(HY_C_GRAB), 0);
    lv_obj_set_style_bg_opa(s_grab, LV_OPA_COVER, 0);
    lv_obj_set_hidden(s_grab, true);

    topbar_battery_refresh();
    topbar_refresh();
}

/* The percent text, the red flag, the bolt and the estimate from the power task's snapshot. A PMU read error
 * or no battery reads as unknown. */
void ui_battery_text(char *pct, size_t pct_len, bool *red, bool *bolt, char *est, size_t est_len, bool *has_est,
                     bool *known)
{
    hy_power_state_t st;
    hy_power_state(&st);
    if (!st.pmu_ok) {
        st.batt.pct = -1;
        st.batt.raw.chg = HY_CHG_NONE;
    }
    hy_batt_text(&st.batt, pct, pct_len, HY_S_BATTERY_NONE, red, bolt);
    *has_est = hy_batt_estimate(&st.batt, est, est_len);
    if (!*has_est) {
        est[0] = '\0';
    }
    *known = st.batt.pct >= 0;
}

void topbar_battery_refresh(void)
{
    char pct[sizeof s_batt_txt], est[sizeof s_est_txt];
    bool red, bolt, has_est, known;
    ui_battery_text(pct, sizeof pct, &red, &bolt, est, sizeof est, &has_est, &known);
    if (s_batt_shown && strcmp(pct, s_batt_txt) == 0 && red == s_batt_red && bolt == s_batt_bolt &&
        has_est == s_batt_est && strcmp(est, s_est_txt) == 0 && known == s_batt_known) {
        return;
    }
    bool bar_changed = !s_batt_shown || strcmp(pct, s_batt_txt) != 0 || red != s_batt_red || bolt != s_batt_bolt ||
                       known != s_batt_known;
    snprintf(s_batt_txt, sizeof s_batt_txt, "%s", pct);
    snprintf(s_est_txt, sizeof s_est_txt, "%s", est);
    s_batt_red = red;
    s_batt_bolt = bolt;
    s_batt_est = has_est;
    s_batt_known = known;
    s_batt_shown = true;
    if (bar_changed) {
        uint32_t col = red ? HY_C_MISS : (known ? HY_C_TEXT : HY_C_TEXT2);
        lv_label_set_text(s_batt, pct);
        lv_obj_set_style_text_color(s_batt, lv_color_hex(col), 0);
        lv_obj_set_style_line_color(lv_obj_get_child(s_bolt, 0), lv_color_hex(col), 0);
        lv_obj_set_hidden(s_bolt, !bolt);
        lv_obj_update_layout(s_bar);   /* the label took its new width */
        lv_obj_align(s_batt, LV_ALIGN_RIGHT_MID, -PAD_X, 0);
        if (bolt) {
            lv_obj_align_to(s_bolt, s_batt, LV_ALIGN_OUT_LEFT_MID, -BOLT_GAP, 0);
            lv_obj_align_to(s_wifi, s_bolt, LV_ALIGN_OUT_LEFT_MID, -WIFI_GAP, 0);
        } else {
            lv_obj_align_to(s_wifi, s_batt, LV_ALIGN_OUT_LEFT_MID, -WIFI_GAP, 0);
        }
    }
    idle_status_refresh();
    qs_battery_refresh();
}

void topbar_refresh(void)
{
    const hy_clock_port_t *ck = hy_clock_port();
    char b[32];
    if (ck->valid(NULL)) {
        char tz[HY_TZ_LEN];
        hy_clock_tz_copy(tz, sizeof tz);
        ui_fmt_hhmm(ck->now(NULL), tz, b, sizeof b);
    } else {
        snprintf(b, sizeof b, "%s", HY_S_TIME_UNSET);
    }
    if (strcmp(b, s_time_txt) != 0) {
        snprintf(s_time_txt, sizeof s_time_txt, "%s", b);
        lv_label_set_text(s_time, b);
    }
}

void topbar_set_visible(bool on)
{
    lv_obj_set_hidden(s_bar, !on);
}

void topbar_set_grab(bool on)
{
    lv_obj_set_hidden(s_grab, !on);
}
