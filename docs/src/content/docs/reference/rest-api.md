---
title: REST API
description: The HTTP API of the device, pairing, errors, versioning, curl examples and the screen and web parity table.
---

The device serves a REST API and the web UI over plain HTTP on port 80, in Wi-Fi client mode and in access point mode. The web UI uses nothing else, so everything it does can be done with `curl`.

:::caution[Keep the device on a trusted LAN]
The web UI is plain HTTP on the local network by design. It is protected by pairing, the `X-Hygeia` header and the host allow-list, not by encryption. Do not forward port 80 to the internet. Join it from the home network or from its own access point only. The firmware download from GitHub is outbound and uses HTTPS.
:::

See also the [security notes](../security/).

## Host names

The device answers a request only when the `Host` header names the device. Any other name gets `421` and the error `host`, which blocks DNS rebinding. In access point mode the device is a captive portal instead: a request for another name, and any unknown path, gets `302` to `http://192.168.4.1/`. The device answers to:

- its IPv4 address, for example `192.168.1.23` (or `192.168.4.1` in access point mode)
- `<host>`, the host name, `hygeia` by default
- `<host>.local`, answered over mDNS in Wi-Fi client mode
- any name whose first label is `<host>`, for example `hygeia.lan`

IPv6 literals are refused. Change the host name in the web UI under Settings, Wi-Fi.

## Pairing

No route except pairing and `GET /api/info` answers an unpaired browser: they get `401` and the error `unpaired`. A browser pairs once, with a 6-digit code shown on the device:

1. `POST /api/pair/start` makes the device show a code on its screen. The code is valid for 120 seconds. A start answers `429`, error `too_many`, with `Retry-After`, and keeps any running code when it comes within 10 seconds of the previous start, within 60 seconds of a Cancel on the device or of a code that expired unused, or as the fourth start within 10 minutes. While a reminder or the battery power-off countdown is on the screen a start answers `409`, error `busy`, and no code runs
2. `POST /api/pair` with `{"code":"123456"}` checks the code. A right code sets the cookie `hy_s` (64 hex characters, `HttpOnly`, `SameSite=Strict`) and answers `{"paired":true}`
3. Five wrong codes stop pairing for 60 seconds (`429`, error `blocked`, with `Retry-After`)

The device keeps at most 4 paired browsers and forgets the oldest when a fifth pairs. `GET /api/browsers` lists them and `DELETE /api/browsers` with `{"index":n}` or `{"all":true}` forgets one or all. Forgetting is also possible on the device, under Settings, Wi-Fi.

## Requests

- Every request that is not a `GET` needs the header `X-Hygeia: 1`, else `403` with the error `forbidden`
- When a request carries an `Origin` header, it has to be `http://` plus the same host as the `Host` header, else `403`
- Bodies are JSON (`Content-Type: application/json`) up to 16 KB. `POST /api/import` takes a file up to 4 MB and `POST /api/ota` a firmware image up to 3 MB, both as the raw body
- Times are Unix seconds in UTC. The medicine and log objects carry the wall-clock strings of the device zone where a person reads them
- The device serves few requests at once and does the work of a request one step at a time. It answers `503` with the error `busy` and `Retry-After: 2` when it cannot take a request. A write that got `503` may still have been applied: read the data again before repeating it

## Errors

Every error has this shape:

```json
{"error": "invalid", "field": "ssid", "message": "The name is 1 to 32 characters"}
```

`error` is a short code, `field` the name of the offending field or `null`, `message` text for a person. Clients branch on `error` and the HTTP status, not on `message`.

| Status | Codes |
|---|---|
| 400 | `invalid` (with `field`) |
| 401 | `unpaired` |
| 403 | `forbidden` |
| 404 | `not_found` |
| 408 | `timeout` |
| 409 | `busy` (also pairing while a reminder shows), `battery`, `confirm`, `full`, `changed`, `code_dup`, `clock_invalid` (the clock is not set), `too_soon` (a dose within the minimum interval), `no_network` (a firmware check or install without a network), `stale` (a firmware install of a version that is no longer the checked one) |
| 412, 428 | `stale`, `precondition_required` (a medicine is written with `If-Match` and the `etag` of the last read) |
| 410 | `undo_expired` |
| 413 | `too_large` |
| 421 | `host` |
| 422 | the medicine rules `code`, `name`, `dose`, `times` and `interval` (each names its `field`); `not_hygeia` and `corrupt` for a bad firmware image |
| 429 | `blocked`, `too_many` |
| 500 | `internal`, `flash` (a firmware write failed) |
| 503 | `busy`, `storage`, `memory` (not enough internal memory for a firmware check or install) |

## Version rule

The path has no version. `GET /api/info` and `GET /api/state` carry `"api": 1`. Adding a field does not change that number. Removing a field or changing its meaning does. A client ignores fields it does not know and checks `api` before it relies on a field that may change.

## Live events

`/ws` is a WebSocket for paired browsers. It sends one JSON text frame per event, for example `{"ev":"taken","ts":1793181600,"med_id":1,"log_id":12,"slot":1793181000,"value":0}`. The events are `taken`, `skipped`, `missed`, `due`, `snoozed`, `undone`, `log_edited`, `meds_changed`, `untracked`, `battery_low`, `battery_critical` and `clock_invalid`. The frame `{"ev":"resync"}` means events were dropped: read the data again. The web UI refetches the page it shows on every frame, except the update frames and the cabinet frame below.

During a firmware update from GitHub the device sends `{"ev":"update","state":"downloading","pct":42}` on every state change and on each 5 percent of the download. `state` is one of `checking`, `up_to_date`, `available`, `downloading`, `verifying`, `restarting` and `failed`; a failed frame adds `"error":"<code>"`. Read `GET /api/update` for the rest.

While the Cabinet open test runs, every counted movement sends `{"ev":"cabinet","test":{"matches":4,"others":1},"last":{"dist":0.41,"thr":0.72,"match":true}}`. `test.matches` and `test.others` are the counts of the test so far; `last` describes the movement just counted: `dist` the distance to the taught movement (`null` when the movement was rejected before the comparison), `thr` the threshold (`null` when there is none), `match` whether it counted as a match. The web UI does not refetch on this frame.

## Routes

| Route | Use |
|---|---|
| `GET /api/info` | name, firmware version, `api`, clock state, `paired`, `repo` (the GitHub repository the device updates from) (open) |
| `POST /api/pair/start`, `POST /api/pair` | pairing (open) |
| `GET /api/browsers`, `DELETE /api/browsers` | list and forget paired browsers |
| `GET /api/state` | everything the dashboard needs |
| `GET /api/medicines`, `POST /api/medicines` | list and add medicines |
| `GET /api/medicines/{id}`, `PUT /api/medicines/{id}` | read and change one (`If-Match` on write) |
| `DELETE /api/medicines/{id}` | delete an archived medicine and its log; without `?confirm=1` the device answers `409` and the number of entries |
| `POST /api/medicines/{id}/archive`, `/unarchive` | archive or bring back |
| `PUT /api/medicines/order` | reorder: `{"ids":[...]}` with every active id once |
| `POST /api/medicines/{id}/pills` | set or add to the pill count: `{"left":n}` or `{"add":n}` |
| `POST /api/medicines/{id}/log`, `/skip`, `/snooze` | the due-card actions; log takes optional `ts` and `note` |
| `GET /api/log?from=&to=&med=`, `POST /api/log` | read the log (`limit` and `after`; the answer carries `next`) and add a past dose |
| `PUT /api/log/{id}`, `DELETE /api/log/{id}` | edit or delete an entry (`orig_ts` or `ts` identify it) |
| `POST /api/log/{id}/undo` | undo within 5 seconds of logging |
| `GET /api/history?days=7` or `30` | the daily grid per medicine |
| `GET /api/schedule?from=&to=` | occurrences in a time range, at most 8 days |
| `GET /api/settings`, `PUT /api/settings` | settings; a key that is not sent stays as it is, and the answer holds the values the device kept. `double_tap_wake` (boolean, default `true`): `false` holds the touch controller in reset while the screen is off, so a touch does not wake it. `wake_single_tap` (boolean, default `false`) applies only while `double_tap_wake` is `true`. A value that is not a boolean gives `400 invalid` with the field name |
| `POST /api/time` | set the clock and the zone from a browser |
| `GET /api/wifi`, `POST /api/wifi/mode` | state, saved networks, mode (`off`, `client`, `ap`) |
| `POST /api/wifi/networks`, `DELETE /api/wifi/networks` | save or forget a network (at most 5) |
| `PUT /api/wifi/ap` | access point password and host name |
| `GET /api/export.csv`, `GET /api/export.json`, `POST /api/import` | export and import |
| `POST /api/chime/test`, `POST /api/power/off` | play the chime, power off |
| `POST /api/ota` | firmware update from an uploaded file |
| `GET /api/update`, `POST /api/update/check`, `POST /api/update/install` | firmware update from GitHub |
| `GET /api/cabinet` | Cabinet open state: `taught`, `taught_at`, `mode`, `live`, `testing`, `teaching`, `checking`, `threshold`, `trigger_mg`, `counts`, `test` and the `recent` Taught captures. `teaching` is true while the movement is being taught on the device, `checking` while the taught movement is being tested there (the Test button after teaching) |
| `POST /api/cabinet/test` | start or stop the test: `{"on":true}` or `{"on":false}`; `409` with `teaching`, `checking`, `not_taught` or `screen_off` when it cannot start |
| `GET /api/cabinet/samples.csv` | the taught takes and the recent Taught captures as CSV |
| `DELETE /api/cabinet/teach` | forget the taught movement and, when the trigger was Taught movement, set it back to Any movement; `409` with `teaching` or `checking` while the movement is being taught or checked |

Passwords are write-only. No route returns a Wi-Fi password, except `GET /api/export.json?secrets=1`, which writes the saved passwords into the file.

### Export and import

- `GET /api/export.csv` is the log as a spreadsheet file. A text cell that starts with `=`, `+`, `-` or `@` gets a leading `'` so a spreadsheet does not run it as a formula
- `GET /api/export.json` holds `meds`, `log`, `gaps` and `settings`. `?secrets=1` adds the saved Wi-Fi networks with their passwords and the access point password; `?secrets=0` is the default
- `POST /api/import?mode=merge|replace` takes that JSON as the body. `&preview=1` counts what would change and changes nothing: new, changed and unchanged medicines, new and duplicate log entries, new untracked periods. `&settings=1` also applies the settings and Wi-Fi networks of the file
- A merge adds what is missing. A replace swaps in the whole file and keeps the replaced data in a backup folder on the device until the next import. If the device data changes while a replace runs (a dose is logged), the replace answers `409` with the error `changed` and replaces nothing: preview and import again
- An export or import runs in the background. While one runs, a second one answers `409` with the error `busy`. A merge that stops partway says so in the message; importing the same file again reports the added entries as duplicates

### Firmware

`POST /api/ota` takes the firmware file `hygeia.bin` as the raw body and answers `{"reboot_in_s":2}`. The device checks the first bytes of the file, writes it to the other update slot and restarts two seconds later. Errors: `422` `not_hygeia` (not a Hygeia image), `422` `corrupt`, `413` (above 3 MB), `409` `battery` (on battery below 30 percent), `409` `busy` (another export, import or update runs, or a restart is pending), `503` `busy` (the running firmware has not confirmed itself yet, retry in a few seconds).

### Firmware update from GitHub

- `GET /api/update` answers the status: `{"state":"available","installed":"dev-a88c56d","latest":"v0.5.0","installed_release":false,"pct":0,"error":null,"message":null,"checked_ago_s":12}`. `latest` is empty and `checked_ago_s` is `null` before the first check. `installed_release` is false for a development build
- `POST /api/update/check` starts a check and answers `202` with the status. The device sends one request to github.com; poll `GET /api/update` or watch the live events for the result
- `POST /api/update/install` takes `{"version":"v0.5.0"}`, which has to be the `latest` of a check made less than 10 minutes ago, and answers `202` with the status. The device then downloads, verifies and restarts. A missing or non-string `version` answers `400` with the field `version`

Errors use the usual shape. A status in the `failed` state carries the same `error` and `message`.

| Status | `error` | Meaning |
|---|---|---|
| `409` | `no_network` | not connected to a Wi-Fi network (access point mode, Wi-Fi off or no link) |
| `409` | `busy` | a check, install, upload, export or import runs |
| `409` | `stale` | install without a recent check, or `version` is not the checked `latest` |
| `409` | `battery` | on battery below 30 percent (install only) |
| `503` | `busy` | the running firmware has not confirmed itself yet; `Retry-After: 2` |
| `503` | `memory` | not enough memory right now; `Retry-After: 2` |

Failures after the start show in the status: `no_internet` (GitHub cannot be reached), `tls`, `no_release`, `bad_release`, `mismatch`, `checksum`, `corrupt`, `flash` and `timeout`. Nothing is installed in any of these cases.

The new firmware has to confirm itself on its first scheduler pass. If it does not, the device restarts into the previous version after 150 seconds. The first install of a firmware with this feature has to be done by USB: see [Build and flash](../../getting-started/flashing/).

## curl examples

The examples use the device at `192.168.1.23`, the network `HomeNet` and a cookie placeholder. Replace `<64 hex characters>` with the value of `hy_s` from the pairing answer.

Pair a command-line client:

```bash
curl -s -X POST -H "X-Hygeia: 1" http://192.168.1.23/api/pair/start
# read the 6-digit code on the device screen
curl -s -i -X POST -H "X-Hygeia: 1" -H "Content-Type: application/json" \
  -d '{"code":"123456"}' http://192.168.1.23/api/pair
# the answer has a header: Set-Cookie: hy_s=<64 hex characters>; Path=/; HttpOnly; SameSite=Strict
```

Read data (a `GET` needs the cookie only):

```bash
C='hy_s=<64 hex characters>'
curl -s -b "$C" http://192.168.1.23/api/info
curl -s -b "$C" http://192.168.1.23/api/medicines
curl -s -b "$C" "http://192.168.1.23/api/history?days=30"
```

Log a dose with a note. The second call names a medicine that does not exist and fails with `404` and the error shape:

```bash
curl -s -b "$C" -X POST -H "X-Hygeia: 1" -H "Content-Type: application/json" \
  -d '{"note":"with food"}' http://192.168.1.23/api/medicines/1/log
curl -s -b "$C" -X POST -H "X-Hygeia: 1" -H "Content-Type: application/json" \
  -d '{}' http://192.168.1.23/api/medicines/99/log
```

Save a Wi-Fi network and switch the device to it:

```bash
curl -s -b "$C" -X POST -H "X-Hygeia: 1" -H "Content-Type: application/json" \
  -d '{"ssid":"HomeNet","password":"<password>"}' http://192.168.1.23/api/wifi/networks
curl -s -b "$C" -X POST -H "X-Hygeia: 1" -H "Content-Type: application/json" \
  -d '{"mode":"client","ssid":"HomeNet"}' http://192.168.1.23/api/wifi/mode
```

Back up, preview an import and import:

```bash
curl -s -b "$C" -OJ http://192.168.1.23/api/export.json
curl -s -b "$C" -X POST -H "X-Hygeia: 1" --data-binary @hygeia-20261031.json \
  "http://192.168.1.23/api/import?mode=merge&preview=1"
curl -s -b "$C" -X POST -H "X-Hygeia: 1" --data-binary @hygeia-20261031.json \
  "http://192.168.1.23/api/import?mode=merge"
```

Update the firmware:

```bash
curl -s -b "$C" -X POST -H "X-Hygeia: 1" -H "Content-Type: application/octet-stream" \
  --data-binary @build/hygeia.bin http://192.168.1.23/api/ota
```

Check for a release on GitHub and install it:

```bash
curl -s -b "$C" -X POST -H "X-Hygeia: 1" http://192.168.1.23/api/update/check
curl -s -b "$C" http://192.168.1.23/api/update
curl -s -b "$C" -X POST -H "X-Hygeia: 1" -H "Content-Type: application/json" \
  -d '{"version":"v0.5.0"}' http://192.168.1.23/api/update/install
```

## Screen and web parity

Every action on the screen has a route. The table lists each route, the web UI page that uses it and the place on the screen that does the same. "web only" means the screen has no equivalent. The table is generated from the route table in the firmware source by `tools/gen_parity.py`; do not edit it by hand.

<!-- parity:start -->
| Route | Web page | Screen |
|---|---|---|
| `GET /api/info` | Every page (version, pairing state), Firmware | Settings > About |
| `GET /api/state` | Dashboard | Home tile |
| `POST /api/pair/start` | Pair this browser | Pairing code overlay |
| `POST /api/pair` | Pair this browser | web only |
| `GET /api/browsers` | Settings | Settings > Wi-Fi |
| `DELETE /api/browsers` | Settings | Settings > Wi-Fi (Forget all) |
| `GET /api/medicines` | Medicines | Home |
| `POST /api/medicines` | Medicines | Medicine form |
| `GET /api/medicines/{id}` | Medicines | Medicine detail |
| `PUT /api/medicines/{id}` | Medicines | Medicine form |
| `DELETE /api/medicines/{id}` | Medicines | web only |
| `POST /api/medicines/{id}/archive` | Medicines | Medicine detail |
| `POST /api/medicines/{id}/unarchive` | Medicines | web only |
| `PUT /api/medicines/order` | Medicines | web only |
| `POST /api/medicines/{id}/pills` | Medicines | web only |
| `POST /api/medicines/{id}/log` | Dashboard | Home tile, due card, Medicine detail |
| `POST /api/medicines/{id}/skip` | Dashboard | Due card |
| `POST /api/medicines/{id}/snooze` | Dashboard | Due card |
| `GET /api/log` | Log | Medicine detail, History |
| `POST /api/log` | Log | Medicine detail > Log earlier |
| `PUT /api/log/{id}` | Log | web only |
| `DELETE /api/log/{id}` | Log | web only |
| `POST /api/log/{id}/undo` | Dashboard | Logged toast |
| `GET /api/history` | History | History |
| `GET /api/schedule` | Schedule | Overview |
| `GET /api/export.csv` | Backup | web only |
| `GET /api/export.json` | Backup | web only |
| `POST /api/import` | Backup | web only |
| `GET /api/settings` | Settings, Wi-Fi | Settings, quick settings |
| `PUT /api/settings` | Settings, Wi-Fi | Settings, quick settings |
| `POST /api/time` | Settings | Settings > Time zone, Date and time |
| `GET /api/wifi` | Wi-Fi | Settings > Wi-Fi |
| `POST /api/wifi/mode` | Wi-Fi | Quick settings, Settings > Wi-Fi |
| `POST /api/wifi/networks` | Wi-Fi | web only |
| `DELETE /api/wifi/networks` | Wi-Fi | web only |
| `PUT /api/wifi/ap` | Wi-Fi | web only |
| `POST /api/chime/test` | Settings | Settings > Sound |
| `POST /api/power/off` | Settings | Power off screen |
| `POST /api/ota` | Firmware | web only |
| `GET /api/update` | Firmware | Settings > About > Check for update |
| `POST /api/update/check` | Firmware | Settings > About > Check for update |
| `POST /api/update/install` | Firmware | Settings > About > Check for update |
| `GET /api/cabinet` | Cabinet open | Settings > Cabinet open |
| `POST /api/cabinet/test` | Cabinet open | Settings > Cabinet open > Test |
| `GET /api/cabinet/samples.csv` | Cabinet open | web only |
| `DELETE /api/cabinet/teach` | Cabinet open | Settings > Cabinet open > Forget the movement |
<!-- parity:end -->
