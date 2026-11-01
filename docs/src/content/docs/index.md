---
title: "Hygeia"
description: Medicine reminder and dose logger for the Waveshare ESP32-S3-Touch-AMOLED-2.16
---

<div style="text-align: center; margin-bottom: 1rem;">
  <img src="/hygeia/assets/logo.svg" alt="Hygeia" width="96" height="96" />
</div>

Hygeia is ESP32-S3 firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.16. It turns the board into a medicine reminder and dose log for one person. Logging a dose is one tap on a tile. At scheduled times the screen shows a full-screen card and plays a chime. Everything runs on the device with no network, no account and no cloud. Wi-Fi and a web UI are optional.

:::caution[Not a medical device]
Hygeia is a personal reminder tool. It is not a medical device and must not be the only safeguard for medication that must not be missed. A reminder can fail: the battery can be flat, the device can be off, the clock can be wrong or the speaker can be muted. Check the reminders against another source before relying on them.
:::

:::note[Pre-release]
These pages describe what the firmware does today. Wi-Fi, the access point and the web UI are built. MQTT, Home Assistant and webhooks are planned and not built; they are listed on the [Roadmap](roadmap/) page only.
:::

## Why a separate device

I built Hygeia because a phone app made it too easy to put a dose off. An app lets you mark a dose as taken or snooze it from anywhere, even when you are nowhere near the medicines, and that is how doses get missed. Hygeia stays next to the medicines all the time. The reminder shows up where the pills are, so you see it when you are standing in front of them and log the dose there.

The board is small and cheap. It runs on its own battery and sleeps when the screen is off, so you can move it around the house without a cable.

It also works for people who do not use a smartphone, older people in particular. Daily use comes down to tapping a tile to log a dose and answering the reminder card when it appears. A carer can do the setup (medicines, times, doses) from a browser through the web UI, which the device serves itself over Wi-Fi. Basic setup also works on the device screen.

Technical users can go further and connect it to MQTT or webhooks to build automations, for example with Home Assistant. These integrations are planned and not built yet. The same device should suit both kinds of user, from someone who only taps tiles to someone wiring it into a home automation setup.

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
- Wi-Fi client and access point modes, switched in quick settings, in Settings, Wi-Fi or in the web UI, with QR codes to join the access point and to open the web UI, and `hygeia.local` on a network
- Web UI served by the device: dashboard, medicines, history, log, schedule, settings, Wi-Fi, backup with CSV and JSON export and import, firmware update with automatic rollback, and power off. A browser pairs once with a 6-digit code shown on the screen
- Wi-Fi turns off after 10 minutes without use on battery, unless Keep Wi-Fi on battery is on

## Pages

- [Hardware](getting-started/hardware/): board facts and what was verified on hardware
- [Build and flash](getting-started/flashing/): ESP-IDF v6.0.2, debug and release builds
- [First boot](getting-started/first-boot/): what the device shows the first time
- [Wi-Fi setup](getting-started/wifi/): the access point, QR codes, pairing and the battery rule
- [User guide](user-guide/home-and-logging/): every screen and gesture, and the [web UI](user-guide/web-ui/)
- [Data model](reference/data-model/), [scheduling rules](reference/scheduling/), [REST API](reference/rest-api/) and [security notes](reference/security/)
- [Roadmap](roadmap/): features that are planned and not built

## License

Hygeia is source-available for non-commercial use under CC BY-NC-SA 4.0. It is not open source. See [License](license/).
