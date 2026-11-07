---
title: Home Assistant
description: Show the medicines and the device in Home Assistant through MQTT discovery, and remove entities when a medicine is archived or deleted.
---

With discovery on, the device announces its entities to Home Assistant over [MQTT](../mqtt/). Home Assistant creates them by itself, and removes them again when a medicine is archived or deleted. You need MQTT working first: the same broker must serve Home Assistant and the device.

## Turn it on

After MQTT is set up, open Settings, Home Assistant discovery in the web UI (see [Web UI](../../user-guide/web-ui/#settings)), turn it on and press Save. The discovery prefix is `homeassistant` unless your Home Assistant uses another. Discovery is sent over MQTT, so MQTT has to be on as well.

The same switch through the REST API, in the browser console of a paired browser:

```js
fetch('/api/settings', {method: 'PUT', headers: {'X-Hygeia': '1', 'Content-Type': 'application/json'},
  body: JSON.stringify({mqtt: {ha: true}})}).then(r => r.json()).then(console.log)
```

- `ha` is the discovery switch, off by default. `ha_prefix` is the discovery prefix, `homeassistant` by default, and must match the prefix of the MQTT integration in Home Assistant
- Discovery needs MQTT on and connected. The entities appear a few seconds after the device connects, and the device shows in Home Assistant under MQTT as "Hygeia"
- Turning `ha` off while connected removes the entities at once

## Entities

The device is called Hygeia (manufacturer Waveshare, model ESP32-S3-Touch-AMOLED-2.16) and carries the firmware version. For each active medicine there are up to five entities. Names start with the medicine name; for Antibiotic:

| Entity | Type | Shows |
| --- | --- | --- |
| Antibiotic last taken | sensor, timestamp | when the last dose was taken |
| Antibiotic next due | sensor, timestamp | the next scheduled dose in the next 8 days |
| Antibiotic due | binary sensor | on while a dose can still be taken (due, late or snoozed), off when it was missed or none is open |
| Antibiotic log | button | logs a dose as taken now, the same as `PRESS` on the log command |
| Antibiotic pills left | sensor, pills | the pill count; only while pill counting is on for that medicine |

The device has four more:

| Entity | Type | Shows |
| --- | --- | --- |
| Battery | sensor, % | the battery level |
| Charging | binary sensor | on while the battery charges |
| Adherence 30 days | sensor, % | doses taken over doses resolved in the last 30 days, over all active medicines |
| Event | event | the events described below |

The sensors read the [state topics](../mqtt/#medicine-state). Last taken and next due are empty until the device knows them.

### Unique ids

A unique id is the node id, the medicine id and the object, for example `hygeia_1a2b_med_1_last_taken`. The node id is `hygeia_` and the last 4 hex digits of the MAC address, in lower case. The medicine id never changes, so renaming a medicine or changing its code keeps the entity and its history; only the name changes, and Home Assistant shows the new name after the device republished the configs. A code reused after archiving never collides with an old entity.

### The event entity

The Event entity lists every event type the device sends: `taken`, `skipped`, `missed`, `due`, `snoozed`, `undone`, `battery_low`, `battery_critical` and `clock_invalid`. It reads `<base>/event`, so it shows the type of the latest event as its `event_type` attribute. It does not carry the medicine. To see which medicine an event is about, use an MQTT trigger on the event topic, as below.

## Archive and delete remove entities

- Archiving or deleting a medicine clears its entities and its state topic at once (within about 2 seconds of the change, while the device is connected)
- Turning pill counting off for a medicine clears its pills sensor
- A medicine deleted while the device was offline is cleaned up at the next connect: on every connect the device reads the retained topics of its node for 3 seconds and clears the ones it no longer wants, including all entities when discovery is off
- Restoring an archived medicine publishes its entities again under the same unique ids

## Automation example

Notify a phone when a dose is missed, with the name of the medicine. The trigger reads the event topic directly, which carries the medicine:

```yaml
alias: Notify on missed dose
triggers:
  - trigger: mqtt
    topic: hygeia/event
conditions:
  - condition: template
    value_template: "{{ trigger.payload_json.event == 'missed' }}"
actions:
  - action: notify.mobile_app_phone
    data:
      message: "Missed dose: {{ trigger.payload_json.medicine.name }}"
```

Replace `hygeia` with your base topic and `notify.mobile_app_phone` with your own notify service. The same payload as the [webhooks](../webhooks/#payload) arrives, with the event name in `event` and the time in `ts` (epoch seconds). An event has the same `id` if the device sends it twice, which can happen after a lost connection.

## Availability and battery

All entities use `<base>/status` for availability. They turn unavailable when the device publishes `offline`, and when the broker sees the device lost (within about 90 seconds).

On battery the radio is on only after you turn Wi-Fi on and stops after 10 minutes without a web request or a touch, so Home Assistant sees the device as unavailable most of the time. This is expected. Reminders and logging on the device do not depend on it. Events wait in the outbox for up to 7 days and arrive the next time the device connects; the state topics refresh within seconds of a connect.
