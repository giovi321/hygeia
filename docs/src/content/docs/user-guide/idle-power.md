---
title: Idle clock and power
description: The dimmed idle clock, screen off and sleep, the PWR key, the battery and the rule that only the PWR key powers the device on.
---

## Idle clock

![Design mockup of the idle clock](/hygeia/assets/mockups/08-idle-dimmed.png)

After 30 seconds without a touch, and with no due card showing, a black overlay covers the screen and brightness fades down to at most 20 percent (or to the set brightness, if that is lower). The overlay shows:

- The time in large digits
- "Next: V at 08:30", naming the next scheduled dose, or "No doses scheduled", or "Set the time" while the clock is invalid
- A status line with the battery percentage and "Wi-Fi off"

The whole group moves by a few pixels each minute to limit burn-in on the AMOLED panel.

Any touch only wakes the screen; it is not passed to the screen underneath. A due card wakes the screen by itself.

## Motion sensor

The motion sensor is read only while the screen is on or dimmed. The dimmed clock turns with the device too, so on USB power the clock follows the device. The sensor is powered down while the screen is off, so it costs nothing then. It does not wake the screen: turning or shaking a sleeping device does nothing.

## Screen off and sleep

- On USB power the screen dims after 30 seconds and stays dimmed. A due card on USB keeps the screen on
- On battery the screen turns off 2 minutes after the last activity and the CPU enters light sleep. A touch, the RTC alarm for the next dose, the PWR key, the user key or plugging in USB wakes it. A due card wakes the screen. A repeat chime on battery does not turn the screen on
- Unplugging USB starts the 2 minute timer from the unplug

## PWR key

- Short press: turns the screen on or off
- Long press, about 2 seconds: opens the power-off confirmation. It shows the next dose and, when a dose is due within 24 hours or open now, the warning "Reminders will not fire while off". Slide the knob to power off through the AXP2101. Cancel, a short press of PWR or 30 seconds without a touch close it
- Pressing PWR while the device is off turns it on

While the device is off, reminders do not fire. Doses that fall in that gap are recorded as untracked, not missed.

## Battery

The top bar shows the battery percentage. It turns red below 15%. A bolt next to it shows USB power with a battery connected. Quick settings shows an estimate such as "~2 d left" once enough discharge data exists (see [Quick settings and settings](../settings/)).

- Low, at 15%: a toast "Battery low" with the percentage. It waits while the Undo toast or the power-off screen shows
- Critical, at 5%: a full-screen "Battery critical" page with "Powering off in 60 s" counting down and the hint "Plug in USB to keep it on". It opens only after two critical readings 10 seconds apart, with a low battery voltage and no USB, so a bad reading does not power the device off. Plugging in USB cancels it. At zero the device powers off. A short press of PWR still turns the screen off and on while it counts

Both levels re-arm after the battery recovers, or when USB is plugged in.

## PWR-only power-on

The device must start only when you press the PWR key. Plugging in USB or connecting the battery must not leave it running.

Early in boot, before the display starts, the firmware reads the AXP2101 power-on status register (0x20). The rules:

- Only a power-on reset is checked. A software restart, a panic, a watchdog reset and a flash or RTS reset keep running
- If the power-on reset came from the PWR key (bit 0), the device boots
- If it came from USB insertion or battery insertion, the firmware powers off through the AXP2101. The battery still charges while off
- The gate fails open: a power-on reset with no known source bit, or with EN mode set, boots. If the chip id register does not read 0x4A, the gate is skipped

Verified on the board: after a power-off, replugging USB leaves the screen dark with the charge LED on, and a PWR press boots. The BOOT bypass was not tested.

To bypass the gate, press and hold BOOT right after pressing PWR or plugging in, until the log shows `gate bypassed (BOOT held)`. To reflash a device that keeps powering off, hold BOOT before the reset to enter ROM download mode.
