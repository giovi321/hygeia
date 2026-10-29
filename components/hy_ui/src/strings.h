/*
 * Every user-visible string of the device UI, English only. No literal user text lives in the .c files.
 *
 * Reached only through the quoted include in ui_internal.h: "src" is deliberately not an include directory of
 * this component, so the C library's <strings.h> is never shadowed by this file.
 */
#ifndef HY_UI_STRINGS_H
#define HY_UI_STRINGS_H

/* U+00A0 no-break space, U+00B7 middle dot, U+2014 em dash, U+2026 ellipsis, U+2039/U+203A single guillemets */
#define HY_S_NB    "\xC2\xA0"
#define HY_S_DOT   " \xC2\xB7 "
#define HY_S_DASH  "\xE2\x80\x94"
#define HY_S_ELL   "\xE2\x80\xA6"
#define HY_S_LSAQ  "\xE2\x80\xB9"
#define HY_S_RSAQ  "\xE2\x80\xBA"

/* ---- durations: number and unit joined by a no-break space so a wrapped line never splits them ---- */
#define HY_S_DUR_LT1M  "<1" HY_S_NB "m"
#define HY_S_DUR_D     "%d" HY_S_NB "d"
#define HY_S_DUR_DH    "%d" HY_S_NB "d %d" HY_S_NB "h"
#define HY_S_DUR_H     "%d" HY_S_NB "h"
#define HY_S_DUR_HM    "%d" HY_S_NB "h %d" HY_S_NB "m"
#define HY_S_DUR_M     "%d" HY_S_NB "m"

/* ---- top bar ---- */
#define HY_S_TIME_UNSET    "--:--"
#define HY_S_BATTERY_NONE  HY_S_DASH "%"            /* battery unknown (no battery or PMU error) */

/* ---- home tiles ---- */
#define HY_S_TILE_AS_NEEDED   "as needed"
#define HY_S_TILE_NO_DOSE     "no dose yet"
#define HY_S_TILE_JUST_NOW    "just now"
#define HY_S_TILE_AGO         "%s ago"              /* duration */
#define HY_S_TILE_DUE         "Due now"
#define HY_S_TILE_LATE        "Late"
#define HY_S_TILE_SNOOZED     "Snoozed" HY_S_DOT "%s" /* HH:MM */
#define HY_S_TILE_MISSED      "Missed"
#define HY_S_TILE_CLOCK_UNSET "clock not set"
#define HY_S_PLUS             "+"
#define HY_S_ADD_IN_WEB       "Add medicines in the web UI"

/* ---- logging from a tile ---- */
#define HY_S_TOO_SOON      "Last dose %s ago, minimum %s. Tap again to log" /* durations */
#define HY_S_ERR_CLOCK     "Set the time first"
#define HY_S_ERR_NOT_FOUND "Medicine not found"
#define HY_S_ERR_IO        "Storage error"
#define HY_S_ERR_FULL      "Too many entries"
#define HY_S_ERR_LOG       "Could not log"
#define HY_S_ERR_SNOOZE    "Could not snooze"

/* ---- toasts ---- */
#define HY_S_LOGGED        "%s logged %s"           /* code, HH:MM */
#define HY_S_SKIPPED       "%s skipped %s"          /* code, HH:MM */
#define HY_S_UNDO          "Undo %u"                /* seconds left */
#define HY_S_UNDO_EXPIRED  "Too late to undo"
#define HY_S_TOAST_BANG    "!"
#define HY_S_NOT_YET       "Not available yet"
#define HY_S_BATT_LOW      "Battery low" HY_S_DOT "%d%%"  /* percent */

/* ---- storage-down root ---- */
#define HY_S_STORAGE_TITLE "Storage unavailable"
#define HY_S_STORAGE_BODY  "Medicines and the log cannot be read. Reminders are off."

/* ---- due card ---- */
#define HY_S_DUE_NAME      "%s" HY_S_DOT "%s"       /* name, dose */
#define HY_S_DUE_AT        "Due %s"                 /* HH:MM */
#define HY_S_DUE_LATE      "Late" HY_S_DOT "due %s" /* HH:MM */
#define HY_S_DUE_MORE      HY_S_DOT "+%u more"
#define HY_S_DUE_SLIDE     "Slide to log as taken " HY_S_RSAQ
#define HY_S_DUE_SNOOZE    HY_S_LSAQ " swipe left: snooze %u min"
#define HY_S_DUE_SKIP      "Skip"
#define HY_S_SNOOZED_UNTIL "Snoozed until %s"       /* HH:MM */

/* ---- overview ---- */
#define HY_S_OV_TITLE      "Today"
#define HY_S_OV_DOSES      "doses"
#define HY_S_OV_COUNT      "%u/%u"                  /* taken, total */
#define HY_S_OV_TL_0       "00"                     /* timeline hour labels */
#define HY_S_OV_TL_12      "12"
#define HY_S_OV_TL_24      "24"
#define HY_S_OV_DUE        "due"
#define HY_S_OV_LATE       "late"
#define HY_S_OV_MISSED     "missed"
#define HY_S_OV_SKIPPED    "skipped"
#define HY_S_OV_UNTRACKED  HY_S_DASH
#define HY_S_OV_AS_NEEDED  "as needed"
#define HY_S_OV_AS_NEEDED_N "as needed" HY_S_DOT "%u today"

/* ---- history ---- */
#define HY_S_HI_TITLE_7    "Last 7 days"
#define HY_S_HI_TITLE_30   "Last 30 days"
#define HY_S_HI_WEEKDAYS   "MTWTFSS"                /* initials, Monday first */
#define HY_S_HI_ON_TIME    "on time"
#define HY_S_HI_LATE       "late"
#define HY_S_HI_MISSED     "missed"
#define HY_S_HI_MONTHS     "JanFebMarAprMayJunJulAugSepOctNovDec" /* three letters each */
#define HY_S_HI_TICK       "%d %.3s"                /* day, month name: "19 Oct" */
#define HY_S_ADHERENCE     "Adherence %u%%"
#define HY_S_ADHERENCE_NONE "Adherence " HY_S_DASH

/* ---- medicine detail, note, earlier ---- */
#define HY_S_DET_PILLS     "%u tablets left"
#define HY_S_DET_PILL_1    "1 tablet left"
#define HY_S_DET_AS_NEEDED "as needed, at least %s apart" /* duration */
#define HY_S_DET_NO_LOGS   "No doses logged yet"
#define HY_S_DET_SKIPPED   "skipped"
#define HY_S_DET_MISSED    "missed"
#define HY_S_DET_NOTE      "Log with note"
#define HY_S_DET_EARLIER   "Log earlier" HY_S_ELL
#define HY_S_DET_30_DAYS   "Last 30 days"
#define HY_S_DET_SEP       ", "                     /* between schedule times */
#define HY_S_NOTE_LOG      "Log"
#define HY_S_NOTE_HINT     "Note (optional)"
#define HY_S_TODAY_AT      "Today %s"               /* HH:MM */
#define HY_S_YESTERDAY_AT  "Yesterday %s"           /* HH:MM */
#define HY_S_LOG_AT        "Log at %s"              /* HH:MM */
#define HY_S_YESTERDAY_LC  "yesterday %s"           /* HH:MM, inside the logged toast */
#define HY_S_COLON         ":"
#define HY_S_EARLIER_BAD   "That time is not valid"

/* ---- quick settings ---- */
#define HY_S_QS_TITLE      "Quick settings"
#define HY_S_QS_WIFI       "Wi-Fi"
#define HY_S_QS_AP         "Access point"
#define HY_S_QS_SOUND      "Sound"
#define HY_S_QS_CHIME_ON   "Chime on"
#define HY_S_QS_CHIME_OFF  "Chime off"
#define HY_S_QS_DND        "Do not disturb"
#define HY_S_ON            "On"
#define HY_S_OFF           "Off"
#define HY_S_QS_BRIGHTNESS "Brightness"
#define HY_S_QS_BATTERY    "Battery " HY_S_DASH "%"
#define HY_S_QS_BATTERY_PCT "Battery %s"            /* "78%" */
#define HY_S_QS_BATTERY_EST "Battery %s" HY_S_DOT "%s" /* "78%", "~2 d" */
#define HY_S_QS_SETTINGS   "Settings " HY_S_RSAQ
#define HY_S_QS_QR         "Web UI QR " HY_S_RSAQ

/* ---- settings ---- */
#define HY_S_SET_TITLE     "Settings"
#define HY_S_SET_ZONE      "Time zone"
#define HY_S_SET_DATETIME  "Date and time"
#define HY_S_SET_SOUND     "Sound"
#define HY_S_SET_VOLUME    "Volume %u%%"
#define HY_S_SET_DISPLAY   "Display"
#define HY_S_SET_BRIGHT    "Brightness %u%%"
#define HY_S_SET_WIFI      "Wi-Fi"
#define HY_S_SET_ABOUT     "About"
#define HY_S_SET_SET       "Set"
#define HY_S_SET_PLAY      "Play chime"
#define HY_S_ABOUT_VERSION "hygeia %s"
#define HY_S_ABOUT_SYNCED  "Clock synced %s ago via %s" /* duration, source */
#define HY_S_ABOUT_UNSYNCED "Clock not synced"
#define HY_S_ABOUT_STORAGE "Storage %s of %s"
#define HY_S_KB            "%u KB"
#define HY_S_MB            "%u.%u MB"
#define HY_S_ABOUT_MEDS    "Medicines %u active, %u archived"
#define HY_S_MONTHS        "Jan\nFeb\nMar\nApr\nMay\nJun\nJul\nAug\nSep\nOct\nNov\nDec" /* roller options */

/* ---- set the time ---- */
#define HY_S_TIME_TITLE    "Set the time"
#define HY_S_TIME_BODY     "Reminders are paused and logging is off until the time is set."
#define HY_S_TIME_BAD_DATE "That date does not exist"
#define HY_S_TIME_BAD_TIME "That time does not exist"
#define HY_S_TIME_TOO_EARLY "Date is before this firmware was built"
#define HY_S_TIME_ZONE_LINE "Time zone %s"          /* IANA name or POSIX string */

/* ---- idle clock ---- */
#define HY_S_IDLE_NEXT     "Next: %s at %s"         /* code, HH:MM */
#define HY_S_IDLE_NONE     "No doses scheduled"
#define HY_S_IDLE_SET_TIME HY_S_TIME_TITLE
#define HY_S_IDLE_STATUS   "%s" HY_S_DOT "Wi-Fi off"   /* battery text */

/* ---- power-off confirmation and the critical-battery countdown ---- */
#define HY_S_CANCEL        "Cancel"
#define HY_S_POWEROFF_TITLE  "Power off"
#define HY_S_POWEROFF_WARN   "Reminders will not fire while off"
#define HY_S_POWEROFF_SLIDE  "Slide to power off " HY_S_RSAQ
#define HY_S_POWEROFF_FAILED "Power off failed"
#define HY_S_CRIT_TITLE     "Battery critical"
#define HY_S_CRIT_LINE      "Powering off in %u s"     /* seconds left */
#define HY_S_CRIT_HINT      "Plug in USB to keep it on"

#endif /* HY_UI_STRINGS_H */
