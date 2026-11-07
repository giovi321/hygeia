# Hygeia design

Status: draft for approval, 2026-10-28

## Intent

Hygeia is firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.16 that turns the board into a medicine reminder and dose log for one person. It sits on a desk on USB power and travels on its own battery. Logging a dose is one tap on a big letter tile. At scheduled times the screen shows a full-screen card and plays a chime. History is visible on the device and in a web UI served by the device. Medicines can be added, edited and archived on the device itself; the web UI edits every detail of the medicines and the log.

The device must work with no network at all. Wi-Fi, MQTT, Home Assistant and webhooks are add-ons that make it reachable, never dependencies for reminding or logging.

What makes it worth building instead of using a phone app: a dedicated object with a glanceable screen, no account, no cloud, data that stays on the device and the owner's LAN, and integration with an existing Home Assistant / n8n setup.

## Goals

1. One-tap logging from a scrollable grid of medicine tiles, each identified by a 1–2 character code and a colour; adding, editing and archiving a medicine on the device with no phone or network
2. Scheduled reminders on screen with chime, snooze and skip, while on USB or battery, with the screen asleep
3. Graphical history on the device: today overview, 7-day grid, per-medicine detail
4. A web UI that can do everything the screen can do, plus full editing of medicines and log entries, export and import
5. Wi-Fi client mode, access-point mode and off, switchable from the screen and the web UI
6. Clock from the onboard RTC, synced from the browser or NTP, with time zones that follow the traveller
7. MQTT with optional Home Assistant discovery, and webhooks
8. Battery level shown as a percentage, battery events published, power on/off with the onboard button
9. A repository that can be made public at any time: no personal data, docs site ready for GitHub Pages, README in the style of Asclepius
10. Cabinet open: mounted on a cabinet door, the device can chime and/or light the screen when the cabinet is opened and a dose is overdue or due soon

## Non-goals

- Multiple users or patients
- Cloud sync, accounts, phone app
- Medication database, interaction checks, dosage advice
- Voice control (the board has microphones; not used in v1)
- BLE (possible later for time sync, see okhsunrog/claudial)
- Multiple languages (English only in v1; strings kept in one place so translation is possible later)

## Anti-goals

- Not a medical device. The README, docs and LICENSE say so. It must never be presented as a safeguard for medication that must not be missed
- A reminder must never depend on Wi-Fi, MQTT, a server or the web UI being reachable
- An accidental touch must never log a dose silently: every log is visible, and undoable for 5 s on the device and editable in the web UI
- No personal data in the repository: real medicine names, network names, broker addresses, tokens, email addresses or the maintainer's surname

## Hardware

Board: Waveshare ESP32-S3-Touch-AMOLED-2.16. Facts from https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16 and https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16, read 2026-10-28.

| Part | Detail |
|---|---|
| MCU | ESP32-S3R8, 8 MB octal PSRAM, 16 MB flash |
| Display | 2.16" 480×480 AMOLED, CO5300 over QSPI (CS 12, SCLK 38, D0–D3 4/5/6/7, RST 39) |
| Touch | CST9220, I2C, INT 11, RST 40 |
| I2C bus | SDA 15, SCL 14: touch, AXP2101, QMI8658, PCF85063, ES8311, ES7210 |
| PMU | AXP2101, MX1.25 Li-ion connector, charge and discharge, PWR key |
| RTC | PCF85063ATL, INT on GPIO 13 |
| Audio out | ES8311 codec, I2S MCLK 42, BCLK 9, LRCK 45, DOUT 8, amp enable GPIO 46 |
| Buttons | BOOT GPIO 0, user key GPIO 18, PWR via AXP2101 |
| Other | QMI8658 IMU (INT1 17, INT2 21), ES7210 dual mic ADC, microSD (SPI) |

To verify on the first hardware session (unconfirmed in the sources):

- whether the RTC keeps time when the Li-ion is empty and USB is unplugged (no backup cell is documented)
- that the ES8311 speaker path plays a chime (a community project marks it unverified)
- touch orientation relative to the panel
- which PWR key events the AXP2101 reports (short press, long press) and their timings
- battery life on light sleep with the screen off and Wi-Fi off
- panel rotation through MADCTL and the matching touch transform in all four orientations

## Stack

Decided:

- ESP-IDF v6.0.2 is the build of record, locally and in CI; direct component dependencies are pinned to exact versions and `dependencies.lock` is committed (Waveshare CI-tests v5.5.5 and v6.0.2). C, with C++ only where a component needs it
- LVGL 9.x through the managed component and `esp_lvgl_port`
- Board support from the registry component `waveshare/esp32_s3_touch_amoled_2_16` (v2.0.1 at time of writing). If its display init is unsuitable, fall back to the approach in thejimoneil/ESP32-Tetris: custom CO5300 init for the full 480×480, `esp_lcd_touch_cst9220`, double-buffered DMA, rounder callback forcing even x1/y1 and odd x2/y2, software rotation
- UI written by hand in LVGL C on one shared theme; no generated-code tools
- Web UI in plain JavaScript and CSS with no framework, concatenated and gzipped by a Python script at build time, embedded in the app image
- Storage on LittleFS; settings in NVS

Reference projects (study, do not copy wholesale, check their licenses before reusing code):

- thejimoneil/ESP32-Tetris: display, touch, ES8311 sound, gestures, NVS
- waveshareteam/ESP32-S3-Touch-AMOLED-2.16: AXP2101, LVGL v9 demo, PCF85063 via SensorLib
- maxiumlee/ESP32-S3-LVGL-Clock: AP web config, alarms, sleep and wake on this board

## Architecture

All behaviour lives in a core that knows nothing about screens or networks. Every input surface calls the same actions; every output surface listens to the same events.

```
 inputs                      core                          outputs
 ───────                     ────                          ───────
 touch UI ─┐            ┌─ actions ─┐                 ┌─▶ UI refresh
 web REST ─┼──────────▶ │ store     │ ── event bus ───┼─▶ web UI (WebSocket)
 MQTT cmd ─┘            │ scheduler │                 ├─▶ MQTT state + events
                        │ clock     │                 ├─▶ webhooks
 RTC alarm, PMU IRQ ──▶ │ power     │                 └─▶ chime, screen wake
                        └───────────┘
```

Components (one ESP-IDF component each; the integration modules share one component and its delivery task):

| Component | Responsibility |
|---|---|
| `board` | display, touch, AXP2101, PCF85063, ES8311, buttons; hides pin and chip details |
| `clock` | time source of truth: RTC read/write, NTP, browser sync, time zone, validity flag |
| `store` | medicines, log, settings; persistence, migrations, export/import |
| `scheduler` | computes occurrences, due/late/missed state, snooze, next wake time |
| `actions` | the only API that changes state: log dose, undo, skip, snooze, edit medicine, edit log entry, set Wi-Fi mode, set time, etc. |
| `events` | in-process publish/subscribe of state changes |
| `power` | battery state, charging, sleep, wake sources, power-off |
| `net` | Wi-Fi client / AP / off, known networks, mDNS, captive DNS in AP mode |
| `web` | HTTP server, REST API, WebSocket, static web UI, browser pairing |
| `mqtt` | publish state and events, receive commands, Home Assistant discovery; a source file of the component `hy_integ`, whose delivery task drives MQTT and webhooks |
| `webhook` | HTTP POST of events with retry; a source file of the component `hy_integ` |
| `outbox` | persistent queue of events not yet delivered to MQTT or webhooks; a source file of `store` |
| `ui` | LVGL screens, theme, gestures, animations |
| `audio` | chime playback |

Feature parity rule: every action available on the screen has a REST endpoint and a web UI control. The parity table in the docs is generated from the `actions` list so drift shows up in review.

## Data model

### Medicine

| Field | Notes |
|---|---|
| `id` | stable integer, never reused |
| `code` | 1–2 characters A–Z or 0–9, shown on the tile, unique among active medicines. Display only: MQTT topics, HA ids and the API use `id` |
| `name` | free text, up to 32 characters |
| `dose` | free text, at most 16 bytes, may be empty, e.g. "1 tab", "5 ml" |
| `color` | one of a fixed palette of 10 colours that read well on black and are distinct for common colour-vision deficiencies |
| `schedule.type` | `fixed` or `as_needed` |
| `schedule.times` | list of local times (HH:MM) for `fixed` |
| `schedule.weekdays` | bitmask, default all days |
| `schedule.min_interval_min` | for `as_needed`: warn when logging sooner |
| `late_after_min` | default 30 |
| `missed_after_min` | default 120 |
| `pill_count` | optional: enabled, remaining, alert threshold; decremented on each taken log |
| `active_from`, `active_until` | optional dates for a course of treatment |
| `archived` | hidden from the device, kept for history |
| `order` | tile position |

The grid scrolls; the web UI handles ordering. A soft limit of 50 active medicines is enforced to bound memory and screen build time; archived medicines do not count toward it. The store holds at most 100 medicines including archived ones. Builder may raise both after measuring.

### Log entry

| Field | Notes |
|---|---|
| `id` | monotonic |
| `ts` | UTC epoch seconds when the dose was taken (or skipped, or declared missed) |
| `tz` | the time zone in force at that moment, so history shows the local time it happened |
| `med_id` | medicine id |
| `status` | `taken`, `skipped`, `missed` |
| `slot` | the scheduled occurrence it satisfies (UTC epoch), empty for as-needed |
| `delta_min` | minutes late or early relative to `slot` |
| `note` | optional text |
| `source` | `screen`, `web`, `mqtt`, `auto` (for `missed`) |
| `edited` | set when changed after creation |

Snoozes are not log entries; they are scheduler state, and are published as events.

### Persistence

- Medicines: one JSON file, written atomically (write temp, fsync, rename)
- Log: append-only JSON Lines, one file per month. Edits and deletes rewrite the month file atomically. Each line about 150 bytes; 10 doses a day is about 0.5 MB a year, so the storage partition holds many years. Retention default: keep everything until the partition is 80% full, then drop the oldest month after warning on screen and in the web UI. Minimum guaranteed retention: 1 year
- Settings: NVS
- Cabinet open template (5 taught takes, at most 12076 bytes; a file with 3 or 4 takes from an earlier version still loads): LittleFS
- Secrets (Wi-Fi passwords, MQTT password, webhook secrets, paired-browser session tokens): NVS (MQTT password and webhook secrets in namespace `integ`), never returned by the API and never included in an export unless the user ticks "Include passwords"
- Integration settings (MQTT, Home Assistant discovery, webhooks): NVS namespace `integ`
- Export: CSV (log only) and JSON (medicines, log, settings including the `mqtt` and `webhooks` members; their password and secrets only with "Include passwords"). An import applies those members by the settings PUT rules. Import: JSON, with a preview of what changes and a choice of merge or replace
- Backup/restore in the web UI uses the JSON export

## Scheduling

- Schedules are in local time of the current time zone. Changing the time zone moves future occurrences to the same wall-clock times in the new zone (08:00 stays 08:00). Past log entries keep their recorded time zone
- Clock jumps from a zone change (decided): flying east, an occurrence whose local time falls inside the skipped hours becomes due immediately on arrival, never missed. Flying west, an occurrence already satisfied for that medicine on that local date and wall-clock time is not raised again when the same local time comes round a second time. The same rules apply to DST transitions
- Daylight saving handled with POSIX TZ rules. The firmware carries a compact IANA→POSIX table (generated at build time, about 20–30 KB) so the zone can be changed on the device with no network; the web UI sends the browser's IANA zone and the device looks it up in the same table
- An occurrence becomes `due` at its time, `late` after `late_after_min`, `missed` after `missed_after_min`. When it becomes missed, a `missed` log entry with source `auto` is written and an event sent
- Dose window: from `early_window_min` before the occurrence (per medicine, default 60) until the next occurrence of the same medicine. A `taken` log in that window satisfies the earliest unsatisfied occurrence; `delta_min` is negative for early doses. A log outside any window is a free log
- Taken after missed: if a dose is logged after its occurrence was marked missed, but still before the next occurrence, the `missed` entry is converted to `taken` (with its real time and lateness) and a `taken` event is sent. The original missed event is not retracted
- Untracked periods: while the device is off, the battery is flat, or the clock is invalid, occurrences cannot be observed. On return, occurrences in that gap are not marked missed. The store records the gap as an `untracked` interval; history draws it grey, adherence excludes it, and a single `untracked` event (start, end) is sent instead of one per occurrence (on the device's event bus and in the web UI; MQTT and webhooks do not send it). Occurrences still within their window when the device returns become due normally. Interim behaviour, until untracked intervals are persisted: slots that fall while the device was off show as untracked and are never marked missed.
- Snooze defaults to 15 minutes, configurable; snoozing does not extend the missed deadline
- As-needed: logging sooner than `min_interval_min` after the previous dose shows a warning with the time since the last dose; the user can still confirm
- The scheduler exposes the next wake time (next occurrence or deadline) to `power`, which sets the RTC alarm

## Clock

- The PCF85063 is the time source. On boot, read it; if its oscillator-stop flag is set or the time is before the firmware build date, the clock is invalid
- Invalid clock: the device shows a "Set the time" screen with a QR code to the web UI. Reminders are paused and logging is disabled until the time is set, because a log with a wrong time is worse than no log
- Sync sources, in priority: browser sync (manual button, and automatically when the web UI is opened and differs by more than 2 s), NTP when in client mode (on connect and every 6 h), manual set on the device
- The web UI never changes the time zone on its own: when the browser's zone differs from the device's, it offers the change with one tap
- Each sync writes the RTC and records the source and time; the web UI shows "synced X ago via Y"

## Power

- On USB: screen dims to the idle clock after 30 s, never turns off unless the user sets it
- On battery: dims after 30 s, screen off after 2 min, CPU in light sleep. Wake sources: touch interrupt, RTC alarm (next due occurrence), AXP2101 interrupt (PWR key, USB plugged), user key, motion sensor interrupt (Cabinet open only). A touch wakes the CPU; the screen turns on only for a double tap unless Wake on single tap is on. The setting Double tap to wake (default on) off holds the touch controller in reset while the screen is off in light sleep, so a touch wakes nothing (about 3.0 mA instead of 7.3 mA with the screen off, USB meter 2026-11-05); then only PWR, the user key, the RTC alarm, the motion sensor (Cabinet open) and USB wake, and Wake on single tap does not apply (its row is hidden). The dimmed and lit screens take touches as usual
- A running Cabinet open teach, check or test keeps the screen on, whatever started it (device, web UI): no dim, no off, on USB and on battery; the normal timers resume when it ends or the view closes. A screen turned off ends the teach, check or test
- The motion sensor is read while the screen is on or dimmed. While the screen is off it is powered down, except while Cabinet open is live (on, the chime or the light switched on, and with Taught movement a movement taught): then it watches for movement in its low-power wake-on-motion mode and its interrupt is a wake source. If the sensor stops answering, the driver retries the command and resets the sensor; if that fails Cabinet open pauses itself and is tried again at the next screen wake (at most four times an hour) or at once on a settings change
- On battery the saved Wi-Fi mode does not start by itself. Turning on Wi-Fi or the access point from the screen or the web UI starts it; it stops after 10 minutes with no web request and no touch. Plugging in USB starts the saved mode. A setting keeps Wi-Fi on while on battery. While the radio is on the CPU does not enter light sleep. Events produced while offline go to the outbox. MQTT and webhooks use the radio only while it is on for another reason. They never start it, never keep it on and never extend the battery idle time; an MQTT command that logs, skips or snoozes counts as activity, like a web request. While the radio is off the delivery task sleeps and wakes nothing. The live event `{"ev":"integ"}` does not make an open web page fetch again, so it does not count as activity either, and the status read that the open Settings page makes (`GET /api/integrations`) does not restart the 10 minute idle timer. They deliver only while the device is connected to a network as a client, not in access point mode. On battery Home Assistant therefore shows the device as unavailable most of the time; that is expected
- PWR key: short press toggles the screen; long press (about 2 s) opens a power-off confirmation slider that warns "Reminders will not fire while off" if any occurrence is due in the next 24 h; confirming powers off through the AXP2101. Pressing PWR while off turns the device on (AXP2101 hardware behaviour)
- The device turns on only with the PWR key (maintainer requirement, 2026-10-28). Plugging in USB or connecting the battery must not leave it running. Early in boot, before the display is initialised, firmware reads the AXP2101 power-on source; if the source is USB insertion or battery insertion rather than the PWR key, it powers off through the AXP2101 at once. The battery still charges while off. A software restart (OTA, crash, watchdog) is not a power-on event and keeps running. The exact register and whether the cold-boot source is reported reliably are verified on the board
- Battery shown as a percentage only in the top bar; red below 15%, USB power indicated by a small bolt next to the number. Low battery (15%) and critical (5%) produce events
- Battery cell and battery-life target: not decided yet. The maintainer will check the cell they have (capacity, MX1.25 connector, polarity) before the power work starts. Until then v1 implements the light-sleep design above, and the power milestone ends with a measured discharge curve; the target and any deeper sleep work are set from that measurement
- Battery estimate ("~2 d") in quick settings, from a moving average of discharge rate; hidden until enough data exists

## Screen UI

Mockups: `design/mockups/screens.html`, `design/mockups/home-layout-options.html` (option A chosen), rendered PNGs in `docs/public/assets/mockups/`. Web UI and device settings: `design/mockups/webui.html`.

Visual rules:

- Pure black background; surfaces #121216 and #1A1A20; text #EDEDF0, secondary #9A9AA4 (meets WCAG AA on the surfaces)
- Each medicine has its own colour; the ring around its code shows elapsed time from the last dose to the next one. Due = full ring plus a 3 px outline in the medicine colour
- Top bar 56 px: time 30 px bold on the left; on the right Wi-Fi arcs (client, with signal), "AP" badge (access point) or crossed arcs (off), then battery percentage. No battery icon
- No bottom menu. Page dots show the position in the horizontal pager
- Font: Inter (SIL OFL), converted with `lv_font_conv` at the sizes used; one place defines the type scale
- Animations under 250 ms, except the boot splash (about 1 s: capsule drops into the ring gap, ring draws around it)
- Burn-in: idle clock drifts a few pixels every minute; static elements dim after 30 s

Gestures:

| Gesture | Effect |
|---|---|
| Swipe left / right | pager: Home ⇄ Overview ⇄ History |
| Pull down from near the top edge | Quick settings, from any screen except the due card |
| Swipe up from near the bottom edge | back to Home, from any screen; with unsaved changes on a medicine form it stays on the form |
| Swipe right from near the left edge | close a detail page; on the medicine form, step back one page or ask "Discard changes?" |
| Vertical swipe in the grid | scroll medicines |
| Tap tile | log as taken, show undo toast (5 s countdown) |
| Long press tile | medicine detail |
| Tap the "+" tile | add a medicine |
| Slide the knob right (due card) | log as taken |
| Swipe left on due card | snooze |
| Swipe up on due card | next due entry |
| Swipe down on due card | previous due entry |
| Tap "Skip" (due card) | log as skipped |
| Double tap while dimmed or off | wake: two taps within 300 ms and 40 px, not passed to the screen underneath; a single tap wakes only when Wake on single tap is on (default off). With Double tap to wake off (default on) a touch does not wake the screen while it is off |
| Turn the device | the screen follows in all four orientations after about 1 s in the new position, unless Rotation lock is on |
| Tap anywhere outside the Undo button while the logged or skipped toast shows | closes the toast; the entry stays logged and the tap does nothing else (no tile log, no navigation); the shake no longer undoes it (`hy_toast_stroke`). Swipes, long presses and edge gestures while it shows are consumed and leave it open, as is a stroke that only wakes the screen or lands on the power-off or pairing overlay |
| Tap anywhere while an info or ok toast shows (`toast_show_info`, `toast_show_ok`) | closes the toast; the tap does nothing else, by the same rule as the Undo toast (`hy_toast_stroke`, `on_undo` false). A clear scrim catches the tap; no scrim and no consumption while the power-off or pairing overlay is up. A toast that opened under the overlay gets the scrim when the overlay closes. A held low-battery toast follows the close |
| Shake the device while the Undo toast shows | undo, the same as tapping Undo |

Swipe directions and edges are those of the screen as the person sees it: they turn with the screen.

Edge zones: two zones, because with the device in a corner or a case the finger cannot always start off the glass. The hard zone (40 px, 16 px at the bottom over scrolling content) is where no widget can take a stroke, so an edge gesture from the border always works; sliders still take it. The edge gesture zone (provisional 80 px, set from the board measurement) is where hy_gesture_classify turns a swipe toward the inside into EDGE_RIGHT, EDGE_DOWN or EDGE_UP when no widget took it. Lists, the keyboard, rollers and text fields consume their strokes from 40 px inward, so over them the edge gesture needs the hard zone, with one exception: a stroke that starts in the band and moves inward (down in the top band, up in the bottom band) is not consumed by a list that is already at that end (`hy_edge_stroke_passes`, decided at the list's scroll start in `ui_input_consume`), so the edge gesture works over a list at its top or bottom. The list's elastic over-pull is switched off for that stroke and restored at the release, so it does not stretch while the view navigates. A list that can still scroll that way keeps the stroke. A tap in either zone stays a tap. A stroke that stopped a gliding list is never an edge gesture. Debug builds have `touch trace on|off` for the measurement (design/hardware-notes.md "Gestures").

Screens: Home (grid of rings, "+" tile opens the add-medicine form), Quick settings (Wi-Fi client / AP / off, sound, do-not-disturb, brightness, battery estimate, Settings, web UI QR), Overview (today count ring and 24 h timeline), History (7-day grid per medicine, double tap for 30 days), Medicine detail (name, dose, schedule, pill count, recent log, log with note, log at an earlier time, 30-day chart, Edit and Archive at the bottom), Add or edit medicine (name, dose, code, colour, schedule), Archive confirm, Dose due (full screen), Logged toast, Idle (dim clock and next dose), Settings (Wi-Fi, time zone, sound, display with rotation lock and Wake on single tap, cabinet open, about), AP on (Wi-Fi QR to join, then web UI QR), Set the time, Power off confirm, Boot splash.

Several due at once: the due card shows the first, with "+2 more"; a swipe up steps to the next entry and a swipe down to the previous, wrapping at either end. The card stays until the entry is taken, skipped or snoozed; stepping through only changes which entry is shown.

History: a dose taken late gets an amber dot, so it differs from today's partial cell. The device cannot edit log entries, apart from undo within the 5 s toast; edits and deletes happen only in the web UI.

Text entry: the on-screen keyboard is used for notes (log with note), medicine names and doses. Codes are picked from a grid of letters and digits. Nothing else on the device takes typed input. The keyboard has a one-shot shift key that capitalises the next letter only. The first letter of a medicine name is capitalised automatically; a dose is not.

Do-not-disturb and quiet hours: the card still appears, the chime is silent. Quiet hours are a web UI setting; the device has only the do-not-disturb toggle. For Cabinet open they silence the chime only; its light still works.

## On-device medicine management

The maintainer's intent, in their words:

> the logic of adding this on-device is that a person not super tech friendly does not want to connect the phone via wifi but can still do the basic stuff on device. In case he needs to do more advanced stuff he can ask for help to someone but at least for the basic stuff he must be autonomous to do on device

What the device edits: code, name, dose, colour, schedule type, and the daily times (`fixed`) or the minimum interval (`as_needed`). Dose is optional free text of at most 16 bytes. Weekdays, the early, late and missed windows, active dates, pill count and order are set in the web UI. When the device edits a medicine it keeps their current values. A new medicine gets the defaults: every day, windows of 60 / 30 / 120 min, no pill count, no active dates, placed after the last tile.

Times page of the medicine form: one row per time with a Remove button (hidden for the last time) and an "Add a time" button. "Add a time" opens an hour and minute roller page (minutes in 5-minute steps) with the Next button "Add HH:MM". Tapping a time row opens the same page titled "Change time", set to that time, with the Next button "Change to HH:MM"; the Remove button does not open it. Committing replaces that time and keeps the list ascending; the same time is a no-op and a time already in the list shows "HH:MM is already in the list" and changes nothing. A time that is not on a 5-minute step (set in the web UI) starts on the nearest lower step. The quick settings closing keeps the page and the index being changed.

Archiving: the device archives a medicine; unarchiving is in the web UI. Archiving asks for confirmation, writes no log entry, removes any due card for that medicine and stops its reminders. Its history stays.

Design rules:

- One decision per screen
- Large targets
- Errors in plain language, on the screen where they can be fixed
- "Discard changes?" before unsaved changes are lost
- Nothing is written without the Save button, or the Archive button on the archive confirmation

A time added or changed so that today's occurrence is already inside its dose window becomes due right away, with the chime. The review screen says so before Save.

## Notifications

- At an occurrence: wake the screen, show the due card, play the chime (default 3 short tones, repeated every 2 min, 5 plays in total per sequence, while unacknowledged, volume from settings), send `due` event
- If the speaker path does not work on hardware, the fallback is a pulsing screen and a gentle brightness flash; the design does not depend on sound
- At `late_after`: the tile shows "late" and the card stays; at `missed_after`: `missed` log and event, card dismissed, tile shows missed until the next occurrence
- Cabinet open: when the device detects the cabinet opening (trigger: any movement, or a movement taught by doing it 5 times) and a dose is overdue or due within a set window (0 to 120 min in 5 min steps, 0 meaning overdue only, default 15; capped by the medicine's early window), it plays one chime and/or lights the screen, per two switches. The chime follows sound, do-not-disturb and quiet hours; the light does not. At most one reaction every 2 minutes. While the screen is on (not dimmed, not off) a movement does nothing: no chime, no reaction counted, and the 2-minute pause does not start. When the chime plays (a chime silenced by Sound off, do not disturb or quiet hours, or lost because the speaker does not work, leaves no note) and the screen does not light (Light off, screen dimmed or off), the device remembers the reason and shows it at the next wake of the screen by any cause: the due card when a dose is open, otherwise Home with the note below for 5 seconds; it is dropped when shown, when the dose is logged or becomes open, or after the dose time. Otherwise nothing happens. An open dose counts even while it is snoozed. The light shows the due card when a dose is open; when the dose is only due soon it shows Home with a 5-second note "Next: <medicine> at <time>" (for example "Next: Vitamin D at 08:00"). The feature is off until turned on; its defaults are Any movement, chime on, light on, sensitivity 3 of 5, window 15 min. Every Cabinet open setting is available on the device and in the web UI. It never logs, skips, snoozes or undoes a dose
- Cabinet open teaching: Settings › Cabinet open › Teach the movement takes 5 takes of any movement (the wording never assumes a door). After Start the device waits 5 s with the stream ignored ("Get ready": the person puts the device back after the tap), then for each take waits for 1 s of stillness (up to 20 s, then teaching stops), plays a high tone (GO) and captures the movement; after a counted take, a too small or too long capture or a take that looks different it ignores the stream for 5 s, then waits for stillness again for the take in question. GO is given after the stillness; if no movement follows GO within 60 s, teaching stops. After the fifth take the five are compared: a take that looks different is asked for again (at most twice, then kept as it is), otherwise the template is saved. Every result and every redo is a tone as well as a status line ("Get ready" and "Hold still" have no tone), at the set volume whatever Sound and do-not-disturb say, and tones queue (4 deep) so two in a row both play: GO one tone of 1319 Hz for 250 ms; TAKE two tones of 880 Hz for 120 ms with a 120 ms gap (takes 1 to 4); REDO one tone of 440 Hz for 400 ms (too small, too long, looked different); DONE rising 659, 880 and 1319 Hz for 120 ms each with 40 ms gaps (saved, check recognised); FAIL falling 659 and 440 Hz for 250 ms each with a 60 ms gap (teaching stopped, check missed or saw nothing). The last take, and a take asked for again after a take that looked different, play no TAKE tone: DONE, REDO or FAIL follows within the build time. A capture that could not start is a redo, with the REDO tone. If the runner refuses Start or Teach again (for example after a sensor fault) the toast "Teaching cannot start now" shows and the view stays as it was
- Cabinet open completion and check: after the save the teach view shows "Movement saved" and "Test it now?" with Test and Done. Test runs a check: the same lead-in, stillness and GO as a take, one capture, matched against the saved template at the set sensitivity. The result is "Movement recognised" with Done, or "Not recognised. Teach the movement again?" (a movement that did not match) or "No movement seen. Teach the movement again?" (none within 60 s), each with Teach again (teaching restarts at once) and Keep. The check starts its capture at the trigger in use (the template's), as a live capture does, so a recognised movement is one the device would act on; a movement below it ends as "No movement seen". A check that is not still within 20 s, or is stopped from outside, plays the FAIL tones and shows "Not still, test stopped" or "Test stopped" on the "Movement saved" view. A check that cannot start shows "The test cannot start now" on the "Movement saved" view. Done and Keep close the view. Leaving the view stops a running teach or check. The runner reports a check as `checking` in GET /api/cabinet and holds the screen on
- Cabinet open test from the device: Settings › Cabinet open › Test, when Trigger is Taught movement (and a movement is taught; without one a "teach first" toast shows), opens the teach view and runs the same guided check of the taught movement as Test on the "Movement saved" view (same lead-in, tones, status lines, result screens and buttons), so the test can be heard with the cabinet closed. Nothing was saved just now, so the view never says "Movement saved" or "Test it now?": a refused start shows the toast "The test cannot start now" and opens no view, and "Not still, test stopped" or "Test stopped" show under the heading "Test the movement" with Done only. When Trigger is Any movement, also with a movement still taught, it opens the counting test view of any movement at the set sensitivity instead, and each movement seen plays the DONE tones (a Taught-mode match plays DONE, another movement FAIL). The web UI test (`POST /api/cabinet/test`) is unchanged: counters and distances, no tones
- Cabinet open trigger switching: saving a taught movement sets the trigger to Taught movement when it was Any movement (owner ruling); forgetting the taught movement, on the device or in the web UI, sets a Taught movement trigger back to Any movement. The template file keeps version 1 and its takes field (3 to 5); a model matches against the takes in use

## Network

- Modes: client, access point, off. Selected on the device (quick settings) and in the web UI; persisted
- Networks are added only in the web UI (decided: no on-device keyboard for network credentials, and no scan). On the device you pick client, AP or off, and choose among saved networks. On a trip, a new network is added by turning on AP mode and opening the web UI from a phone
- Client mode: list of known networks, tries them in order of last success; on failure for 2 min, stays in client mode retrying with backoff (never silently switches to AP). The device shows a hint to start AP mode (quick settings tile and Settings › Wi-Fi)
- AP mode: SSID `hygeia-XXXX` (last 4 hex of the MAC), WPA2 with a random 10-character password generated on first boot, changeable. Device shows a Wi-Fi QR code that includes the password, then a QR to `http://192.168.4.1`. Captive-portal DNS redirects to the web UI
- mDNS name `hygeia.local` in client mode (configurable)

## Web UI

Mockups: `design/mockups/webui.html`.

- Pages: Dashboard, Medicines (list, add, edit, reorder, archive), History (30-day grid, log table with edit, delete, add, export CSV/JSON, import), Schedule (week view of all occurrences), Settings (Wi-Fi, MQTT, Home Assistant discovery, webhooks, clock, screen and sound, quiet hours, paired browsers, backup/restore, firmware update), Cabinet open (trigger, switches, sensitivity, window, teach status, test counter, sample download)
- The MQTT, Home Assistant discovery and webhooks panels show the connection state, each webhook's last result and the outbox counters (pending, dropped when full, expired, rejected)
- Responsive: sidebar on desktop, top menu on a phone
- Live updates over WebSocket from the event bus
- No open access. A browser is paired once with a 6-digit code shown on the device screen; the device then keeps a session token for that browser (at most 4 browsers, the oldest is forgotten). Paired browsers can be forgotten on the device and in the web UI. The web UI is plain HTTP on the local network by design (a design choice, not a missing feature), protected by pairing, the `X-Hygeia` header and the host allow-list; the docs say to keep it on a trusted LAN. The GitHub update download is outbound and uses HTTPS
- Firmware update, two ways. Upload a `hygeia.bin` file. Or press "Check for update" (web UI, or Settings › About on the device): the device asks GitHub for the latest stable release of the project and, after a confirmation, downloads `hygeia.bin` itself over HTTPS, checks it against the release's `SHA256SUMS` and installs it. Both ways write the inactive slot, verify it, restart, and roll back on a failed boot. Nothing contacts GitHub until the button is pressed; there are no background checks. Checking needs Wi-Fi with internet; installing needs USB or a battery charge of 30 % or more

API: JSON, paths as listed, no version in the path. `GET /api/info` and `GET /api/state` carry `"api": 1`; adding fields does not change it, removing or changing one does. Clients ignore unknown fields. Errors are `{"error": "<code>", "field": "<name or null>", "message": "<text>"}`

REST API (JSON):

| Method and path | Action |
|---|---|
| `GET /api/info` | device name, firmware version, API version (open, no pairing needed) |
| `POST /api/pair/start` | show a pairing code on the device screen (open) |
| `POST /api/pair` | pair this browser with the code shown on the device (open) |
| `GET/DELETE /api/browsers` | list or forget paired browsers |
| `GET /api/state` | everything the dashboard needs |
| `GET/POST /api/medicines`, `GET/PUT /api/medicines/{id}` | medicines |
| `DELETE /api/medicines/{id}` | archived medicines only; deletes the medicine and its log entries after a confirmation that shows the count |
| `POST /api/medicines/{id}/archive`, `/unarchive` | archive or restore a medicine |
| `PUT /api/medicines/order` | reorder medicines |
| `POST /api/medicines/{id}/pills` | set (`{"left":n}`) or add to (`{"add":n}`) the pill count |
| `POST /api/medicines/{id}/log` | log taken, optional `ts`, `note` |
| `POST /api/medicines/{id}/skip`, `/snooze` | due-card actions |
| `GET /api/log?from&to&med`, `POST /api/log`, `PUT/DELETE /api/log/{id}` | log |
| `POST /api/log/{id}/undo` | undo within 5 s (also used by the device toast) |
| `GET /api/history?days=7\|30` | per-medicine daily grid for 7 or 30 days |
| `GET /api/schedule?from&to` | all occurrences in a time range |
| `GET /api/export.csv`, `GET /api/export.json`, `POST /api/import` | export and import |
| `GET/PUT /api/settings` | settings (secrets write-only), including the Cabinet open fields and `double_tap_wake`, and the `mqtt` and `webhooks` members (MQTT password and webhook secrets write-only) |
| `POST /api/time` | set time and zone from the browser |
| `GET /api/wifi` | mode, status, saved networks, AP settings (no passwords) |
| `POST /api/wifi/mode` | client, ap, off |
| `POST/DELETE /api/wifi/networks` | add or remove a saved network |
| `PUT /api/wifi/ap` | access point password and host name (the SSID is fixed) |
| `GET /api/integrations` | MQTT connection state, last result of each webhook, outbox counters |
| `POST /api/webhook/test` | send a test event to one webhook, body `{"index":n}`; the result shows in `GET /api/integrations` |
| `POST /api/chime/test` | play the chime |
| `POST /api/power/off` | power off the device |
| `POST /api/ota` | firmware upload |
| `GET /api/update` | state of the GitHub update (check result, progress, last error) |
| `POST /api/update/check` | ask GitHub for the latest stable release |
| `POST /api/update/install` | download and install the checked release, body `{"version":"vX.Y.Z"}` |
| `GET /api/cabinet` | Cabinet open state: teach status, test counter |
| `POST /api/cabinet/test` | start or stop test mode, body `{"on":true}` or `{"on":false}` |
| `GET /api/cabinet/samples.csv` | download the taught takes and the recent test and live captures as CSV |
| `DELETE /api/cabinet/teach` | forget the taught movement |
| `GET /ws` | WebSocket event stream |

## MQTT

- Settings: on switch, broker URL (`mqtt://`, port 1883 by default, or `mqtts://`, 8883), username, password (write-only), base topic (default `hygeia`), Home Assistant discovery switch and discovery prefix (default `homeassistant`). Client id `hygeia-XXXX` (the last 4 hex digits of the MAC, as in the access point name), fixed. Topics below are written with the base `<base>`
- Protocol: MQTT 3.1.1 over TCP or TLS only; no WebSocket transports and no MQTT 5, to save flash. Keepalive 60 s. Host names ending in `.local` are resolved through mDNS queries
- TLS: `mqtts://` verifies the broker against the ESP-IDF certificate bundle (common set) only. Brokers with a self-signed or private-CA certificate are not supported in this version, and there is no switch that skips verification. Certificate dates are not checked, as for the GitHub update
- Availability on `<base>/status`: `online` retained, QoS 1, published on connect; `offline` retained, QoS 1, published before every deliberate stop, because a clean disconnect does not fire the last will; last will `offline` retained for an unclean loss. With the 60 s keepalive Home Assistant sees a lost device within about 90 s. When the radio turns off on battery the device does not publish `offline` first: the broker's last will marks it offline after about 1.5 keepalives (about 90 s)
- State, retained, QoS 1, published when its content changes:
  - `<base>/med/<id>/state` per active medicine: `{"id","code","name","status","due","last_taken","next_due","pills_left"}` in that order. `status` is the tile kind of `GET /api/state` (`as_needed`, `no_dose`, `since`, `due`, `late`, `snoozed`, `missed`). `due` is true for due, late and snoozed (an open dose that can still be taken) and false for missed. `last_taken` and `next_due` are UTC ISO 8601 strings such as `2025-11-06T07:02:00Z`, or null, because Home Assistant's timestamp sensors need that form. `pills_left` is a number, or null when pill counting is off
  - `<base>/device`: `{"battery","charging","usb","wifi_mode","rssi","clock_source","clock_valid","adherence_30d","fw"}`; `battery`, `rssi`, `clock_source` and `adherence_30d` are null when unknown
- Events on `<base>/event`: QoS 1, not retained, the same payload as webhooks (timestamps in epoch seconds, as in the REST API), for the nine event types listed under Webhooks. `log_edited`, `meds_changed` and `untracked` are not sent as events; they only refresh the state topics
- Commands on `<base>/med/<id>/log`, `<base>/med/<id>/skip` and `<base>/med/<id>/snooze`, subscribed with QoS 1. The medicine must exist and be active. Retained command messages are ignored. `log` accepts any payload (`PRESS` from the Home Assistant button); a JSON object may add `ts` (the web's window: not before the firmware build date and not more than 60 s ahead), `note` (at most 63 bytes) and `confirm`. A log of an as-needed medicine inside its minimum interval is refused unless `confirm` is true, as in the web UI. `snooze` takes an optional number of minutes from 1 to 120 (a plain number or `{"minutes":n}`), default the snooze setting. Results are not answered on MQTT; the state topics show the outcome. Anyone the broker lets publish on these topics can log, skip and snooze: the broker's access control is the protection
- Session: the client uses a clean session, so the broker keeps no QoS 1 commands for the device across reconnects. A duplicate delivered within one session is acted on; the 5 s undo on the device and the editable log cover that rare case
- State job: a job in the LVGL task builds the tiles and the device fields; it runs on connect, 1.5 s after any event on the device's bus (the scheduler evaluates at the next second, and bursts of events share one job) and every 60 s while connected. A topic is published only when its JSON differs from the last one sent on this connection, and the device topic only when something but `rssi` changed or 300 s passed. State publishes go through the same in-flight limit as events. A medicine that leaves the list while connected has its retained state topic cleared
- Receive queue: the MQTT task hands acknowledgements and command messages to the delivery task through a queue of 32 in PSRAM. The delivery task empties the queue between webhook requests, because one request can hold it about 10 s. A message that still does not fit is counted (queue, command) and logged, never silent. The retained topics of the cleanup scan do not use this queue: while a scan listens, the MQTT task stores each retained topic it receives in a 32 KB PSRAM area, so a burst of retained messages loses none
- Connect steps: `online` and any of the three subscriptions that a refused call left undone are retried every 5 s and logged once
- Flow: at most 4 QoS 1 publishes in flight. Outbox events go one at a time, in order. A publish not acknowledged within 30 s is sent again; acknowledgements for other message ids are ignored
- Memory: esp-mqtt's outbox data and its in and out buffers, the message queue and the command payload pool live in PSRAM; only task stacks and sockets use internal RAM. If the esp-mqtt version in use cannot place its outbox or buffers in PSRAM through its configuration, state topics and discovery configs go out QoS 0 retained, only `online` and events use QoS 1, the buffers drop to 1536 bytes, and bursts are paced at one publish per 50 ms
- Topics and HA unique ids use the stable medicine `id`, so renaming a code or reusing it after archiving never moves or collides with entities
- Home Assistant discovery (toggle, default off): retained configs on `<prefix>/<component>/<node>/<object>/config`, node id `hygeia_<4 hex>` (from the MAC, as the access point name, lower case). One device "Hygeia", manufacturer "Waveshare", model "ESP32-S3-Touch-AMOLED-2.16", with the firmware version. Per active medicine: `sensor` last taken (timestamp), `sensor` next due (timestamp), `binary_sensor` due, `button` log (sends `PRESS` to the log command topic), and `sensor` pills left only while pill counting is on. Device-level: `sensor` battery, `binary_sensor` charging, `sensor` adherence 30 d, and an `event` entity on `<base>/event` that lists all nine event types (listing only due and missed would make Home Assistant warn on every other event). Entity names use the medicine name (for example "Antibiotic last taken"); unique ids use the medicine id (`<node>_med_<id>_<object>`). Every entity uses `<base>/status` for availability
- Removal: on every connect a scan subscribes to `<prefix>/+/<node>/+/config` and `<base>/med/+/state` for 3 s and stores each retained topic (a non-empty retained message of any size; only the topic is kept). When the 3 s are over it unsubscribes and clears, with an empty retained message, each stored topic that is no longer wanted: configs of medicines archived or deleted, the pills sensor of a medicine with pill counting off, every config when discovery is off, and state topics of medicines that are not active. Each topic is judged alone, so a scan that missed topics can leave stale ones but never clears a wanted one. If the 32 KB area fills (about 300 topics fit; a full set of 50 medicines uses about 25 KB), the scan is marked incomplete, the topics it did store are judged and cleared, and the scan runs again; it is repeated up to 6 times in a row, and a warning is logged if topics may still remain. A clear that the client reports lost, or that is not acknowledged within 30 s, also triggers another scan. Without a list of medicines (the state job did not finish) nothing is judged and the scan runs again after 5 s. This also cleans up after a medicine deleted while the device was offline, with no list kept in flash. A medicine archived or deleted while connected loses its configs and state topic at once (after the 1.5 s state job); turning discovery off while connected clears its configs and runs the scan at once; changing the discovery prefix while connected scans the old prefix and clears what is there
- Discovery publishing: after `online` and the first state, every wanted config is published retained (QoS 1), one per 50 ms through the in-flight limit, and again when the medicine's name changes. A config that the client reports lost is sent again. The pills sensor config is cleared at once when pill counting is switched off
- Adherence: `adherence_30d` in the device state is the sum of taken over the sum of resolved for the 30-day window of every active medicine, as `GET /api/history?days=30` computes it per medicine (the same function). It is computed after each connect and then at most once per 30 minutes after a taken, skipped, missed or undone event, never while disconnected, one medicine per job and one job per 50 ms, so the screen is never held for long. It is null while tracking has not started or nothing is resolved yet

## Webhooks

- At most 4 URLs (this bounds NVS, RAM and the delivery loop), `http://` or `https://`, each with its own on switch, event selection (default every type) and optional secret (write-only). A webhook keeps its slot; the web UI never reorders webhooks. Removing one sends an empty slot (`"url":""`, off, no secret, no events) at the same index, so the webhooks after it keep their index, their secret and their waiting events
- Event types: `taken`, `skipped`, `missed`, `due`, `snoozed`, `undone`, `battery_low`, `battery_critical`, `clock_invalid`. `undone` lets a receiver that counts doses take back a dose undone within 5 s. One `clock_invalid` event is sent per boot when the device starts with an invalid clock, with `ts` null
- POST with `Content-Type: application/json`, members in this order: `{"id","event","ts","tz","device","medicine":{"id","code","name","dose"},"slot","log","battery"}`
  - `id`: the outbox sequence number, unique and increasing per device, starting at 1 (0 is only the id of the test event). It is never reused, because the outbox never deletes a segment before the state file holding the sequence is written. Receivers de-duplicate by `id`
  - `ts`: epoch seconds of the event, null while the clock is invalid. `tz`: the IANA zone name. `device`: the client id `hygeia-XXXX`
  - `medicine`: null for device events. `slot`: the scheduled occurrence (UTC epoch seconds) or null
  - `log`: the stored log line as the REST API returns it, or null. For `undone` it is always `{"id":n}`
  - `battery`: a number (percent), or null when unknown; for `battery_low` and `battery_critical` the reading that raised the event
- Signature, when the webhook has a secret: header `X-Hygeia-Signature: sha256=<64 lowercase hex>`, HMAC-SHA256 over the exact body bytes with the secret as the key, computed with PSA (mbedTLS). The signature carries no timestamp; receivers de-duplicate by `id`
- TLS: `https://` verifies the receiver against the ESP-IDF certificate bundle (common set) only. Receivers with a self-signed or private-CA certificate are not supported in this version, and there is no switch that skips verification. Certificate dates are not checked
- Responses: 2xx means delivered. 408, 429, 5xx and network errors are retried. 401 and 403 pause that webhook (status "auth"; its events are kept and still age) until the webhook list is saved again (an unchanged list counts), the network reconnects, a Test succeeds, or the debug console resets it (debug builds only). Every other status, including redirects, which are not followed, rejects that one event for that webhook; it is counted as "rejected" and delivery moves on
- Retry after the 1st, 2nd and 3rd failure in a row: 1, 5 and 30 minutes, then every 30 minutes, until the event has been 7 days in the outbox (then it counts as "expired")
- Attempts happen only while the device is connected to a network as a client. A new connection resets the waiting time, so queued events go out at once
- A webhook that keeps answering gets about 2 s per pass (up to 20 events); then MQTT and the other webhooks run, and it continues on the next pass, so a slow receiver never throttles MQTT
- Memory: an attempt is skipped (not counted as a failure, tried again 60 s later, status "memory") while internal free heap is below 63488 bytes for HTTPS or 45056 bytes for HTTP. With many medicines the device may hold HTTPS webhooks back while its memory is low; the web UI shows why. Attempts also wait while a firmware update, upload, export or import runs (status "busy")
- Test (`POST /api/webhook/test`): asynchronous, answers 202; the result shows in `GET /api/integrations` and through the live event `{"ev":"integ"}`. The test event (`"id":0`, `"event":"test"`) is signed, sent to one saved webhook by index, outside the outbox, and works while that webhook is switched off. It obeys the same memory and busy checks as delivery. The web UI disables Test on a row with unsaved changes

## Outbox

Persistent queue on LittleFS for MQTT and webhook events not yet delivered (offline, Wi-Fi off on battery, broker or receiver down, reboots in between).

- Files: append-only segment files of 100 events each and a small state file, all in `/data/outbox`. A ring file with fixed slots is not used: overwriting a slot inside a large LittleFS file rewrites the file from that block onward, so it wears the flash far more than appending. The ring behaviour comes from deleting a whole segment once every destination is past it and the state file has been written
- Capacity: 1000 events, counted from the oldest event still waiting for any destination. When full, the oldest waiting event is dropped for every destination and counted in "Dropped when full". Events reach the outbox through a capture queue of 128 events in RAM, which holds a scheduler pass that marks many doses missed at once; an overflow is counted as "lost"
- Counters: dropped, expired, rejected and lost are shown in the web UI and never reset by the device (no reset button in this version)
- Order is kept per destination: each destination gets its events in the order they happened, and a stuck webhook never holds back MQTT or another webhook
- Delivery is at least once: after a crash or power cut, events acknowledged in the last 10 s can be sent again with the same `id`
- Turning a destination off or removing a webhook discards the events waiting for it. Editing a webhook's URL keeps its slot, its secret and its waiting events, so a typo fix still delivers them. MQTT keeps its events across a broker URL change
- Expiry: an event is dropped as expired 7 days after it entered the outbox. The time counts from the enqueue, not from the event's own time, so a dose backdated by more than 7 days is still delivered. Events enqueued while the clock is invalid never expire by age
- Write budget: with no destination on, no event touches flash and `/data/outbox` is not opened. Otherwise each event costs one fsync'd append. On LittleFS an append to a partly filled last block copies that block into a freshly erased one, so the real cost is one 4 KB erase and up to 4 KB of programming plus a metadata commit per event, plus at most one state write per 10 s of delivery and one forced state write and segment deletion per 100 events, and one state write when a destination is turned off or removed (so a new webhook in the same slot never inherits the old one's waiting events after a power cut). At 50 events a day that is about 100 erases a day, spread over about 2500 blocks by LittleFS wear levelling. No timer writes anything
- Storage down: when the storage partition did not mount or the outbox cannot be opened, a RAM outbox takes over (its events last until a restart) and the status shows "storage"
- Memory: the RAM outbox (one arena of about 1.1 MB), the capture queue, records and payload buffers live in PSRAM

## Partitions

16 MB flash: NVS 64 KB, otadata, two OTA app slots of 3 MB each, LittleFS storage for the remaining space (about 9.5 MB) holding medicines, log and outbox. Exact table left to the builder within these proportions.

The web UI is embedded in the app image (gzipped, expected well under 300 KB), so a firmware update always ships a matching web UI and REST API. User data lives only in NVS and the storage partition, which no update touches.

Release artefacts:

- `hygeia.bin`: application image, for the web UI update and the GitHub update
- `hygeia-full.hex`: bootloader, partition table, otadata and application as an Intel HEX file, flashed with `esptool write-flash 0x0 hygeia-full.hex` (esptool 5 or later, as installed by ESP-IDF v6.0.2) for a first install. HEX leaves the gaps between regions unwritten, so flashing it writes nothing inside the NVS and storage partitions: settings, medicines and the log survive a full reflash
- `SHA256SUMS`: SHA-256 of both. The device's GitHub update depends on the names `hygeia.bin` and `SHA256SUMS`
- `hygeia-<ver>-factory.bin` (planned, CI does not publish it yet): full image including an empty storage partition, for first install or a deliberate wipe; the docs label it as erasing all data
- Store and settings carry a schema version; migrations run on boot after an update, with a backup of the old files kept until the next successful boot

## Repository and publishing

- Name `hygeia`, owner `giovi321`, private until stable
- License CC BY-NC-SA 4.0 for everything (decided). README states the project is source-available for non-commercial use, not open source
- Nothing personal in the repo. Examples use generic medicines (Antibiotic, Vitamin D, Magnesium), `HomeNet`, `broker.lan`, `n8n.lan`. Secrets never in source; `secrets*.h`, `.env`, `sdkconfig` gitignored
- Git identity `giovi321`, no surname, no real email
- README in the Asclepius format: centred logo, badges (CI, docs, license), "Read the docs" button, short not-a-medical-device note, plain-language intro, mockup images, quick start (flash a release with esptool or the web flasher, first boot, AP QR), what it does, hardware, tech stack, development, license
- Docs: Astro Starlight in `docs/`, same setup as Asclepius (`site: https://giovi321.github.io`, `base: /hygeia`), sections Getting started (flashing, first boot, Wi-Fi), User guide (home, logging, reminders, history, gestures, travel and time zones, battery), Web UI, Integrations (MQTT, Home Assistant, webhooks with an n8n example), Hardware notes, Architecture, REST API, Development, Troubleshooting. `docs.yml` workflow present but manual-only (`workflow_dispatch`) until the repo is public, because GitHub Pages does not publish from a private repo on the free plan
- CI: GitHub Actions building the firmware in the official ESP-IDF container, building the web UI, running host unit tests; release workflow attaches merged `.bin` files
- Logo kit already in `docs/public/assets/` (decided: "dose dial", slate #495155 ring and crimson #8E4449 capsule; on black #D9DAD6 and #D0646B)

## Testing

- Host unit tests (plain C, built with CMake on the host or the ESP-IDF linux target) for `scheduler`, `store`, `clock` time-zone handling, `outbox`, import/export round trip. The scheduler tests cover DST changes, time-zone changes, as-needed intervals, missed detection, multiple doses due at once
- UI layout checks by rendering screens to PNG off-device; the community `lvgl-preview-skill` may be used after its code has been reviewed
- Hardware checklist for each release: boot splash, touch in all corners, logging and undo, due card and chime, sleep and RTC wake on battery, PWR short/long press, power off and on, AP join by QR, web UI parity, MQTT and HA discovery, webhook with signature, OTA and rollback, clock invalid flow, add, edit and archive a medicine on the device, auto-rotate in all four orientations, rotation lock, shake to undo, cabinet open in both trigger modes, teach and test mode

## Left to the builder

- Internal module APIs, task layout, stack sizes, LVGL buffer sizes
- Exact partition table, JSON schema versions and migration code
- Chime sound design, within "short, gentle, three tones"
- Exact colour palette values for medicines, within the rules above
- Order of work, as long as offline reminding and logging come before any network feature

## Decisions made in this design without explicit user input

These are my calls; flag any you disagree with.

1. Invalid clock blocks logging and pauses reminders until the time is set
2. A medicine code can be 1 or 2 characters (you said "typically one letter")
3. Automatic `missed` log entries written after the missed threshold
4. Chime repeats every 2 min, 5 plays in total per sequence, while unacknowledged
5. Client mode never falls back to AP on its own; it shows a hint instead
6. Home Assistant discovery is off by default
7. Retention: keep everything until storage is 80% full, minimum 1 year
8. Soft limit of 50 active medicines; archived medicines do not count toward it, and the store holds at most 100 medicines including archived ones
9. Secrets excluded from exports unless asked for
10. Font Inter, converted for LVGL
11. Spec lives in `design/spec.md` rather than under `docs/`, so the published docs site stays user-facing
12. Early dose window 60 min by default, per medicine
13. A dose taken after "missed" but before the next occurrence turns the missed entry into a late taken entry
14. Periods while off, flat or with an invalid clock are recorded as untracked, not missed, and excluded from adherence
15. MQTT topics and HA ids keyed by medicine id; codes limited to A–Z and 0–9
16. Web UI embedded in the firmware; updates and reflashes never touch the log; a separate factory image wipes
17. Time-zone table in firmware so the zone can be changed on the device offline
