---
title: Due card and reminders
description: The full-screen card shown when a dose is due, the chime, snooze and skip.
---

![Design mockup of the due card](/hygeia/assets/mockups/06-dose-due.png)

## When the card appears

When a scheduled dose becomes due, the screen wakes if it was dimmed and shows a full-screen card with the medicine code, name and dose and the line "Due 14:00". When the dose is past its late limit the line reads "Late · due 14:00". The card stays until the dose is taken, skipped or snoozed.

If several doses are due at once, the card shows the first and adds "+2 more". Swipe up, or swipe up from the bottom edge, to step through them. Stepping only changes which dose is shown.

## Actions

| Action | Effect |
|---|---|
| Slide the knob right ("Slide to log as taken") | logs the dose as taken |
| Swipe left ("swipe left: snooze 15 min") | snoozes the card |
| Tap Skip | logs the dose as skipped |

After a taken or skipped log, a toast with a 5 second undo countdown appears. Snoozing hides the card and shows "Snoozed until HH:MM" on the tile. Snooze length is 15 minutes by default. The code clamps the setting to 1 to 120 minutes; no screen changes it yet.

## Chime

The chime is three short tones. It plays when a dose becomes due, then every 2 minutes while the dose stays open and unsnoozed, up to 5 plays in total per sequence. A new dose that becomes due starts a new sequence.

The chime is silent when Sound is off or when Do not disturb is on. The card still appears in both cases. If the speaker fails to start, the screen flashes instead.

## Late and missed

- A dose becomes late 30 minutes after its time by default, and missed 120 minutes after it
- When a dose becomes missed, the device writes a missed entry to the log and the card goes away
- A dose logged after it was marked missed, and before the next occurrence, turns the missed entry into a taken entry with the real time

Snoozing does not move the missed limit. See [Scheduling rules](../../reference/scheduling/) for the exact rules.

## What reminders need

A reminder needs a valid clock and a mounted storage partition. It does not need Wi-Fi or any other network. While the clock is invalid, reminders are paused. While the device is off, flat or the clock is invalid, doses that fall in that gap are recorded as untracked instead of missed.
