# Hygeia design

Status: draft for approval, 2026-10-28

## Intent

Hygeia is firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.16 that turns the board into a medicine reminder and dose log for one person. It sits on a desk on USB power and travels on its own battery. Logging a dose is one tap on a big letter tile. At scheduled times the screen shows a full-screen card and plays a chime. History is visible on the device and in a web UI served by the device, where the medicines and the log can also be edited.

The device must work with no network at all. Wi-Fi, MQTT, Home Assistant and webhooks are add-ons that make it reachable, never dependencies for reminding or logging.

What makes it worth building instead of using a phone app: a dedicated object with a glanceable screen, no account, no cloud, data that stays on the device and the owner's LAN, and integration with an existing Home Assistant / n8n setup.

## Goals

1. One-tap logging from a scrollable grid of medicine tiles, each identified by a 1–2 character code and a colour
2. Scheduled reminders on screen with chime, snooze and skip, while on USB or battery, with the screen asleep
3. Graphical history on the device: today overview, 7-day grid, per-medicine detail
4. A web UI that can do everything the screen can do, plus full editing of medicines and log entries, export and import
5. Wi-Fi client mode, access-point mode and off, switchable from the screen and the web UI
6. Clock from the onboard RTC, synced from the browser or NTP, with time zones that follow the traveller
7. MQTT with optional Home Assistant discovery, and webhooks
8. Battery level shown as a percentage, battery events published, power on/off with the onboard button
9. A repository that can be made public at any time: no personal data, docs site ready for GitHub Pages, README in the style of Asclepius

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
- No personal data in the repository: real medicine names, network names, broker addresses, tokens, email addresses or the owner's surname

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

## Stack

Decided:

- ESP-IDF v6.0.2 is the build of record, locally and in CI; direct component dependencies are pinned to exact versions and `dependencies.lock` is committed (Waveshare CI-tests v5.5.5 and v6.0.2). C, with C++ only where a component needs it
- LVGL 9.x through the managed component and `esp_lvgl_port`
- Board support from the registry component `waveshare/esp32_s3_touch_amoled_2_16` (v2.0.1 at time of writing). If its display init is unsuitable, fall back to the approach in thejimoneil/ESP32-Tetris: custom CO5300 init for the full 480×480, `esp_lcd_touch_cst9220`, double-buffered DMA, rounder callback forcing even x1/y1 and odd x2/y2, software rotation
- UI written by hand in LVGL C on one shared theme; no generated-code tools
- Web UI as a small single-page app built at compile time, gzipped, served from flash
- Storage on LittleFS; settings in NVS

Reference projects (study, do not copy wholesale, check their licences before reusing code):

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

Components (one ESP-IDF component each):

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
| `web` | HTTP server, REST API, WebSocket, static web UI, optional password |
| `mqtt` | publish state and events, receive commands, Home Assistant discovery |
| `webhook` | HTTP POST of events with retry |
| `outbox` | persistent queue of events not yet delivered to MQTT or webhooks |
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
| `dose` | free text, e.g. "1 tab", "5 ml" |
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

There is no hard limit on the number of medicines. The grid scrolls; the web UI handles ordering. A soft limit of 50 active medicines is enforced to bound memory and screen build time (builder may raise it after measuring).

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
- Secrets (Wi-Fi passwords, MQTT password, webhook secret, web UI password hash): NVS, never included in export unless the user ticks "include secrets"
- Export: CSV (log only) and JSON (medicines, log, settings). Import: JSON, with a preview of what changes and a choice of merge or replace
- Backup/restore in the web UI uses the JSON export

## Scheduling

- Schedules are in local time of the current time zone. Changing the time zone moves future occurrences to the same wall-clock times in the new zone (08:00 stays 08:00). Past log entries keep their recorded time zone
- Clock jumps from a zone change (decided): flying east, an occurrence whose local time falls inside the skipped hours becomes due immediately on arrival, never missed. Flying west, an occurrence already satisfied for that medicine on that local date and wall-clock time is not raised again when the same local time comes round a second time. The same rules apply to DST transitions
- Daylight saving handled with POSIX TZ rules. The firmware carries a compact IANA→POSIX table (generated at build time, about 20–30 KB) so the zone can be changed on the device with no network; the web UI sends the browser's IANA zone and the device looks it up in the same table
- An occurrence becomes `due` at its time, `late` after `late_after_min`, `missed` after `missed_after_min`. When it becomes missed, a `missed` log entry with source `auto` is written and an event sent
- Dose window: from `early_window_min` before the occurrence (per medicine, default 60) until the next occurrence of the same medicine. A `taken` log in that window satisfies the earliest unsatisfied occurrence; `delta_min` is negative for early doses. A log outside any window is a free log
- Taken after missed: if a dose is logged after its occurrence was marked missed, but still before the next occurrence, the `missed` entry is converted to `taken` (with its real time and lateness) and a `taken` event is sent. The original missed event is not retracted
- Untracked periods: while the device is off, the battery is flat, or the clock is invalid, occurrences cannot be observed. On return, occurrences in that gap are not marked missed. Until plan 3 adds the `untracked` interval, slots that fall while the device was off are untracked, never missed. The store records the gap as an `untracked` interval; history draws it grey, adherence excludes it, and a single `untracked` event (start, end) is sent instead of one per occurrence. Occurrences still within their window when the device returns become due normally
- Snooze defaults to 15 minutes, configurable; snoozing does not extend the missed deadline
- As-needed: logging sooner than `min_interval_min` after the previous dose shows a warning with the time since the last dose; the user can still confirm
- The scheduler exposes the next wake time (next occurrence or deadline) to `power`, which sets the RTC alarm

## Clock

- The PCF85063 is the time source. On boot, read it; if its oscillator-stop flag is set or the time is before the firmware build date, the clock is invalid
- Invalid clock: the device shows a "Set the time" screen with a QR code to the web UI. Reminders are paused and logging is disabled until the time is set, because a log with a wrong time is worse than no log
- Sync sources, in priority: browser sync (manual button, and automatically when the web UI is opened and differs by more than 2 s), NTP when in client mode (on connect and every 6 h), manual set on the device
- Each sync writes the RTC and records the source and time; the web UI shows "synced X ago via Y"

## Power

- On USB: screen dims to the idle clock after 30 s, never turns off unless the user sets it
- On battery: dims after 30 s, screen off after 2 min, CPU in light sleep. Wake sources: touch interrupt, RTC alarm (next due occurrence), AXP2101 interrupt (PWR key, USB plugged), user key
- Wi-Fi defaults to off on battery; toggle on the screen and in the web UI. Events produced while offline go to the outbox
- PWR key: short press toggles the screen; long press (about 2 s) opens a power-off confirmation slider that warns "Reminders will not fire while off" if any occurrence is due in the next 24 h; confirming powers off through the AXP2101. Pressing PWR while off turns the device on (AXP2101 hardware behaviour)
- The device turns on only with the PWR key (owner requirement, 2026-10-28). Plugging in USB or connecting the battery must not leave it running. Early in boot, before the display is initialised, firmware reads the AXP2101 power-on source; if the source is USB insertion or battery insertion rather than the PWR key, it powers off through the AXP2101 at once. The battery still charges while off. A software restart (OTA, crash, watchdog) is not a power-on event and keeps running. The exact register and whether the cold-boot source is reported reliably are verified on the board
- Battery shown as a percentage only in the top bar; red below 15%, charging indicated by a small bolt next to the number. Low battery (15%) and critical (5%) produce events
- Battery cell and battery-life target: not decided yet. The owner will check the cell they have (capacity, MX1.25 connector, polarity) before the power work starts. Until then v1 implements the light-sleep design above, and the power milestone ends with a measured discharge curve; the target and any deeper sleep work are set from that measurement
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
| Pull down from top edge | Quick settings, from any screen |
| Swipe up from bottom edge | back to Home, from any screen |
| Swipe right from left edge | close a detail page |
| Vertical swipe in the grid | scroll medicines |
| Tap tile | log as taken, show undo toast (5 s countdown) |
| Long press tile | medicine detail |
| Slide the knob right (due card) | log as taken |
| Swipe left on due card | snooze |
| Tap "Skip" (due card) | log as skipped |
| Tap while dimmed | wake |

Screens: Home (grid of rings, "+" tile shows a QR to the web UI's add page), Quick settings (Wi-Fi client / AP / off, sound, do-not-disturb, brightness, battery estimate, Settings, web UI QR), Overview (today count ring and 24 h timeline), History (7-day grid per medicine, double tap for 30 days), Medicine detail (name, dose, schedule, pill count, recent log, log with note, log at an earlier time, 30-day chart), Dose due (full screen), Logged toast, Idle (dim clock and next dose), Settings (Wi-Fi, time zone, sound, display, about), AP on (Wi-Fi QR to join, then web UI QR), Set the time, Power off confirm, Boot splash.

Several due at once: the due card shows the first, with "+2 more" and a swipe up to step through them. The card stays until the entry is taken, skipped or snoozed; stepping through only changes which entry is shown.

History: a dose taken late gets an amber dot, so it differs from today's partial cell. The device cannot edit or delete log entries; edits are made only in the web UI.

Text entry: the on-screen keyboard is used for notes only (log with note). Nothing else on the device takes typed input.

Do-not-disturb and quiet hours: the card still appears, the chime is silent. Quiet hours are a web UI setting; the device has only the do-not-disturb toggle.

## Notifications

- At an occurrence: wake the screen, show the due card, play the chime (default 3 short tones, repeated every 2 min, 5 plays in total per sequence, while unacknowledged, volume from settings), send `due` event
- If the speaker path does not work on hardware, the fallback is a pulsing screen and a gentle brightness flash; the design does not depend on sound
- At `late_after`: the tile shows "late" and the card stays; at `missed_after`: `missed` log and event, card dismissed, tile shows missed until the next occurrence

## Network

- Modes: client, access point, off. Selected on the device (quick settings) and in the web UI; persisted
- Networks are added only in the web UI (decided: no on-device keyboard or scan). On the device you pick client, AP or off, and choose among saved networks. On a trip, a new network is added by turning on AP mode and opening the web UI from a phone
- Client mode: list of known networks, tries them in order of last success; on failure for 2 min, stays in client mode retrying with backoff (never silently switches to AP). The device shows a hint to start AP mode
- AP mode: SSID `hygeia-XXXX` (last 4 hex of the MAC), WPA2 with a random 10-character password generated on first boot, changeable. Device shows a Wi-Fi QR code that includes the password, then a QR to `http://192.168.4.1`. Captive-portal DNS redirects to the web UI
- mDNS name `hygeia.local` in client mode (configurable)

## Web UI

Mockups: `design/mockups/webui.html`.

- Pages: Dashboard, Medicines (list, add, edit, reorder, archive), History (30-day grid, log table with edit, delete, add, export CSV/JSON, import), Schedule (week view of all occurrences), Settings (Wi-Fi, MQTT, Home Assistant discovery, webhooks, clock, screen and sound, quiet hours, web UI password, backup/restore, firmware update)
- Responsive: sidebar on desktop, top menu on a phone
- Live updates over WebSocket from the event bus
- Optional password (off by default): session cookie after login. No TLS on the device; the docs say to keep it on a trusted LAN
- Firmware update: upload of a `.bin` to the inactive OTA slot, verified, then reboot; rollback on failed boot

REST API (JSON):

| Method and path | Action |
|---|---|
| `GET /api/state` | everything the dashboard needs |
| `GET/POST /api/medicines`, `GET/PUT/DELETE /api/medicines/{id}` | medicines |
| `POST /api/medicines/{id}/log` | log taken, optional `ts`, `note` |
| `POST /api/medicines/{id}/skip`, `/snooze` | due-card actions |
| `GET /api/log?from&to&med`, `POST /api/log`, `PUT/DELETE /api/log/{id}` | log |
| `POST /api/log/{id}/undo` | undo within 5 s (also used by the device toast) |
| `GET /api/export.csv`, `GET /api/export.json`, `POST /api/import` | export and import |
| `GET/PUT /api/settings` | settings (secrets write-only) |
| `POST /api/time` | set time and zone from the browser |
| `POST /api/wifi/mode` | client, ap, off |
| `POST /api/webhook/test` | send a test event |
| `POST /api/ota` | firmware upload |
| `GET /ws` | WebSocket event stream |

## MQTT

- Broker URL, username, password, base topic (default `hygeia`), client id `hygeia-XXXX`
- Availability: `hygeia/status` = `online`/`offline` (last will)
- State: `hygeia/med/<id>/state` JSON per medicine (code, name, last taken, next due, status, pill count); `hygeia/device` JSON (battery, charging, Wi-Fi mode, RSSI, clock source)
- Events: `hygeia/event` JSON, same payload as webhooks
- Commands: `hygeia/med/<id>/log`, `hygeia/med/<id>/skip`, `hygeia/med/<id>/snooze`
- Topics and HA unique ids use the stable medicine `id`, so renaming a code or reusing it after archiving never moves or collides with entities
- Home Assistant discovery (toggle, default off): one device "Hygeia"; per medicine a `sensor` last taken (timestamp), `sensor` next due (timestamp), `binary_sensor` due, `button` log, optional `sensor` pills left; device-level `sensor` battery, `binary_sensor` charging, `sensor` adherence 30 d; an `event` entity for due/missed. Discovery topics removed when a medicine is archived or discovery is turned off

## Webhooks

- One or more URLs, each with its own event selection: `taken`, `skipped`, `missed`, `due`, `snoozed`, `battery_low`, `battery_critical`, `clock_invalid`
- POST JSON: `{"event", "ts", "tz", "device", "medicine": {"code", "name", "dose"}, "log": {...}, "battery"}`
- Optional shared secret: `X-Hygeia-Signature` HMAC-SHA256 of the body
- HTTPS supported with the ESP-IDF certificate bundle
- Retries with backoff (1, 5, 30 min), then kept in the outbox until delivered or older than 7 days

## Outbox

Persistent ring file on LittleFS for MQTT and webhook events not yet delivered (offline, Wi-Fi off on battery, broker down). Capacity 1000 events; oldest dropped when full, with a counter shown in the web UI. Delivered in order when the network returns.

## Partitions

16 MB flash: NVS 64 KB, otadata, two OTA app slots of 3 MB each, LittleFS storage for the remaining space (about 9.5 MB) holding medicines, log and outbox. Exact table left to the builder within these proportions.

The web UI is embedded in the app image (gzipped, expected well under 300 KB), so a firmware update always ships a matching web UI and REST API. User data lives only in NVS and the storage partition, which no update touches.

Release artefacts:

- `hygeia-<ver>-ota.bin`: app only, for the web UI firmware update
- `hygeia-<ver>-update.bin` and flash offsets for esptool / the web flasher: bootloader, partition table and app, never the storage or NVS partitions, so reflashing keeps the log
- `hygeia-<ver>-factory.bin`: full image including an empty storage partition, for first install or a deliberate wipe; the docs label it as erasing all data
- Store and settings carry a schema version; migrations run on boot after an update, with a backup of the old files kept until the next successful boot

## Repository and publishing

- Name `hygeia`, owner `giovi321`, private until stable
- Licence CC BY-NC-SA 4.0 for everything (decided). README states the project is source-available for non-commercial use, not open source
- Nothing personal in the repo. Examples use generic medicines (Antibiotic, Vitamin D, Magnesium), `HomeNet`, `broker.lan`, `n8n.lan`. Secrets never in source; `secrets*.h`, `.env`, `sdkconfig` gitignored
- Git identity `giovi321`, no surname, no real email, no AI attribution in commits, code comments or docs
- README in the Asclepius format: centred logo, badges (CI, docs, licence), "Read the docs" button, short not-a-medical-device note, plain-language intro, mockup images, quick start (flash a release with esptool or the web flasher, first boot, AP QR), what it does, hardware, tech stack, development, licence
- Docs: Astro Starlight in `docs/`, same setup as Asclepius (`site: https://giovi321.github.io`, `base: /hygeia`), sections Getting started (flashing, first boot, Wi-Fi), User guide (home, logging, reminders, history, gestures, travel and time zones, battery), Web UI, Integrations (MQTT, Home Assistant, webhooks with an n8n example), Hardware notes, Architecture, REST API, Development, Troubleshooting. `docs.yml` workflow present but manual-only (`workflow_dispatch`) until the repo is public, because GitHub Pages does not publish from a private repo on the free plan
- CI: GitHub Actions building the firmware in the official ESP-IDF container, building the web UI, running host unit tests; release workflow attaches merged `.bin` files
- Logo kit already in `docs/public/assets/` (decided: "dose dial", slate #495155 ring and crimson #8E4449 capsule; on black #D9DAD6 and #D0646B)

## Testing

- Host unit tests (plain C, built with CMake on the host or the ESP-IDF linux target) for `scheduler`, `store`, `clock` time-zone handling, `outbox`, import/export round trip. The scheduler tests cover DST changes, time-zone changes, as-needed intervals, missed detection, multiple doses due at once
- UI layout checks by rendering screens to PNG off-device; the community `lvgl-preview-skill` may be used after its code has been reviewed
- Hardware checklist for each release: boot splash, touch in all corners, logging and undo, due card and chime, sleep and RTC wake on battery, PWR short/long press, power off and on, AP join by QR, web UI parity, MQTT and HA discovery, webhook with signature, OTA and rollback, clock invalid flow

## Left to the builder

- Internal module APIs, task layout, stack sizes, LVGL buffer sizes
- The SPA framework for the web UI (small and dependency-light; Preact or vanilla) and its build tooling
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
8. Soft limit of 50 active medicines
9. Secrets excluded from exports unless asked for
10. Font Inter, converted for LVGL
11. Spec lives in `design/spec.md` rather than under `docs/`, so the published docs site stays user-facing
12. Early dose window 60 min by default, per medicine
13. A dose taken after "missed" but before the next occurrence turns the missed entry into a late taken entry
14. Periods while off, flat or with an invalid clock are recorded as untracked, not missed, and excluded from adherence
15. MQTT topics and HA ids keyed by medicine id; codes limited to A–Z and 0–9
16. Web UI embedded in the firmware; updates and reflashes never touch the log; a separate factory image wipes
17. Time-zone table in firmware so the zone can be changed on the device offline
