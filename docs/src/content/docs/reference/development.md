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
| `components/hy_ui` | LVGL screens, theme, fonts |
| `host_tests/` | unit tests that build and run on the host |
| `design/` | the design spec, hardware notes and mockups |
| `docs/` | this documentation site |

The core knows nothing about screens. UI code calls actions; changes are published as events.

## Host tests

The pure logic is tested on the host with plain C, gcc and make:

```bash
make -C host_tests test
```

## Debug console

The default build has a serial console on the USB Serial/JTAG port with line commands. It is for development; the release build turns it off. Commands include `time <epoch> [posix_tz]`, `zone <iana> <posix_tz>`, `clock`, `tap <code>`, `undo`, `logs`, `alarm <seconds>`, `chime`, `shot`, `nav`, `idle`, `gesture`, `remindtest`, `due`, `history`, `detail`, `note`, `earlier`, `qs`, `settings`, `settz <iana>`, `perf`, `powerstat`, `seed4`, `imu`, `motion`, `rot`, `madctl` and `shake`. The set changes as the firmware changes; the source is `main/console.c` and `main/app_main.c`.

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
