---
title: First boot
description: What the device does the first time it starts.
---

## Power-on

After flashing, the device keeps running because a flash reset is not a power-on reset. After that, the device starts only when you press the PWR key. Plugging in USB or connecting the battery leaves it off. See [Idle clock and power](../../user-guide/idle-power/).

## Splash

The splash plays for about 1 second: a capsule drops into the gap of a ring and the ring draws around it.

## Set the time

The clock is invalid when the RTC oscillator-stop flag is set or when its time is before the date the firmware was built. A new board is in this state. The device then shows the "Set the time" screen:

- Reminders are paused and logging is off until the time is set, because a dose logged with a wrong time is worse than no log
- Pick the date and time with the rollers and tap Set. Dates before the firmware build date, and dates or times that do not exist, are rejected
- The time zone line shows the current zone. Change it in Settings, under Time zone

Once the time is valid, the Home screen appears and the scheduler starts.

## Example medicines

If the device finds no medicines file in its storage, it adds three example medicines through the normal validation:

| Code | Name | Schedule |
|---|---|---|
| A | Antibiotic | fixed: 06:00, 14:00, 22:00, every day |
| V | Vitamin D | fixed: 08:30, every day |
| M | Magnesium | as needed, no minimum interval |

These are placeholders. There is no way yet to add, edit or archive medicines on the device, and the web UI that would do it is not built (see [Roadmap](../../roadmap/)). Until then, the medicines on a device are the three examples, or whatever a debug build adds through the serial console.

## Storage problems

If the storage partition cannot be mounted or read, the device shows "Storage unavailable" with the text "Medicines and the log cannot be read. Reminders are off." Nothing is logged in this state.
