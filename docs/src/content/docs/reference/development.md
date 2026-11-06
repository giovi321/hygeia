---
title: Development
description: Repository layout, host tests and the serial debug console.
---

## Repository layout

| Path | Contents |
|---|---|
| `main/` | `app_main.c`, the debug console, Kconfig |
| `components/hy_core` | pure logic with no hardware: types, medicines, occurrences, scheduler rules, actions, navigation and gestures, settings, time zone table, summaries |
| `components/hy_board` | display, touch, PMU, brightness, IMU driver |
| `components/hy_motion` | the motion task: reads the IMU while the screen is on or dimmed, orientation and shake |
| `components/hy_clock` | RTC driver, time zone, clock validity |
| `components/hy_sched` | runs the scheduler with the real clock and store |
| `components/hy_store` | LittleFS mount, medicine and log files, NVS settings |
| `components/hy_audio` | chime playback |
| `components/hy_net` | Wi-Fi client and access point, mDNS, captive portal, the battery rule for the radio |
| `components/hy_web` | HTTP server, REST API, WebSocket events, pairing sessions, backup and firmware update |
| `components/hy_ui` | LVGL screens, theme, fonts |
| `web/` | the web UI: HTML, CSS and JavaScript, bundled into the firmware by `tools/web_bundle.py` |
| `tools/` | the web bundle builder, the web UI mock server `webmock.py`, the parity table generator and the time zone table generator |
| `host_tests/` | unit tests that build and run on the host |
| `design/` | the design spec, hardware notes and mockups |
| `docs/` | this documentation site |

The core knows nothing about screens. UI code calls actions; changes are published as events.

## Host tests

The pure logic is tested on the host with plain C, gcc and make:

```bash
make -C host_tests test
```

## Web UI development

`python tools/webmock.py` serves the web UI from `web/` and answers the API from sample files, so the pages can be built without a board. Edits show on reload. Options such as `--unpaired`, `--slow` and `--low-battery` reproduce the pairing page, a slow device and a refused firmware update. The table in [REST API](../rest-api/#screen-and-web-parity) is generated from the firmware route table by `tools/gen_parity.py`.

## Debug console

The default build has a serial console on the USB Serial/JTAG port with line commands. It is for development; the release build turns it off. Commands include `time <epoch> [posix_tz]`, `zone <iana> <posix_tz>`, `clock`, `tap <code>`, `undo`, `logs`, `alarm <seconds>`, `chime`, `shot`, `nav`, `idle`, `gesture`, `remindtest`, `due`, `history`, `detail`, `note`, `earlier`, `qs`, `settings`, `settz <iana>`, `perf`, `powerstat`, `seed4`, `imu`, `motion`, `rot`, `madctl`, `touch trace on|off` (logs the first reported points of each stroke, for the edge zone measurement) and `shake`. The set changes as the firmware changes; the source is `main/console.c` and `main/app_main.c`.

Commands added for the Cabinet open teach view and the battery work:

| Command | What it does |
|---|---|
| `cab ui <idle\|running\|saved\|checking\|ok\|miss\|nomove\|failed\|nostill\|teststopped>` | Shows the teach view in that phase without the runner, for screenshots. `nostill` and `teststopped` are the saved view after a check that did not start still or was stopped |
| `cab press <start\|test\|again\|keep\|done\|cancel>` | Presses a button of the teach view without a finger, so the `cue` lines of a real teach or check can be read. Open the view first with `cab ui idle`; a button that is not shown in the current phase does nothing (Cancel is shown while idle, failed, running and checking) |
| `gesture <bandright\|banddown\|bandup>` | A stroke that starts in the 80 px gesture band, with nothing under the finger that scrolls. With `consumed` a scrolling widget claims the stroke right after the press |
| `sleepstat` | Flags in force, sleep counters, wakes by cause, the last 12 touch judgments on a dark screen, `lsstat`, PM locks |
| `pwrx` | Lists the battery investigation flags and `lsstat` (entries that ran, rejected or woke at once, too short to try). Prints a warning when the touch driver released the controller reset during a `touch` run, which makes that run invalid. The `touch` flag does what the setting Double tap to wake off does, so it is an extra way to force it |
| `pwrx <touch\|nonet\|panel\|wakers\|lsvbus\|gesture> on\|off` | Sets one flag (RAM only, off at every boot). `lsvbus` needs Wi-Fi off (`wifi mode off`) on USB, or the radio keeps the sleep mode off. `gesture` puts the touch controller in its gesture wake mode during light sleep (an experiment, see design/hardware-notes.md) |
| `pwrx mv [clear]` | Prints the ring of 60 s battery readings (RTC memory), or empties it |
| `pmu rails` | Read-only dump of the AXP2101 rail registers |

What each flag does and risks is in `design/hardware-notes.md`, part "Battery investigation tools".

## Docs site

The documentation is an Astro Starlight site in `docs/`.

```bash
cd docs
npm ci
npm run dev
npm run build
```

The site is built for `https://giovi321.github.io/hygeia/`. The `docs.yml` workflow builds it on every push and pull request that touches `docs/`, and deploys it to GitHub Pages on push to `main` once the repository is public.

## Conventions

- Strings shown on the device live in one header, `components/hy_ui/src/strings.h`, so a translation is possible later. The firmware is English only
- Direct component dependencies are pinned to exact versions
- Examples use generic medicines (Antibiotic, Vitamin D, Magnesium); no real names, networks or addresses belong in the repository
