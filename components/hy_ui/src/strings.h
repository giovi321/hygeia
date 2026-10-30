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
#define HY_S_DET_EDIT      "Edit"
#define HY_S_DET_ARCHIVE   "Archive"
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
#define HY_S_QS_BATTERY_LEFT "%s left"             /* "~2 d" */
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
#define HY_S_IDLE_DUE_ONE  "%s due"                 /* code of the open dose */
#define HY_S_IDLE_DUE_MORE "%s and %u more due"      /* code of the first open dose, count of the others */
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

/* ---- medicine form ---- */
#define HY_S_BACK          "Back"
#define HY_S_NEXT          "Next"
#define HY_S_DONE          "Done"
#define HY_S_MF_STEP       "Step %d of %d"          /* number, HY_MF_STEPS */
#define HY_S_MF_NAME       "Name"
#define HY_S_MF_NAME_HINT  "e.g. Vitamin D"
#define HY_S_MF_ERR_NAME   "Type a name first"
#define HY_S_MF_DOSE       "Dose (optional)"
#define HY_S_MF_DOSE_HINT  "e.g. 1 tab or 5 ml"
#define HY_S_MF_ERR_DOSE   "Use 16 characters or fewer"
#define HY_S_MF_CODE       "Short code"
#define HY_S_MF_CODE_HINT  "Shown on the tile"
#define HY_S_MF_CODE_CHANGE "Change"
#define HY_S_MF_ERR_DUP    "%s is already used by %s"   /* code, the other medicine's name */
#define HY_S_MF_ERR_CODE   "Pick 1 or 2 letters or digits"
#define HY_S_MF_COLOR      "Colour"
#define HY_S_MF_ERR_COLOR  "Pick a colour"
#define HY_S_MF_TYPE       "How is it taken?"
#define HY_S_MF_FIXED      "At set times"
#define HY_S_MF_FIXED_SUB  "Reminds you at the times you choose"
#define HY_S_MF_ASNEEDED   "When needed"
#define HY_S_MF_ASNEEDED_SUB "No reminders. Warns if taken too soon"
#define HY_S_MF_TIMES      "Times"
#define HY_S_MF_ERR_TIMES  "Add at least one time"
#define HY_S_MF_EVERY_DAY  "Every day"
#define HY_S_MF_DAY_NAMES  {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"}
#define HY_S_MF_WEB_SET    HY_S_DOT "set in the web UI"
#define HY_S_MF_REMOVE     "Remove"
#define HY_S_MF_ADD_TIME   "Add a time"
#define HY_S_MF_ADD_AT     "Add %s"                 /* HH:MM */
#define HY_S_MF_ERR_TIME_DUP "%s is already in the list" /* HH:MM */
#define HY_S_MF_INTERVAL   "Minimum time between doses"
#define HY_S_MF_NO_MIN     "No minimum"
#define HY_S_MF_MIN        "%d" HY_S_NB "min"
#define HY_S_MF_HOUR       "%d" HY_S_NB "h"
#define HY_S_MF_HOUR_MIN   "%d" HY_S_NB "h %d" HY_S_NB "min"
#define HY_S_MF_INTERVAL_SUB "If you log it sooner, the device asks you to confirm"
#define HY_S_MF_ERR_INTERVAL "Pick a time of 24 h or less"
#define HY_S_MF_REVIEW_NEW "Check and save"
#define HY_S_MF_REVIEW_EDIT "Edit %s"               /* name */
#define HY_S_MF_ROW_NAME   "Name"
#define HY_S_MF_ROW_DOSE   "Dose"
#define HY_S_MF_ROW_CODE   "Code and colour"
#define HY_S_MF_ROW_SCHED  "Schedule"
#define HY_S_MF_DOSE_NONE  "None"
#define HY_S_MF_AT_LEAST   HY_S_DOT "at least %s"   /* duration */
#define HY_S_MF_WEB_ONLY   "Days and reminder windows: web UI"
#define HY_S_MF_DUE_NOW    "%s today will be due right away" /* HH:MM */
#define HY_S_MF_SNOOZE_WARN "A snoozed dose will show again now"
#define HY_S_MF_SAVE       "Save"
#define HY_S_MF_DISCARD_TITLE "Discard changes?"
#define HY_S_MF_KEEP       "Keep editing"
#define HY_S_MF_DISCARD    "Discard"
#define HY_S_MF_ERR_FULL   "The list is full. Archive a medicine first"
#define HY_S_MF_ERR_GONE   "This medicine was archived meanwhile"
#define HY_S_MF_ERR_IO     "Could not save. Try again"
#define HY_S_MF_ERR_WEB    "Could not save. Ask for help in the web UI"
#define HY_S_MF_ADDED      "%s added"               /* code */
#define HY_S_MF_SAVED      "Saved"
#define HY_S_MF_FULL_ACTIVE "50 medicines is the limit. Archive one first"
#define HY_S_MF_FULL_STORED "No room for more medicines. Ask for help in the web UI"

/* ---- archive confirmation ---- */
#define HY_S_ARCH_TITLE    "Archive %s?"            /* name */
#define HY_S_ARCH_BODY     "It leaves the home screen and stops reminding. Its history is kept. Someone can bring it back in the web UI."
#define HY_S_ARCH_DUE      "A dose is due now. It will not be logged."
#define HY_S_ARCH_CONFIRM  "Archive"
#define HY_S_ARCH_DONE     "%s archived"            /* code */

#endif /* HY_UI_STRINGS_H */
