---
title: Quick settings and settings
description: The pull-down quick settings and the Settings pages.
---

## Quick settings

![Design mockup of quick settings](/hygeia/assets/mockups/02-quick-settings.png)

Pull down from the top edge on any screen to open quick settings. Swipe up to close.

| Control | State today |
|---|---|
| Wi-Fi tile | shows "Not available yet" |
| Access point tile | shows "Not available yet" |
| Sound tile | toggles the chime; the tile reads "Chime on" or "Chime off" |
| Do not disturb tile | toggles do not disturb; the tile reads On or Off |
| Brightness slider | sets the panel brightness, 10 to 100 percent |
| Battery line | shows only the estimate, for example "~2 d left", not the percentage; hidden on USB power and until enough discharge data exists |
| Settings button | opens Settings |
| Web UI QR button | shows "Not available yet" |

Do not disturb keeps the due card but silences the chime.

Changes are saved to NVS and survive a restart. Defaults: sound on, do not disturb off, volume 60, brightness 50, snooze 15 minutes.

## Settings

Settings has these entries:

- Time zone: two rollers, region and city, from a table of time zones built into the firmware, so the zone can change with no network. Set stores the zone and its POSIX rule
- Date and time: the same rollers as the "Set the time" screen
- Sound: a volume slider and a Play chime button
- Display: brightness
- Wi-Fi: not available yet
- About: version, clock sync state, storage used and free, and the number of active and archived medicines

The Time zone page changes the zone for future occurrences. Scheduled times keep their wall-clock time in the new zone. See [Scheduling rules](../../reference/scheduling/).
