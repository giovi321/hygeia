---
title: Scheduling rules
description: How occurrences, states, windows, time zones and untracked periods work.
---

The scheduler decides which doses are open, which become missed, when the chime plays and when to evaluate next. It uses no network and no storage of its own; it works from the medicines, the log, the snoozes and the clock.

## Occurrences

- Only fixed, non-archived medicines have occurrences. As-needed and archived medicines have none
- Occurrences follow the local time of the current time zone, the weekday mask and the active dates
- A local time that does not exist (spring-forward gap) is scheduled at the first valid minute after the gap. A time that occurs twice (fall-back) is scheduled once, at its first occurrence
- Daylight saving uses POSIX time zone rules from a table built into the firmware

## States

An occurrence has one state at a time. A log decides first:

1. Taken, if a taken log exists for the slot; else skipped, if a skipped log exists; else missed, if a missed log exists
2. Untracked, if the slot lies in a gap and its window closed before the gap ended
3. Otherwise by time from the slot: upcoming before it, due from it, late from slot plus `late_after_min`, missed from slot plus `missed_after_min`

A dose that is due or late is open. Per medicine, only the earliest open occurrence within the last 8 days is shown. At most 16 doses are open at once.

## Dose window

A log satisfies an occurrence when its time falls from `early_window_min` before the occurrence until the next occurrence of the same medicine. It satisfies the earliest unsatisfied occurrence, and `delta_min` is negative for an early dose. A log outside any window is a free log.

Exception: if the earliest unsatisfied occurrence is already past its missed limit and the log falls in the early window of a later one, the later one wins. A dose taken early for the next slot is not credited to a missed one.

## Missed and taken after missed

When an occurrence becomes missed, the device writes a missed entry with source `auto`. If a dose is logged after that, and before the next occurrence, the missed entry becomes a taken entry with the real time and lateness.

## Snooze

Snooze defaults to 15 minutes. Snoozing hides the due card until the snooze ends. It does not extend the missed limit.

## As-needed medicines

Logging sooner than `min_interval_min` after the last taken dose shows a warning with the time since that dose. A second tap within 5 seconds logs it anyway. A minimum interval of 0 never warns.

## Chime

A chime plays when a dose newly opens, then every 120 seconds while any open dose is unsnoozed, up to 5 plays per sequence. Do not disturb and Sound off silence it; see [Due card and reminders](../../user-guide/due-card/).

## Time zone changes

Changing the zone keeps the same wall-clock times in the new zone: 08:00 stays 08:00. Past log entries keep the zone they were logged in.

- Moving east: a dose whose local time falls inside the skipped hours becomes due on arrival and is never missed
- Moving west: a dose already satisfied for that local date and wall-clock time is not raised again when the same local time comes round a second time
- The same rules apply to daylight saving changes

## Untracked periods

While the clock is invalid, tracking stops: no reminders, and no missed entries. When tracking restarts, each medicine restarts at the current time, and slots whose window closed before that point are untracked, not missed. History shows them as untracked and adherence excludes them. After any clock jump, tracking restarts the same way.

The design plans to persist untracked intervals in the store. Today, slots that fall while the device was off show as untracked and are never marked missed.

## Next wake time

The scheduler computes the earliest instant at which it must evaluate again: the next occurrence, each open dose's late and missed limits, snooze ends and the next chime. The device arms the RTC alarm for it. The PCF85063 alarm matches day of month, hour, minute and second, with no month field, so a target more than about one month ahead fires early. The scheduler re-arms the alarm after every evaluation.
