---
title: Roadmap
description: Features that are planned and not built. No dates are given.
---

:::caution[Not built]
Everything on this page is planned and does not exist in the firmware. The plans come from `design/spec.md` and can change. No dates are given.
:::

## Wi-Fi and access point mode

Planned: Wi-Fi client, access point and off modes, switchable from quick settings. Client mode would try saved networks in order of last success and never fall back to the access point on its own. Access point mode would use the SSID `hygeia-XXXX` with a generated password, a Wi-Fi QR code on screen and a captive portal. Networks would be added only in the web UI. The device would answer as `hygeia.local` over mDNS in client mode.

Today the Wi-Fi tile, the Access point tile and the Web UI QR button show "Not available yet".

## Web UI

Planned: a single-page app served by the device from flash, with a REST API and a WebSocket event stream. It would cover the dashboard, medicines (add, edit, reorder, archive), the log (edit, delete, add, export, import), a schedule view, settings, backup and restore, and firmware update with rollback. An optional password would protect it. There would be no TLS on the device, so it is meant for a trusted LAN. Every action available on the screen would also exist in the REST API.

Today the device adds, edits and archives medicines (see [Add, edit and archive medicines](../user-guide/medicines/)), but log entries cannot be edited or deleted, and archived medicines cannot be brought back.

## MQTT and Home Assistant discovery

Planned: MQTT with a configurable base topic, availability through a last will, state topics per medicine and for the device, an event topic and command topics to log, skip and snooze. Topics would use the medicine `id`. Home Assistant discovery, off by default, would create one device with sensors, binary sensors, buttons and an event entity per medicine.

## Webhooks

Planned: HTTP POST of events (taken, skipped, missed, due, snoozed, battery low, battery critical, clock invalid) to one or more URLs, with an optional HMAC-SHA256 signature, retry with backoff and a persistent outbox for events not yet delivered.

## Other planned items

- Export (CSV, JSON) and import
- Time sync from the browser or NTP
- Persisted untracked intervals and a retention policy for the log
- Editing every medicine detail (weekdays, dose windows, pill count, order) and unarchiving: the device edits the basics only, the rest is meant for the web UI
- Battery capacity and a battery life target: no discharge measurement exists yet
