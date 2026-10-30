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

The time zone (IANA name and POSIX rule) is also stored in NVS, namespace `clock`.

## Storage

- Medicines: one JSON file, `meds.json`, in the LittleFS storage partition mounted at `/data`. A file larger than 256 KB is treated as corrupt
- Log: append-only JSON Lines, one file per month, `log/YYYY-MM.jsonl`. Edits rewrite the month file. Corrupt lines are dropped by a rewrite and counted
- Settings and the time zone: NVS

There is no export or import yet. A retention policy that drops old months when the partition fills is in the design but is not implemented.
