---
title: Webhooks
description: Send dose and device events to up to four HTTP receivers, with signatures, retries and a test button.
---

A webhook sends each event as an HTTP POST with a JSON body to a URL you choose, for example an n8n workflow. Reminders and logging never depend on it: a receiver that is down, slow or wrong only delays its own events.

## Setup

In the web UI, open Settings, Webhooks:

- Add up to 4 webhooks. Each has a URL (`http://` or `https://`), an on switch, a choice of events (every type by default) and an optional secret
- A webhook keeps its row. Removing one never moves the others
- The secret is write-only: the page shows only whether one is set. Leave the field empty to keep it, or clear it to remove it
- Save, then press Test on the row (see [The test button](#the-test-button))

Delivery runs only while the device is connected to a network as a client, never in access point mode. Webhooks never turn the radio on and never keep it on. On battery the radio runs only after you turn Wi-Fi on and stops after 10 minutes without a web request or a touch, so events wait in the outbox until the radio is on again. See [Idle clock and power](../../user-guide/idle-power/#the-radio-mqtt-and-webhooks).

## Events

| Event | When |
| --- | --- |
| `taken` | a dose was logged as taken |
| `skipped` | a dose was skipped |
| `missed` | a dose passed its missed time without a log |
| `due` | a dose became due |
| `snoozed` | a reminder was snoozed |
| `undone` | a log was undone within 5 s; a receiver that counts doses takes it back |
| `battery_low` | the battery fell to 15 % |
| `battery_critical` | the battery fell to 5 % |
| `clock_invalid` | the device started with an invalid clock (once per boot) |

Editing or deleting a log entry, changing medicines and untracked periods are not sent.

## Payload

```json
{"id":128,"event":"taken","ts":1762412520,"tz":"Europe/Zurich","device":"hygeia-1A2B",
 "medicine":{"id":1,"code":"A","name":"Antibiotic","dose":"1 tab"},"slot":1762412400,
 "log":{"id":42,"ts":1762412520,"tz":"CET-1CEST,M3.5.0,M10.5.0/3","med_id":1,"status":"taken","slot":1762412400,
        "delta_min":2,"source":"screen","edited":false},
 "battery":82}
```

The device sends it on one line, with no spaces, in this member order.

- `id`: a number that is unique per device and only grows. A receiver de-duplicates by it
- `event`: one of the names above
- `ts`: epoch seconds of the event, `null` while the clock is invalid. `tz`: the IANA zone name
- `device`: the device name, `hygeia-` and 4 hex digits, the same as the access point name
- `medicine`: `null` for device events. `slot`: the scheduled time of the dose (epoch seconds) or `null`
- `log`: the stored log entry, as the REST API returns it, or `null`. For `undone` it is always only `{"id":n}`, the id of the entry that was taken back
- `battery`: the battery percent, or `null` when unknown. For `battery_low` and `battery_critical` it is the reading that raised the event

## Signature

When a webhook has a secret, the request carries

```
X-Hygeia-Signature: sha256=<64 lowercase hex digits>
```

It is HMAC-SHA256 of the exact body bytes, with the secret as the key. Compute it over the raw body before any JSON parsing, and compare in constant time. The signature has no timestamp: de-duplicate by `id` to ignore a repeated request.

Python:

```python
import hashlib
import hmac

SECRET = b"example-secret"

def valid(body: bytes, header: str) -> bool:
    want = "sha256=" + hmac.new(SECRET, body, hashlib.sha256).hexdigest()
    return hmac.compare_digest(want, header or "")
```

n8n, on `n8n.lan`: a Webhook node (method POST, option Raw Body on) followed by a Code node in "Run Once for All Items" mode. The Code node needs the `crypto` module allowed (`NODE_FUNCTION_ALLOW_BUILTIN=crypto` on the n8n server).

```js
const crypto = require('crypto');
const SECRET = 'example-secret';

const raw = await this.helpers.getBinaryDataBuffer(0, 'data');
const got = $input.first().json.headers['x-hygeia-signature'] || '';
const want = 'sha256=' + crypto.createHmac('sha256', SECRET).update(raw).digest('hex');
if (got.length !== want.length || !crypto.timingSafeEqual(Buffer.from(got), Buffer.from(want))) {
  throw new Error('bad signature');
}
return [{ json: JSON.parse(raw.toString('utf8')) }];
```

## Responses

- 2xx: delivered
- 408, 429, 5xx, a timeout or a network error: tried again. After the 1st, 2nd and 3rd failure in a row the device waits 1, 5 and 30 minutes, then every 30 minutes
- 401 or 403: the webhook pauses with the status "auth". Its events are kept, but nothing is sent until you save its settings again, the device reconnects to the network, or a Test succeeds
- Any other status, including redirects (the device does not follow them): that one event is rejected for that webhook, counted as "rejected", and delivery moves on

Up to 20 events go to one webhook per pass, in the order they happened. One slow or failing webhook never holds back another webhook or MQTT.

## Delivery and expiry

- Events wait in the outbox on the device flash, so they survive a restart. The outbox holds 1000 events; when it is full the oldest waiting event is dropped for every destination and counted as "dropped when full"
- An event that is still waiting 7 days after it entered the outbox is dropped as "expired". Events stored while the clock was invalid do not expire
- Delivery is at least once: after a power cut or a crash, events delivered in the last 10 seconds can be sent again with the same `id`
- Turning a webhook off or removing it discards its waiting events. Editing its URL keeps them, so a corrected typo still gets them
- With every webhook off and MQTT off, nothing about events is written to the flash

## The test button

Test sends one event `{"id":0,"event":"test",...}` to the saved URL of that row, signed with its secret. It does not go through the outbox and does not change the retry state, and it also works while the webhook is switched off. The result (HTTP status or error) shows on the row a moment later. A successful test ends an "auth" pause. Test is disabled on a row with unsaved changes, and it needs the device connected as a client.

## TLS

`https://` receivers are checked against the certificate bundle built into the firmware (the common public certificate authorities). A receiver with a self-signed certificate or one from a private certificate authority does not work in this version, and there is no switch to skip the check. Certificate dates are not checked, because the clock can be wrong. On a home network, `http://` to a local receiver such as `http://n8n.lan:5678/webhook/hygeia` avoids the question; the body then travels unencrypted, and the signature still shows that it came from a device that knows the secret.

## Memory

A TLS session needs internal memory. The device does not try while less than 62 KB (63488 bytes) of internal memory is free for HTTPS, or 44 KB (45056 bytes) for HTTP. It then waits 60 seconds and shows the status "memory"; this does not count as a failure. With many medicines (about 30 or more active ones) the free internal memory can stay below the HTTPS limit, and HTTPS webhooks then wait until memory frees up or their events expire. Use an `http://` receiver on the local network in that case. Requests also wait, with the status "busy", while a firmware update, an upload, an export or an import runs.
