---
title: Web UI
description: The pages of the web UI, backup and restore, firmware update and what the screen does the same.
---

The device serves the web UI itself, in a browser on the same network. It edits everything the screen edits and more: every weekday, window and pill count of a medicine, the whole log, the Wi-Fi networks and backups. The page works on a phone and on a desktop. To reach it, see [Wi-Fi setup](../../getting-started/wifi/).

:::caution[Keep it on a trusted LAN]
The web UI is plain HTTP on the local network by design. It is protected by pairing, the `X-Hygeia` header and the host allow-list, not by encryption, so anything on the network can read what travels between the browser and the device. Use it at home, or on the device's own access point. The firmware download from GitHub is outbound and uses HTTPS.
:::

![The web UI dashboard in a desktop browser](/hygeia/screenshots/webui-dashboard.png)

A browser pairs once with a code shown on the device screen. Until then every page shows "Pair this browser". See [Pairing](../../getting-started/wifi/#pairing).

The pages update on their own when the device changes, for example when a dose is logged on the screen. If the device cannot be reached, the page keeps what it shows and says so.

## Dashboard

One tile per medicine with the ring, the code and the last and next dose, as on the Home screen. A due dose shows its card with Taken, Skip and Snooze. A logged dose offers Undo for 5 seconds.

## Medicines

The list of active medicines, with Add medicine, Edit, the order arrows with Save order, and an Archived section from which a medicine can be brought back or deleted. Delete removes the medicine and its log entries after a confirmation that shows how many entries go. The form sets the name, dose, code, colour, schedule (fixed times, as needed, weekdays, minimum interval), the early, late and missed windows and the pill count. Two people editing the same medicine do not overwrite each other: the second save is refused and the page shows the new data.

## History and Log

History shows the last 30 days as a grid per medicine, with the same colours as the screen: filled for taken, an amber dot for late, a red outline for missed, a grey outline for skipped and a dark cell for days the device was off. Adherence is shown per medicine. Tap a day to see its entries.

Log is the table behind the grid, newest first, with filters for medicine and dates and Load more for older entries. Each row has Edit (time, status, note) and Delete, both after a confirmation. Add a past dose records a dose taken earlier, with its time and a note. Times use the time zone of the device.

## Schedule

The current week, Monday to Sunday: a column per day on a wide screen and a list per day on a phone. Each slot shows its state: upcoming, due, late, missed, taken, skipped or not tracked. As-needed medicines have no slots.

## Settings

- Screen and sound: volume with Play chime, brightness, sound, do not disturb, snooze minutes, dim time and screen off time on battery and on USB, Double tap to wake (off: a touch does not wake the screen while it is off, which saves current), and Wake on single tap (off: tap twice to wake the dimmed or dark screen; hidden while Double tap to wake is off)
- Quiet hours: from, to, or off
- Rotation lock
- Clock: the device time and when it was last synced, a time zone picker with the zones of the browser, and Sync from this browser. The device refuses a zone name that is not in its own table and the page says so
- Paired browsers: the list, forget one, forget all. Forgetting this browser returns to the pairing page
- Integrations (MQTT, Home Assistant, webhooks): "Coming in a later version". The page has no controls for them
- Power off: asks for confirmation and warns when a dose is due within 24 hours. The device stays off, and does not remind, until you press the power button

Settings links to Wi-Fi, Backup, Firmware and Cabinet open.

## Cabinet open

The page sets [Cabinet open](../cabinet-open/) and shows its state. It has three parts:

- Reminder: Cabinet open on or off, Trigger (Any movement or Taught movement), Sensitivity ("1 of 5" to "5 of 5"), "Remind if due within" ("Overdue only" or 5 to 120 min), Chime and Light the screen. Taught movement can be chosen only once a movement is taught
- Movement: "Not taught" or when the movement was taught, the matching limit, counters since the start (motion interrupts, captures, matches, other movements, reminders), Download samples and Forget the movement. The movement itself is taught on the device
- Test: Start and Stop. The counters follow the device live, with the distance and limit of the last Taught capture. The screen of the device has to be on to start, and it stays on while the test runs. While the movement is being taught or checked on the device, the page shows "The movement is being taught on the device" or "The movement is being tested on the device" and Start and Forget are disabled

The page uses `GET /api/cabinet`, `POST /api/cabinet/test`, `GET /api/cabinet/samples.csv` and `DELETE /api/cabinet/teach`, and the `cabinet` object of `GET /api/settings` and `PUT /api/settings` (see the [REST API](../../reference/rest-api/#routes)). Forget is refused while the movement is being taught or checked on the device. Forgetting a movement while the trigger is Taught movement also sets the trigger back to Any movement.

## Wi-Fi

The mode (Off, Wi-Fi, Access point), the saved networks with Forget, a form to add a network, the access point password, the host name and Keep Wi-Fi on battery. Switching the mode from the access point or from Wi-Fi drops the connection of the page. The page then says, for example: "The page will lose its connection. Reconnect to HomeNet and open http://hygeia.local". The access point password is never shown: it is a field of dots with a Show button that works only for what you typed. See [Wi-Fi setup](../../getting-started/wifi/) for the battery rule.

## Backup

Export:

- Download CSV: the log for a spreadsheet. A note that starts with `=`, `+`, `-` or `@` gets a leading `'` so a spreadsheet does not run it
- Download JSON: medicines, log, untracked periods and settings. The box "Include Wi-Fi passwords" is off by default. When it is on, the file holds the saved Wi-Fi passwords and the access point password in plain text: keep that file private

Import takes a JSON file from Export:

1. Choose the file and the mode. Merge adds what the device does not have and removes nothing. Replace swaps in the whole file
2. Tap Preview. The device counts new, changed and unchanged medicines and new and duplicate log entries, and changes nothing
3. Tap Import. For Replace the page first shows "Everything on the device is replaced. The current data is kept in a backup folder until the next import" and asks for confirmation

If the file holds settings or Wi-Fi networks, a box offers to apply them too. Dose entries made while a replace runs make it stop with "changed": nothing is replaced, and you preview and import again. The device accepts files up to 4 MB.

## Firmware

The page shows the installed version and offers two ways to update.

### Update from GitHub

Tap Check for update. The device sends one request to github.com and reads the version of the latest release from the answer. Nothing is checked in the background: the device contacts GitHub only when you tap Check for update or Install, on this page or on the screen under Settings, About.

- The device needs a Wi-Fi network with internet. In access point mode, or when Wi-Fi is off, the page says why it cannot check. The rest of the web UI keeps working
- When a newer release exists the page shows "Install" with the version and a link to the release notes on GitHub. Tap Install, confirm, and the device downloads the firmware over HTTPS, checks it and restarts. A bar shows the download
- The device installs the file only when its checksum matches the one published with the release and the file is the version it showed. Otherwise nothing is installed and the page says so
- A development build is always offered the latest release, with a note that the installed firmware may be newer than the release. A release that is equal to or newer than the latest one shows "Up to date"
- On battery the install needs a charge of 30 percent or more. A check works at any charge
- A check or install that is already running on the screen or in another browser shows here too, and the buttons and the upload are disabled meanwhile

### Upload a file

Choose the file `hygeia.bin` from a build and tap Upload and restart. A bar shows the upload. After the upload the device checks the file, writes it and restarts. The page then says "The device restarts. This page reconnects when it is back" and asks the device for its version every 2 seconds for up to 60 seconds.

- On battery the update needs a charge of 30 percent or more
- The new firmware has to confirm itself soon after it starts. If it does not, the device restarts into the previous version after about 150 seconds. If the page gives up after 60 seconds, wait a little and reload: the device may be back on the previous version
- The first install of a firmware with this feature has to be done over USB, because it carries a new bootloader with rollback. After that, the page updates it. See [Build and flash](../../getting-started/flashing/)
- Settings and the log are kept across an update

## Screen and web

Every action on the screen has a route in the [REST API](../../reference/rest-api/#screen-and-web-parity). The things only the web UI does are the full medicine form, editing and deleting log entries, adding networks, backups, firmware updates and the Cabinet open sample download. Teaching the movement is done only on the device.
