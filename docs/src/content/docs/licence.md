---
title: Licence
description: Hygeia is licensed under CC BY-NC-SA 4.0.
---

Hygeia is licensed under the Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License (CC BY-NC-SA 4.0). This covers the firmware source code, documentation, logo, mockups and all other material in the repository, unless a file states otherwise.

You may share and adapt it for non-commercial purposes, provided you give appropriate credit, indicate if changes were made, and distribute your contributions under the same licence.

Because of the non-commercial term, Hygeia is source-available. It is not open source in the sense of the Open Source Definition.

- Human-readable summary: [creativecommons.org/licenses/by-nc-sa/4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)
- Full legal code: [creativecommons.org/licenses/by-nc-sa/4.0/legalcode](https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode)

The material is provided as is, without warranties of any kind.

## Not a medical device

Hygeia is a personal reminder tool, not a medical device. Do not rely on it as the only safeguard for medication that must not be missed.

## Third-party code

Hygeia's own licence does not apply to the components below. Each keeps its own licence.

Kept in the repository with their licence files:

| Component | Where | Licence |
|---|---|---|
| cJSON | `host_tests/third_party/cjson`, host tests only | MIT |
| Unity | `host_tests/third_party/unity`, host tests only | MIT |
| Inter font, compiled to LVGL C arrays | `components/hy_ui/fonts` | SIL Open Font License 1.1 (`OFL.txt`) |
| posix_tz_db, the time zone to POSIX rule table | `third_party/posix_tz_db` | MIT |

Fetched at build time by the ESP-IDF component manager, not stored in the repository. The versions are pinned in `dependencies.lock`:

| Component | Version | Licence |
|---|---|---|
| ESP-IDF | 6.0.2 | Apache 2.0 |
| LVGL | 9.6.0 | MIT |
| Montserrat font, built into LVGL | | SIL Open Font License 1.1 |
| waveshare/esp32_s3_touch_amoled_2_16 | 2.0.1 | Apache 2.0 |
| waveshare/esp_lcd_touch_cst9217 | 2.0.0 | Apache 2.0 |
| joltwallet/littlefs | 1.23.1 | MIT |
| espressif/cjson | 1.7.19~2 | MIT |
| espressif/esp_lvgl_port | 2.9.0 | Apache 2.0 |
| espressif/esp_lvgl_adapter | 0.6.4 | Apache 2.0 |
| espressif/esp_lv_decoder | 0.4.3 | Apache 2.0 |
| espressif/esp_lv_fs | 1.0.1 | Apache 2.0 |
| espressif/esp_mmap_assets | 2.0.1 | Apache 2.0 |
| espressif/esp_lcd_co5300 | 2.2.0 | Apache 2.0 |
| espressif/esp_lcd_panel_io_additions | 1.0.1~1 | Apache 2.0 |
| espressif/esp_lcd_touch | 1.2.1 | Apache 2.0 |
| espressif/esp_io_expander | 1.2.1 | Apache 2.0 |
| espressif/esp_codec_dev | 1.5.11 | Apache 2.0 |
| espressif/button | 4.2.3 | Apache 2.0 |
| espressif/knob | 1.1.0 | Apache 2.0 |
| espressif/usb | 1.6.0 | Apache 2.0 |
| espressif/cmake_utilities | 0.5.3 | Apache 2.0 |
| espressif/esp_new_jpeg | 1.0.2 | MIT (Espressif) |
| espressif/freetype | 2.14.3~1 | FreeType License |
| espressif/libpng | 1.6.59 | PNG Reference Library License |
| espressif/zlib | 1.3.2~2 | zlib licence |

A firmware image built from this repository contains LVGL, the Montserrat and Inter fonts, ESP-IDF code and the other components that the linker keeps. Their notices apply to that image.
