---
title: MQTT
description: Publish medicine and device state to an MQTT broker, send events, and log, skip or snooze doses with commands.
---

The device connects to one MQTT broker as a client. It publishes the state of each medicine and of the device, sends the same events as the [webhooks](../webhooks/), and accepts commands that log, skip or snooze a dose. Reminders and logging never depend on it: a broker that is down or slow only delays what the device publishes.

## Setup

In the web UI, open Settings, MQTT (see [Web UI](../../user-guide/web-ui/#settings)). Turn it on, enter the broker URL, the user and password if the broker asks for them, and the base topic, then press Save. The line at the top of the panel shows the state of the connection and, when nothing goes out, the reason in plain words. The password field shows "Saved" when one is set; type to replace it, or press Clear to remove it.

The same settings can be sent through the REST API, `PUT /api/settings`, from a paired browser. Paste this in the browser console (developer tools) of the web UI and run it:

```js
fetch('/api/settings', {method: 'PUT', headers: {'X-Hygeia': '1', 'Content-Type': 'application/json'},
  body: JSON.stringify({mqtt: {on: true, url: 'mqtt://broker.lan', user: 'hygeia', password: 'example-secret',
  base: 'hygeia'}})}).then(r => r.json()).then(console.log)
```

- `on` turns MQTT on. `url` is `mqtt://broker.lan` (port 1883) or `mqtts://broker.lan` (port 8883). A port can follow the host, for example `mqtt://192.168.1.23:1884`
- `user` and `password` are needed only if the broker asks. The password is write-only: `GET /api/settings` shows only `password_set`. Leave `password` out to keep the one that is set, send `""` to clear it
- `base` is the base topic, `hygeia` by default. Every topic below starts with it, written here as `<base>`
- `ha` and `ha_prefix` switch on [Home Assistant discovery](../home-assistant/)
- Members you leave out keep their value. See [REST API](../../reference/rest-api/) for the whole `mqtt` member

The client id is `hygeia-` and the last 4 hex digits of the MAC address, the same as the access point name. It cannot be changed. MQTT 3.1.1 over TCP or TLS is the only transport: there are no WebSocket transports and no MQTT 5.

Host names ending in `.local` are resolved through mDNS. If your network does not forward mDNS between the device and the broker, use the IP address.

## Radio and battery

The device connects only while the radio is on for another reason and the device is a Wi-Fi client, not an access point. MQTT never turns the radio on, never keeps it on, and its traffic does not count as activity for the 10-minute battery idle limit. On battery the radio is on only after you turn Wi-Fi on and stops after 10 minutes without a web request or a touch, so a subscriber sees the device as unavailable most of the time. Events wait in the outbox until the next connection. See [Idle clock and power](../../user-guide/idle-power/#the-radio-mqtt-and-webhooks).

A command that logs, skips or snoozes a dose counts as activity, like a web request: someone acted.

## TLS limits

`mqtts://` verifies the broker against the certificate bundle built into the firmware (the common set of public authorities). A broker with a self-signed certificate or a private authority is not supported in this version, and there is no switch that skips the check. The device does not check certificate dates. Use `mqtt://` on a trusted network if the broker has no public certificate.

## Topics

| Topic | Direction | QoS | Retained | Content |
| --- | --- | --- | --- | --- |
| `<base>/status` | device to broker | 1 | yes | `online` or `offline` |
| `<base>/med/<id>/state` | device to broker | 1 | yes | state of one active medicine |
| `<base>/device` | device to broker | 1 | yes | state of the device |
| `<base>/event` | device to broker | 1 | no | one event, same payload as a webhook |
| `<base>/med/<id>/log` | broker to device | 1 | ignored if retained | log a dose |
| `<base>/med/<id>/skip` | broker to device | 1 | ignored if retained | skip the dose that is due |
| `<base>/med/<id>/snooze` | broker to device | 1 | ignored if retained | snooze the reminder |

`<id>` is the medicine id shown in the web UI, not its code, so renaming or re-coding a medicine never moves a topic.

### Availability

The device publishes `online` (retained) on every connect. Before it stops on purpose, for example when you turn MQTT off or change the broker, it publishes `offline` (retained). A lost connection triggers the last will, which is also `offline` (retained). With a keepalive of 60 s the broker notices a lost device within about 90 s. When the radio turns off on battery the device does not publish `offline` first: the broker's last will marks it offline after about 1.5 keepalives, so about 90 s later.

### Medicine state

Published when the content changes, and again after each connect. Example for Antibiotic (A) with id 1:

```json
{"id":1,"code":"A","name":"Antibiotic","status":"due","due":true,"last_taken":"2026-11-06T07:02:00Z","next_due":"2026-11-06T19:00:00Z","pills_left":12}
```

- `status` uses the tile kinds of `GET /api/state`: `as_needed`, `no_dose`, `since`, `due`, `late`, `snoozed`, `missed`
- `due` is true for `due`, `late` and `snoozed` (a dose that can still be taken) and false for `missed`
- `last_taken` and `next_due` are UTC times in ISO 8601, or `null`
- `pills_left` is a number, or `null` when pill counting is off

The state is checked after every event and every 60 s, and published only when it differs from what was last sent on this connection. The scheduler updates at the next second after an event, so a state topic follows an action about 1.5 s later. A status that changes with the clock alone, such as `due` turning to `late`, appears within a minute.

### Device state

```json
{"battery":82,"charging":false,"usb":false,"wifi_mode":"client","rssi":-61,"clock_source":"ntp","clock_valid":true,"adherence_30d":null,"fw":"1.4.0"}
```

Published when anything except `rssi` changed, and at least every 5 minutes so that `rssi` stays current. `battery`, `rssi` and `clock_source` are `null` when unknown.

`adherence_30d` is the share of resolved doses in the last 30 days that were taken, in percent, over all active medicines. It uses the same count as the 30-day History page. The device computes it after each connect and then at most every 30 minutes after a dose was taken, skipped, missed or undone, and never while disconnected. It is `null` until the figure exists, and when no dose in the window is resolved.

### Events

`<base>/event` carries the webhook payload (see [Webhooks](../webhooks/#payload)) for the nine event types `taken`, `skipped`, `missed`, `due`, `snoozed`, `undone`, `battery_low`, `battery_critical` and `clock_invalid`. Timestamps are epoch seconds. Events are not retained. Editing a log, changing medicines and untracked periods are not sent as events; they only refresh the state topics.

Delivery is at least once. After a dropped connection or a restart the same event can arrive twice with the same `id`. Its `id` is unique per device and only grows, so a subscriber that must not count twice de-duplicates by it. Events wait in the outbox while the broker or the radio is off, for up to 7 days.

## Commands

Publish to `<base>/med/<id>/<command>`. The medicine must exist and be active. A command published with the retained flag is ignored, so a stale command never runs again after a reconnect. The device does not answer a command on MQTT: the state topic shows the outcome. Results are in the device log, with the command, the medicine id and the result, but never the payload.

### log

Logs a dose as taken now. Any payload works, for example `PRESS` from a Home Assistant button:

```
mosquitto_pub -h broker.lan -t hygeia/med/1/log -m PRESS
```

A JSON object adds options:

```
mosquitto_pub -h broker.lan -t hygeia/med/1/log -m '{"note":"with food","ts":1794038520}'
```

The example `ts` is 2026-11-07 08:02 UTC. Use a time after the build date of your firmware and not more than 60 s ahead of now; `date +%s` prints the current time.

- `note`: up to 63 bytes
- `ts`: epoch seconds of the dose. Same window as the web UI: not before the firmware build date and not more than 60 s ahead. A value outside it makes the command fail
- `confirm`: true accepts a dose of an as-needed medicine inside its minimum interval. Without it the log is refused, as in the web UI. A log with `ts` is not checked against the interval

### skip

Skips the dose that is due at the moment. The payload is ignored. A medicine with no dose due fails.

### snooze

Snoozes the reminder. An empty payload or `PRESS` uses the snooze setting. A number of minutes from 1 to 120 sets it, either plain or as an object:

```
mosquitto_pub -h broker.lan -t hygeia/med/1/snooze -m 15
mosquitto_pub -h broker.lan -t hygeia/med/1/snooze -m '{"minutes":15}'
```

### Delivery of commands

The device uses a clean session, so the broker keeps no commands for it while it is away. A duplicate delivered within one session is acted on twice; the 5 s undo on the device and the editable log cover that rare case. A command payload is limited to 512 bytes.

### Access control

Anyone the broker lets publish on these topics can log, skip and snooze doses. The broker's access control is the only protection. Give the device its own user, allow it to publish under `<base>/` and under the discovery prefix, subscribe to `<base>/med/+/#`, and allow only the users that should act on doses to publish commands.

On every connect the device also listens for 3 seconds to the retained messages under `<prefix>/+/<node>/+/config` and `<base>/med/+/state`, to remove entities and state topics that are no longer wanted (see [Home Assistant](../home-assistant/#archive-and-delete-remove-entities)). Allow it to subscribe to those too. This runs with discovery off as well, so that turning discovery off removes the entities.
