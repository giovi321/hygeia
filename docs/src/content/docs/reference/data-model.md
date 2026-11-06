---
title: Data model
description: Medicines, log entries, settings and how they are stored.
---

## Medicine

| Field | Notes |
|---|---|
| `id` | stable integer, never reused |
| `code` | 1 or 2 characters, A to Z or 0 to 9, shown on the tile, unique among active medicines |
| `name` | text, up to 32 characters |
| `dose` | text, up to 16 characters, for example "1 tab" |
| `color` | index 0 to 9 into a fixed palette of 10 colours chosen to read on black |
| schedule type | `fixed` or `as_needed` |
| times | up to 8 local times per day, stored as minutes after local midnight (fixed only) |
| weekdays | bitmask, bit 0 Monday to bit 6 Sunday; default all days |
| `min_interval_min` | as-needed only: warn when logging sooner than this |
| `early_window_min` | default 60: how long before an occurrence a dose counts for it |
| `late_after_min` | default 30 |
| `missed_after_min` | default 120 |
| pills | optional: enabled, remaining, alert threshold; decremented on each taken log |
| `active_from`, `active_until` | optional local dates (YYYYMMDD) for a course of treatment |
| `archived` | hidden from the device, kept for history |
| `order` | tile position |

Validation checks, in this order: code format, name, colour, schedule times, code uniqueness. At most 50 medicines are allowed.

The code is for display. Identifiers elsewhere use `id`.

## Log entry

| Field | Notes |
|---|---|
| `id` | monotonic |
| `ts` | UTC epoch seconds when the dose was taken, skipped or declared missed |
| `tz` | the POSIX time zone in force at that moment, so history keeps the local time it happened |
| medicine | the medicine `id` |
| `status` | `taken`, `skipped` or `missed` |
| `slot` | the scheduled occurrence (UTC epoch) the entry satisfies; 0 for a free log |
| `delta_min` | minutes late (positive) or early (negative) relative to the slot |
| `note` | text, up to 63 characters |
| `source` | `screen`, `web`, `mqtt` or `auto` (used for missed entries) |
| `edited` | set when the entry was changed after creation |

Snoozes are scheduler state, not log entries. Only `screen` and `auto` sources are produced today.

## Settings

Stored in NVS:

| Setting | Default | Range |
|---|---|---|
| sound | on | on or off |
| do not disturb | off | on or off |
| volume | 60 | 0 to 100 |
| brightness | 50 | 10 to 100 |
| snooze | 15 min | 1 to 120 |
| rot_lock | 0 | 0 or 1 (1 keeps the current orientation) |
| double_tap_wake | on | on or off (off holds the touch controller in reset while the screen is off); key `wake_dtap`, absent reads as on |
| rot | 0 | 0 to 3: the orientation in force, 0, 90, 180 or 270 degrees clockwise; an out-of-range value reads as 0 |

The settings are in NVS namespace `settings`.

### Integration members

The settings JSON also carries the integration settings, in two members next to the ones above.

`mqtt`:

| Member | Default | Rule |
|---|---|---|
| `on` | false | true needs a valid `url` |
| `url` | empty | `mqtt://host[:port]` (port 1883) or `mqtts://host[:port]` (port 8883), no path. An off broker may keep an empty url |
| `user` | empty | up to 64 bytes of valid UTF-8, no control characters |
| `password` | empty | same rule as `user`. Write only: the settings object shows `password_set` (true or false) instead |
| `base` | `hygeia` | topic prefix, 1 to 32 characters of letters, digits, `_`, `-` and `/`; not starting or ending with `/`, no `//` |
| `ha` | false | Home Assistant discovery on or off |
| `ha_prefix` | `homeassistant` | same rule as `base` |

`webhooks` is a list of 0 to 4 items. Each item:

| Member | Default | Rule |
|---|---|---|
| `url` | required | `http://` or `https://` (ports 80 and 443), host, optional port, optional path starting with `/`. Unique in the list; the scheme and host compare without regard to case |
| `on` | true | this webhook is active |
| `events` | every type | a list of `taken`, `skipped`, `missed`, `due`, `snoozed`, `undone`, `battery_low`, `battery_critical`, `clock_invalid`. An empty list sends nothing |
| `secret` | empty | up to 64 bytes of valid UTF-8, no control characters. Write only: the settings object shows `secret_set` instead |

A host is 1 to 64 letters, digits, dots and hyphens (a dotted IPv4 address qualifies) and does not start or end with `.` or `-`. A url has no user name, no `#`, no spaces or control characters and at most 255 bytes.

A `PUT` with a `webhooks` list replaces the whole list. A secret stays with its position in the list: if the item is sent without `secret`, it keeps the secret of the stored webhook at the same position, also when its url changed. `"secret": ""` clears it, a string sets it. An item added at the end starts with no secret. For `mqtt`, a member that is absent keeps its value, and `"password": ""` clears the password. `password_set` and `secret_set` in a request are ignored.

The password and the webhook secrets are never returned by the API, written to the log or shown in a status. A settings export contains them only when requested with `secrets=1`.

The time zone (IANA name and POSIX rule) is also stored in NVS, namespace `clock`.

## Storage

- Medicines: one JSON file, `meds.json`, in the LittleFS storage partition mounted at `/data`. A file larger than 256 KB is treated as corrupt
- Log: append-only JSON Lines, one file per month, `log/YYYY-MM.jsonl`. Edits rewrite the month file. Corrupt lines are dropped by a rewrite and counted
- Settings and the time zone: NVS

There is no export or import yet. A retention policy that drops old months when the partition fills is in the design but is not implemented.
