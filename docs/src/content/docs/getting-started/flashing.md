---
title: Build and flash
description: Build Hygeia with ESP-IDF v6.0.2 and flash it to the board.
---

There are no prebuilt release images yet. You build the firmware from source.

## Requirements

- ESP-IDF v6.0.2. Other versions are not tested; the component manifest allows `>=6.0.2,<6.1`
- The board connected over USB-C to the USB Serial/JTAG port
- Components from the ESP component registry are pinned to exact versions and downloaded on the first build: the Waveshare board support component 2.0.1, LVGL 9.6.0, esp_lvgl_port 2.9.0, cJSON and LittleFS. `dependencies.lock` is committed

## Set up the environment

On Windows in PowerShell, with ESP-IDF v6.0.2 installed in your user profile:

```powershell
& $env:USERPROFILE\esp-idf-v6.0.2\export.ps1
```

On Linux or macOS, source the `export.sh` of your ESP-IDF v6.0.2 checkout.

## Debug build

The default build includes a serial debug console (`CONFIG_HY_DEBUG_CONSOLE=y`) with line commands for development: time, tap, screenshots, reminder tests and others. See [Development](../../reference/development/).

```bash
idf.py build
idf.py -p <port> flash
```

Replace `<port>` with the serial port of the board, for example `COM7` on Windows or `/dev/ttyACM0` on Linux.

## Release build

The release build turns the debug console off. It uses a separate build directory and its own `sdkconfig`:

```bash
idf.py -B build-rel -D SDKCONFIG=build-rel/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release" build
```

Without `-D SDKCONFIG=...` the command silently reuses the debug `sdkconfig`, and the console stays on. To flash the release build, add `-B build-rel` to the flash command:

```bash
idf.py -B build-rel -p <port> flash
```

## Flash layout

The 16 MB flash holds NVS (64 KB), otadata, phy_init, two 3 MB app slots (`ota_0`, `ota_1`) and a LittleFS `storage` partition of 0x9E0000 bytes (about 9.9 MB) that holds the medicines and the log. Over-the-air update is not implemented; the second app slot is laid out for it.

`idf.py flash` writes the bootloader, partition table and app. It does not erase the storage partition, so the log survives a reflash. `idf.py erase-flash` wipes everything, including the log.

## Download mode

GPIO 0 is a strapping pin. To enter ROM download mode, hold BOOT while connecting USB or pressing PWR, then release it after the reset. Use this to reflash a device that keeps powering off.

## Continuous integration

The repository builds the firmware (debug and release configurations) in the official ESP-IDF container for v6.0.2 on every push and pull request.
