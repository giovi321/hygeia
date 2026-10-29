# Hardware notes

Observed on the Waveshare ESP32-S3-Touch-AMOLED-2.16 with the registry BSP (waveshare/esp32_s3_touch_amoled_2_16 2.0.1), 2026-10-28, from the serial log only.

## Panel init

- CO5300 over QSPI, driver espressif/esp_lcd_co5300 2.2.0, initialised by the BSP function `bsp_display_new()`
- The panel driver logs three warnings (commands 3Ah, 36h, 29h overwritten by the external init sequence); expected with the BSP init table
- Panel creation and init take about 1.3 s after boot (log 0.6 s to 1.9 s)
- Registry BSP path was sufficient: full 480-wide flushes, even-aligned, no custom init sequence needed

## Resolution and flush areas

- LVGL display 480x480, RGB565
- Flush areas seen: x=0..479, y in steps of 48 (480x48), including the full-screen redraw after `lv_obj_clean`
- None logged: no odd-size or misaligned flush appeared in the serial log during the observed runs (the 2-pixel rounder is re-implemented in `board.c`, because the BSP display start function is bypassed)
- Partial flushes narrower than the screen can be taller than 48 rows (210 px wide: 108 rows), because LVGL fits rows into the same 46080-byte buffer; the flush log flags only misalignment or more than 480 x 48 pixels
- Display, owner-verified on firmware a319c4e: all four 40 px corner squares fully visible, no offset or tearing

## Buffers

- 2 draw buffers of 48 lines x 480 px x 2 bytes = 46080 bytes each, internal DMA-capable RAM, no PSRAM
- Internal DMA RAM used by display registration: 94952 bytes
- Heap after init: internal 195659 bytes, DMA-capable 187871 bytes, PSRAM 8384508 bytes
- The BSP display start function hardcodes 50-line PSRAM buffers, so `hy_board_init` registers the display with esp_lvgl_adapter itself
- The BSP lock wrapper `bsp_display_lock()` returns the esp_err_t as a bool, so ESP_OK reads as false; `hy_board_lock` calls `esp_lv_adapter_lock` directly

## Touch

- CST9217 driver reports chip type 0x9220, resolution 480x480, project ID 0x542F
- Interrupt mode enabled, single-point input

## I2C devices found

Bus SDA 15, SCL 14, 7-bit addresses found by probing 0x08 to 0x77: 0x18, 0x34, 0x40, 0x51, 0x5a, 0x6b

- 0x18 matches the ES8311 codec
- 0x34 matches the AXP2101 PMU
- 0x51 matches the PCF85063 RTC
- 0x5a matches the CST9217 touch controller
- 0x6b matches the QMI8658 IMU
- 0x40 matches the ES7210 mic ADC

The device-to-address mapping is inferred from the usual addresses of these parts, not read from the chips.

## Serial console

- COM7 is the ESP32-S3 USB Serial/JTAG port; the console is set to it as primary (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`) so that typed lines reach the firmware. With the UART primary and USB Serial/JTAG secondary, input was not received

## Touch orientation

Verified on firmware a319c4e: no axis swap or mirror. The BSP default `swap_xy=1, mirror_x=0, mirror_y=1` is used unchanged, and the logged touches match the corners: TL x=33 y=28, TR x=466 y=29, BL x=24 y=460, BR x=460 y=466.

The owner's first two touch attempts in one session logged no touches at all; this is unexplained and was not reproduced.

## Power-on source

- AXP2101 at 0x34 confirmed: register 0x03 reads 0x4A, the chip id in XPowersLib (`AXP2101Constants.h`, commit d6997586e68f65afd51baa775903df930db39821)
- PWRON status register 0x20, bits: 0 PWRON key low, 1 IRQ pin low, 2 VBUS insert, 3 battery above 3.3 V when charged, 4 battery insert, 5 EN mode. Power-off: set bit 0 of register 0x10. XPowersLib has no call that clears the status, so the latch is not cleared
- Log evidence on COM7 (USB connected, 2026-10-28), log-only build then enforcing build:
  - after `idf.py -p COM7 flash`: `pwron_status=0x04 reset_reason=11 (other)`
  - after a readlog.py RTS reset: `rst:0x15 (USB_UART_CHIP_RESET)`, `pwron_status=0x04 reset_reason=11 (other)`
  - the VBUS-insert bit stays latched across chip resets; flash and RTS resets report ESP_RST_USB (11), not power-on, so they keep running
- Decision: enforcement is unconditional. Only `ESP_RST_POWERON` is gated; everything else keeps running. The gate fails open: a power-on reset with no known source bit (R22), or with EN mode (bit 5) set (R24), boots; a chip-id register (0x03) other than 0x4A skips the gate and writes nothing to 0x10

Owner-verified on firmware a319c4e:

- Battery connected, after a PWR long-press off: unplugging and replugging USB leaves the screen dark and the charge LED shows charging
- A PWR press boots with `pwron_status=0x01`, `reset_reason=1` (power-on); the gate allows
- USB only, no battery: plugging in leaves the screen dark; PWR boots with `0x01`
- The BOOT bypass is not tested on hardware
- One earlier accidental USB replug came up running with no boot log captured; it was not reproduced in later replugs

### Recovery

- GPIO 0 is a strapping pin: holding BOOT through the reset (while plugging in or pressing PWR) enters ROM download mode instead of running the app
- Bypass: press BOOT right after pressing PWR or plugging in (after the reset) and hold it until the log shows `gate bypassed (BOOT held)`
- Reflash a device that keeps powering off: hold BOOT before the reset (while connecting USB) to enter ROM download mode

## RTC

Verified on the board from the serial log, 2026-10-28.

- PCF85063 at 0x51 on the shared bus; register layout taken from the SensorLib driver (see `components/hy_clock/src/pcf85063.c`): CTRL1 0x00, time registers 0x04..0x0A in BCD, seconds bit 7 = oscillator-stop (OS) flag, year 00..99 read as 2000 + yy, no century bit
- First boot after the earlier firmware: raw `a0 56 00 01 06 01 00`, OS flag set, decoded 2000-01-01; the firmware reported the clock invalid
- `time <epoch> [posix_tz]` wrote the RTC; read back 1 to 2 s later with the OS flag clear, weekday register 3 for Wednesday 2026-10-28
- After an RTS reset (power stays on) the RTC kept running and the firmware reported the clock valid with the correct UTC time; the TZ stored in NVS (namespace "clock", key "tz") was also restored
- A time before the build epoch is rejected by `time` (ESP_ERR_INVALID_ARG)
- Time is kept over a USB power cycle (`osc_stopped=0`, valid)
- RTC kept time over 74 s with USB unplugged and the battery connected, device off (`osc_stopped=0` afterwards)
- Not tested: time kept with no battery and no USB. Whether the board has a backup supply for the PCF85063 is unknown; the firmware's "Set the time" flow covers the case where it does not

## RTC alarm

Verified on the board, 2026-10-28 (serial) and 2026-10-29 (owner, build 7f8df51).

- The PCF85063 interrupt is on GPIO 13 (input, pull-up, falling edge); the ISR only sets a flag, `hy_clock_alarm_take` reads it
- `alarm 20`: fired 19.5 s after the command, at exactly set time + 20 s, through the hardware interrupt
- A target at or before now + 2 s is not armed: the chip is disarmed and a software due-now flag is raised, so the next take returns true (`alarm 1` fired 0.12 s later)
- One-shot contract: `hy_clock_alarm_take` disarms the chip (AIE and AF cleared) and resets the programmed value; the scheduler calls `hy_clock_alarm_set` after every evaluation to re-arm. Owner check: alarm set at 19232 ms, the scheduler re-armed ("rtc alarm set") at 38302 ms, 19.1 s later
- The chip matches second, minute, hour and day of month only, with no month field: a target more than about one month ahead fires early
- `hy_clock_alarm_take` logs `rtc alarm fired at <utc>` and names the source, hardware interrupt or software due-now flag (added after the 2026-10-29 owner check, which could only infer the fire from the re-arm; not yet run on the board)

## Audio

- Codec path ES8311 verified: `hy_audio_init` returns ESP_OK, the chime renders to 16319 samples at 22050 Hz, played in about 850 ms including 100 ms of trailing silence (748 ms before the silence was added)
- Volume 60 judged "fine" by the owner on 2026-10-29
- Not yet checked: the "Play chime" button in Sound settings, and the repeat chimes every 2 min up to 5 times (device muted in an office)

## Gestures

Thresholds that worked on the panel (owner session 2026-10-29, build 7f8df51):

- Edge zone: 40 px (top edge down, bottom edge up from y 440, left edge right)
- Swipe: dominant axis at least 60 px and at least 1.5 times the other axis
- Tap: at most 16 px of movement and under 500 ms; a long press is at most 16 px and 500 ms or more
- Double tap: 300 ms between release and the next press, 40 px between the two points
- A stroke that leaves and returns to the start is not a tap: `max_dev` (maximum deviation from the start) must also be 16 px or less, not only the end-point distance
- A stroke that starts in an edge zone is never consumed by a widget, except sliders, which take it with a forced consume

## Splash

- Splash done in 1008 ms on both boots; maximum frame gap 74 ms inside the 900 ms animation (target about 50), measured on build 7f8df51 before the hidden views moved out of the splash

## Performance

Scheduler evaluation and Home refresh on the board (newlib, CLIB malloc):

- 3 active medicines: evaluation 34.3 to 35.6 ms (target 60 ms), `home_refresh` 8.8 to 9.2 ms (target 20 ms)
- 50 active medicines: evaluation about 141 ms (target 300 ms), `home_refresh` 91 to 94 ms (target 60 ms, missed). `hy_tile_view` costs about 1.8 ms per fixed medicine, 88 to 90 ms for 50 tiles; LVGL updates are 3 to 4 ms
- Picolibc was slower (3 medicines: evaluation 60 to 64 ms, `home_refresh` 33 ms; 50 medicines: 306 ms and 315 ms) because it re-parses the TZ string on every `localtime_r` and `mktime`; the project builds with newlib
- Internal heap free with 50 tiles: 44 to 49 KB (minimum 35 KB); with 3 medicines 97 to 151 KB

## Owed hardware checks

Owner-verified on firmware 73db1cc (2026-10-29): repeat chimes (5 plays at 2 min intervals, then silent), Play chime in Sound settings, detail scroll, due card dots, QWERTY note keyboard. Splash 1000 to 1009 ms with a 52 ms maximum frame gap over 3 boots. RTC alarm log line: hardware fire at the programmed second, software due-now fire within 1 s.

Still owed:
- Serial check A: waking stroke while a due card appears (`idle on`, `gesture hold`, `remindtest 0`, `gesture release left`: no snooze, nav stays `[due]`). The run on 73db1cc was inconclusive because real touches released the scripted hold
- Serial check B: idle Next after an early dose (take V early, `idle on`, the idle screen must not name the satisfied slot)

## Home grid

- Owner-verified on firmware a319c4e: tile tap, Undo by finger, scroll with 5 medicines, and the panel's look against the mockup
- LVGL task stack minimum free: 6996 bytes

- Owner-verified on firmware 86d1ea6: taps on other tiles while the logged toast shows do not log, Undo still removes the entry, and a second tap within 5 s after the too-soon warning logs the dose (serial log not captured for this check)
