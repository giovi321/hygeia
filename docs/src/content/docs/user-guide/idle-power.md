---
title: Idle clock and power
description: The dimmed idle clock and the rule that only the PWR key powers the device on.
---

## Idle clock

![Design mockup of the idle clock](/hygeia/assets/mockups/08-idle-dimmed.png)

After 30 seconds without a touch, and with no due card showing, a black overlay covers the screen and brightness fades down to at most 20 percent (or to the set brightness, if that is lower). The overlay shows:

- The time in large digits
- "Next: V at 08:30", naming the next scheduled dose, or "No doses scheduled", or "Set the time" while the clock is invalid
- A status line with a dash and a percent sign for the battery, and "Wi-Fi off"

The whole group moves by a few pixels each minute to limit burn-in on the AMOLED panel.

Any touch only wakes the screen; it is not passed to the screen underneath. A due card wakes the screen by itself.

The screen never turns fully off and the CPU never enters sleep in the current firmware. Battery-saving sleep is not built.

## PWR-only power-on

The device must start only when you press the PWR key. Plugging in USB or connecting the battery must not leave it running.

Early in boot, before the display starts, the firmware reads the AXP2101 power-on status register (0x20). The rules:

- Only a power-on reset is checked. A software restart, a panic, a watchdog reset and a flash or RTS reset keep running
- If the power-on reset came from the PWR key (bit 0), the device boots
- If it came from USB insertion or battery insertion, the firmware powers off through the AXP2101. The battery still charges while off
- The gate fails open: a power-on reset with no known source bit, or with EN mode set, boots. If the chip id register does not read 0x4A, the gate is skipped

Verified by the owner on the board: after a power-off, replugging USB leaves the screen dark with the charge LED on, and a PWR press boots. The BOOT bypass was not tested.

To bypass the gate, press and hold BOOT right after pressing PWR or plugging in, until the log shows `gate bypassed (BOOT held)`. To reflash a device that keeps powering off, hold BOOT before the reset to enter ROM download mode.

Holding PWR powers the board off through the AXP2101. The firmware has no power-off confirmation screen and no warning that reminders will not fire while off; both are planned in the design but not built. While off, reminders do not fire.
