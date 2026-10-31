---
title: Wi-Fi setup
description: Join the device by its access point and QR code, add your network, pair a browser and what the battery does to the radio.
---

The device serves the web UI over Wi-Fi. It has two network modes besides off: the access point, where the device makes its own network, and Wi-Fi client, where it joins your home network. Networks are added only in the web UI, so the first setup always uses the access point.

:::caution[Keep it on a trusted LAN]
The web UI has no TLS. Use it on your home network or on the device's own access point, and do not forward port 80 to the internet. See [REST API](../../reference/rest-api/).
:::

## First setup with the access point

1. Pull down quick settings and tap the Access point tile. The device starts an access point with the name `hygeia-XXXX` (the X characters are fixed per device) and a generated password
2. The screen shows "Join the device's Wi-Fi" with a QR code and the name and password in text. Scan the code with a phone, or join the network by hand, then tap Next
3. The screen shows "Then open" with a QR code for `http://192.168.4.1/`. Scan it or type the address.
4. The web UI asks you to pair the browser. See [Pairing](#pairing)
5. Open Settings, Wi-Fi in the web UI. Under "Add a network", enter the name and password of your home network and tap Save network. The device keeps at most 5 networks
6. Choose Wi-Fi as the mode. The page warns that it will lose its connection. Confirm, reconnect your phone or computer to your home network and open `http://hygeia.local`

The access point password can be changed in the web UI under Access point, and the host name under Host name. The access point name is fixed.

## Open the web UI later

With the device on your home network, tap the Web UI QR button in quick settings. The screen shows a QR code for `http://<ip>/` and the address in text, for example `http://192.168.1.23` and `hygeia.local`. In access point mode the button shows the address `http://192.168.4.1/`.

The device answers to its IP address, to `hygeia` (or the host name you set), to `hygeia.local` and to a name that starts with `hygeia.`. On Wi-Fi client mode other names get an error; in access point mode they are sent to the web UI. See [Host names](../../reference/rest-api/#host-names).

`hygeia.local` works through mDNS in Wi-Fi client mode. Some networks and some Android versions do not resolve `.local` names: use the IP address from the Web UI QR button then. Give the device a fixed address in your router if you want a bookmark that does not change.

## Pairing

The web UI shows data and changes it, so each browser pairs once:

1. Open the web UI. An unpaired browser shows "Pair this browser". Tap "Show a code on the device"
2. The device wakes its screen and shows a 6-digit code for 120 seconds
3. Type the code in the browser and tap Pair

Five wrong codes stop pairing for a minute. The device keeps 4 browsers and forgets the oldest when a fifth pairs. Forget browsers in the web UI under Settings, Paired browsers, or on the device under Settings, Wi-Fi.

## The battery and the radio

Wi-Fi uses power, and the CPU cannot sleep while the radio is on. On battery the device therefore handles the radio like this:

- The saved mode does not start by itself after a restart on battery. Tap the Wi-Fi or Access point tile, or switch the mode in the web UI, to start it
- The radio stops after 10 minutes with no web request and no touch. The Wi-Fi tile then reads "Off on battery"
- After that pause, a touch on the screen or a web request does not start the radio again. Only a tap on the Wi-Fi or Access point tile, a mode change from the web UI or plugging in USB starts it. Plugging in USB starts the saved mode
- Settings, Wi-Fi has the switch "Keep Wi-Fi on battery". With it on, the radio stays on while on battery and the battery drains faster

A firmware update on battery needs a charge of 30 percent or more.

## Time from the network

In Wi-Fi client mode the device sets its clock over NTP, and the web UI can set the clock and the time zone from the browser under Settings, Clock. Both keep reminders correct without touching the screen. See [First boot](../first-boot/).

## If the network does not work

- The Wi-Fi tile reads "Can't connect" when a saved network does not answer. The device keeps trying that mode and never falls back to the access point on its own. Tap the Access point tile to get back in
- A wrong password is only visible as "Can't connect". Save the network again with the right password under Settings, Wi-Fi in the web UI
- To start over, forget the saved networks in the web UI or run the access point again
