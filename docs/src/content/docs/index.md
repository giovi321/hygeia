---
title: "Hygeia"
description: Medicine reminder and dose logger for the Waveshare ESP32-S3-Touch-AMOLED-2.16
---

<div style="text-align: center; margin-bottom: 1rem;">
  <img src="/hygeia/assets/logo.svg" alt="Hygeia" width="96" height="96" />
</div>

Hygeia is ESP32-S3 firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.16. It turns the board into a medicine reminder and dose log for one person. Logging a dose is one tap on a tile. At scheduled times the screen shows a full-screen card and plays a chime. Everything runs on the device with no network, no account and no cloud.

:::caution[Not a medical device]
Hygeia is a personal reminder tool. It is not a medical device and must not be the only safeguard for medication that must not be missed. A reminder can fail: the battery can be flat, the device can be off, the clock can be wrong or the speaker can be muted. Check the reminders against another source before relying on them.
:::

:::note[Version 0.3.0, pre-release]
These pages describe what the firmware does today. Wi-Fi, the web UI, MQTT, Home Assistant, webhooks, auto-rotate and shake to undo are not built; they are listed on the [Roadmap](roadmap/) page only.
:::

## What works today

- Tiles with a letter code and a colour per medicine, tap to log a dose, 5 second undo
- Full-screen due card with chime, slide to log, swipe to snooze, skip
- Today overview, 7-day and 30-day history, per-medicine detail with notes and earlier logging
- Add, edit and archive medicines on the device, with an on-screen keyboard and an optional dose
- Quick settings and settings: sound, do not disturb, brightness slider, volume, time zone, date and time, and a battery line that shows the estimate
- Battery percentage with a charging bolt, low (15%) and critical (5%) warnings with a countdown to power off
- PWR key: short press turns the screen on or off, long press opens the power-off confirmation, and the device powers on only with the PWR key
- Idle clock that dims the screen and shows the next dose; on battery the screen turns off after 2 minutes and the CPU sleeps
- Clock from the onboard RTC, with a "Set the time" screen when the RTC is invalid
- Data on the device flash: medicines and log survive power cycles and reflashes that leave the storage partition alone

## Pages

- [Hardware](getting-started/hardware/): board facts and what was verified on hardware
- [Build and flash](getting-started/flashing/): ESP-IDF v6.0.2, debug and release builds
- [First boot](getting-started/first-boot/): what the device shows the first time
- [User guide](user-guide/home-and-logging/): every screen and gesture
- [Data model](reference/data-model/) and [scheduling rules](reference/scheduling/)
- [Roadmap](roadmap/): features that are planned and not built

## Licence

Hygeia is source-available for non-commercial use under CC BY-NC-SA 4.0. It is not open source. See [Licence](licence/).
