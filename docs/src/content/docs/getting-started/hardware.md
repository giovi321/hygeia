---
title: Hardware
description: Board facts for the Waveshare ESP32-S3-Touch-AMOLED-2.16 and what was verified.
---

Hygeia targets one board: the Waveshare ESP32-S3-Touch-AMOLED-2.16. No other board is supported.

## Board facts

Source: the Waveshare documentation and the Waveshare example repository, read 2026-10-28.

| Part | Detail |
|---|---|
| MCU | ESP32-S3R8, 8 MB octal PSRAM, 16 MB flash |
| Display | 2.16 inch 480x480 AMOLED, CO5300 controller over QSPI |
| Touch | CST9220-family controller on I2C, interrupt on GPIO 11, reset on GPIO 40 |
| I2C bus | SDA GPIO 15, SCL GPIO 14, shared by touch, PMU, IMU, RTC and audio codecs |
| PMU | AXP2101, MX1.25 Li-ion connector, charging, PWR key |
| RTC | PCF85063ATL, interrupt on GPIO 13 |
| Audio out | ES8311 codec, amplifier enable on GPIO 46 |
| Buttons | BOOT on GPIO 0, user key on GPIO 18, PWR through the AXP2101 |
| Other | QMI8658 IMU, ES7210 dual microphone ADC, microSD slot (SPI) |

The firmware uses the display, touch, the PMU (power-on gate, battery readings, PWR key and power off), the RTC, the audio codec, the user key as a wake source and the IMU (accelerometer only, read while the screen is on or dimmed, for auto-rotate and shake to undo). The microphones and microSD slot are not used.

## Verified on the board

Observed from the serial log and on the board, 2026-10-28 and 2026-10-29. Full notes are in `design/hardware-notes.md` in the repository.

- Display: full 480x480 flushes, all four corners visible, no offset or tearing
- Touch: no axis swap or mirror needed; corner touches report the expected coordinates
- I2C devices answer at 0x18, 0x34, 0x40, 0x51, 0x5a and 0x6b. The mapping of address to chip follows the usual addresses of these parts and was not read back from the chips
- RTC: keeps time over a reset, over a USB power cycle and over 74 s with USB unplugged and the battery connected
- RTC alarm: fires through the hardware interrupt at the set time
- Audio: the ES8311 path plays the chime; volume 60 was judged fine by ear
- Power-on source: the AXP2101 reports whether the PWR key started the device (see [Idle clock and power](../../user-guide/idle-power/))

## Not verified

- Whether the RTC keeps time with no battery and no USB. No backup cell is documented for the board. If the time is lost, the device shows the "Set the time" screen
- The "Play chime" button in Sound settings, and the repeat chimes every 2 minutes
- Battery life: no discharge measurement exists yet
- The BOOT-key bypass of the power-on gate

## Display and memory

- LVGL 9 draws to two 48-line buffers of 480 px in RGB565 (46080 bytes each) in internal DMA-capable RAM
- With 50 active medicines, building the Home tiles takes 91 to 94 ms on the board, above the 60 ms target. With 3 medicines it takes about 9 ms
