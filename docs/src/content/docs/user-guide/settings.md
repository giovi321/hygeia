---
title: Quick settings and settings
description: The pull-down quick settings and the Settings pages.
---

## Quick settings

![Design mockup of quick settings](/hygeia/assets/mockups/02-quick-settings.png)

Pull down from the top edge on any screen to open quick settings. Swipe up to close.

| Control | State today |
|---|---|
| Wi-Fi tile | tap to switch between Wi-Fi client and off; the line under it reads Off, the network name, Connecting, Can't connect or Off on battery |
| Access point tile | tap to start the access point and show the QR codes to join it, tap again to stop it; the line reads Off or On |
| Sound tile | toggles the chime; the tile reads "Chime on" or "Chime off" |
| Do not disturb tile | toggles do not disturb; the tile reads On or Off |
| Brightness slider | sets the panel brightness, 10 to 100 percent |
| Battery line | shows only the estimate, for example "~2 d left", not the percentage; hidden on USB power and until enough discharge data exists |
| Settings button | opens Settings |
| Web UI QR button | shows a QR code and the address of the web UI, or the Wi-Fi and access point buttons when no network is up |

Do not disturb keeps the due card but silences the chime.

The Wi-Fi and Access point tiles are explained in [Wi-Fi setup](../../getting-started/wifi/). The same settings, and more, are in the [web UI](../web-ui/).

Changes are saved to NVS and survive a restart. Defaults: sound on, do not disturb off, volume 60, brightness 50, snooze 15 minutes.

## Settings

Settings has these entries:

- Time zone: two rollers, region and city, from a table of time zones built into the firmware, so the zone can change with no network. Set stores the zone and its POSIX rule
- Date and time: the same rollers as the "Set the time" screen
- Sound: a volume slider and a Play chime button
- Display: brightness, and Rotation lock under the brightness slider
- Wi-Fi: the mode (Off, Wi-Fi, Access point), the saved networks (tap one to connect to it), the access point name and password, the web UI address, the number of paired browsers with Forget all, and Keep Wi-Fi on battery. Networks are added only in the web UI
- About: version, clock sync state, storage used and free, and the number of active and archived medicines

The Time zone page changes the zone for future occurrences. Scheduled times keep their wall-clock time in the new zone. See [Scheduling rules](../../reference/scheduling/).

## Rotation lock

The screen turns to follow the device. Rotation lock, in Settings under Display, stops that.

- On: the screen keeps the orientation it shows at that moment, also after a restart
- Off: the screen turns again once the device has stayed still in a new position for about 1 second

Rotation lock is not in quick settings. Defaults: Rotation lock off, orientation upright.
