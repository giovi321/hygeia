---
title: Security notes
description: What protects the web UI and the REST API, what does not, and how to run the device safely.
---

The web UI and the REST API let a browser read and change everything on the device. These notes list the rules the firmware applies and the limits that remain.

## What the device enforces

- Pairing: no route except `GET /api/info` and the pairing routes answers an unpaired browser. A browser pairs with a 6-digit code shown on the device screen, valid for 120 seconds. Five wrong codes stop pairing for 60 seconds. A code never appears while a dose reminder or the battery power-off countdown is on the screen
- Session cookie: a paired browser holds the cookie `hy_s`, 64 hex characters, `HttpOnly` and `SameSite=Strict`, valid for one year. The device stores only the SHA-256 of the token, in NVS. The device keeps 4 paired browsers and forgets the oldest when a fifth pairs. Forget browsers in the web UI under Settings, or on the device under Settings, Wi-Fi
- `X-Hygeia` header: every request that is not a `GET` needs `X-Hygeia: 1`. A page on another site cannot send that header without a cross-origin permission the device never gives
- Origin check: when a request carries an `Origin` header, it has to be `http://` plus the same host as the `Host` header
- Host allow-list: a request is answered only when its `Host` header is the device address, the host name, `<host>.local` or a name whose first label is the host name. Any other name gets `421`. This blocks DNS rebinding, where a page on another site points its own name at the address of the device
- Write-only secrets: no route returns a saved Wi-Fi password or the access point password. The Wi-Fi page shows the access point password as dots. The only exception is an export that you ask to include passwords
- Firmware images: an upload is checked for the Hygeia image header and size, and the new firmware must confirm itself or the device returns to the previous version. See [Web UI](../../user-guide/web-ui/#firmware)
- Limits: bodies are up to 16 KB, an import up to 4 MB and a firmware image up to 3 MB. JSON nesting is limited. The device serves few requests at once and answers `503` when busy

See [REST API](../rest-api/) for the routes and error codes.

## What the device does not do

- Plain HTTP by design. The web UI does not use TLS: the device serves plain HTTP on port 80, protected by pairing, the `X-Hygeia` header and the host allow-list. The pairing code, the session cookie, every page and every export travel unencrypted, and anyone on the same network segment can read them. The update download from GitHub is outbound and uses HTTPS
- No user accounts. A paired browser has full access. Pairing proves that someone could see the screen of the device
- No protection against someone who holds the device. Anyone who can touch it can open Settings, show a pairing code and pair a browser

## Run it safely

- Use it on a home network you trust, or on the access point of the device. Do not forward port 80 to the internet and do not publish the address
- Pair a browser only when you started the pairing yourself. A code that appears on the screen without you asking means a browser on the network asked for one: cancel it and check the network
- Change the access point password in the web UI under Wi-Fi if you shared it. A new password restarts the access point
- Keep exported files private. A JSON export with "Include Wi-Fi passwords" holds the saved passwords in plain text. An export without that box holds none
- Forget browsers you no longer use, and all browsers before you give the device away
- Turn Wi-Fi off when you do not need it. On battery the radio stops by itself after 10 minutes without use. See [Wi-Fi setup](../../getting-started/wifi/#the-battery-and-the-radio)
