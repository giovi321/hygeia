---
title: Roadmap
description: Features that are planned and not built. No dates are given.
---

:::caution[Not built]
Everything on this page is planned and does not exist in the firmware. The plans come from `design/spec.md` and can change. No dates are given.
:::

## Web UI additions

The web UI, Wi-Fi, pairing, export and import, and firmware update are built (see [Web UI](../user-guide/web-ui/) and [Wi-Fi setup](../getting-started/wifi/)). Not built: TLS, a captive portal page that opens the web UI by itself, and the Integrations page controls for MQTT, Home Assistant and webhooks, which show "Coming in a later version".

## MQTT and Home Assistant discovery

Planned: MQTT with a configurable base topic, availability through a last will, state topics per medicine and for the device, an event topic and command topics to log, skip and snooze. Topics would use the medicine `id`. Home Assistant discovery, off by default, would create one device with sensors, binary sensors, buttons and an event entity per medicine.

## Webhooks

Planned: HTTP POST of events (taken, skipped, missed, due, snoozed, battery low, battery critical, clock invalid) to one or more URLs, with an optional HMAC-SHA256 signature, retry with backoff and a persistent outbox for events not yet delivered.

## Other planned items

- Persisted untracked intervals and a retention policy for the log
- Battery capacity and a battery life target: no discharge measurement exists yet
