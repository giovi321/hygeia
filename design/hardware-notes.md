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
- Display, verified by hand on firmware 9c7539e: all four 40 px corner squares fully visible, no offset or tearing

## Rounded glass corners

The glass hides a rounded area in each corner of the 480 x 480 panel. The radius is not documented and was not measured.

- Design estimate: 70 px, the constant `HY_CORNER_R` in `components/hy_ui/src/ui_internal.h`. Layouts keep everything that must be read or touched inside a circle of that radius centred 70 px in from each corner
- Basis (to be confirmed): on 94b19a0 the outer bottom keys of the text keyboard (key edge 6 px from the side, bottom 8 px above the screen edge, 12 px rounding) were reported half cut. Geometry says those keys start losing pixels at a radius of about 35 px and lose 4 to 8 % of their area at 50 to 60 px, so the reported cut points to the upper end of the 40 to 60 px first guess or above. Nothing else was reported clipped; the top bar clock and the Set buttons of Time zone and Set the time would start to lose pixels at about 70 to 72 px
- The corner-square check above ("all four 40 px corner squares fully visible") does not fit these figures: a rounded corner of 50 px would hide about a third of such a square, its outer corner first. That check was looser than its wording
- To confirm: `boardtest` draws, in every corner, quarter circles of radius 50, 60, 70, 80, 90 and 100 px (colours listed in the centre label). The glass radius is the smallest one whose arc shows whole, its middle included. Set `HY_CORNER_R` to that value or above

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

Verified on firmware 9c7539e: no axis swap or mirror. The BSP default `swap_xy=1, mirror_x=0, mirror_y=1` is used unchanged, and the logged touches match the corners: TL x=33 y=28, TR x=466 y=29, BL x=24 y=460, BR x=460 y=466.

The first two touch attempts in one session logged no touches at all; this is unexplained and was not reproduced.

## Power-on source

- AXP2101 at 0x34 confirmed: register 0x03 reads 0x4A, the chip id in XPowersLib (`AXP2101Constants.h`, commit d6997586e68f65afd51baa775903df930db39821)
- PWRON status register 0x20, bits: 0 PWRON key low, 1 IRQ pin low, 2 VBUS insert, 3 battery above 3.3 V when charged, 4 battery insert, 5 EN mode. Power-off: set bit 0 of register 0x10. XPowersLib has no call that clears the status, so the latch is not cleared
- Log evidence on COM7 (USB connected, 2026-10-28), log-only build then enforcing build:
  - after `idf.py -p COM7 flash`: `pwron_status=0x04 reset_reason=11 (other)`
  - after a readlog.py RTS reset: `rst:0x15 (USB_UART_CHIP_RESET)`, `pwron_status=0x04 reset_reason=11 (other)`
  - the VBUS-insert bit stays latched across chip resets; flash and RTS resets report ESP_RST_USB (11), not power-on, so they keep running
- Decision: enforcement is unconditional. Only `ESP_RST_POWERON` is gated; everything else keeps running. The gate fails open: a power-on reset with no known source bit (R22), or with EN mode (bit 5) set (R24), boots; a chip-id register (0x03) other than 0x4A skips the gate and writes nothing to 0x10

Verified by hand on firmware 9c7539e:

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

Verified on the board, 2026-10-28 (serial) and 2026-10-29 (by hand, build 5867131).

- The PCF85063 interrupt is on GPIO 13 (input, pull-up, falling edge); the ISR only sets a flag, `hy_clock_alarm_take` reads it
- `alarm 20`: fired 19.5 s after the command, at exactly set time + 20 s, through the hardware interrupt
- A target at or before now + 2 s is not armed: the chip is disarmed and a software due-now flag is raised, so the next take returns true (`alarm 1` fired 0.12 s later)
- One-shot contract: `hy_clock_alarm_take` disarms the chip (AIE and AF cleared) and resets the programmed value; the scheduler calls `hy_clock_alarm_set` after every evaluation to re-arm. Board check: alarm set at 19232 ms, the scheduler re-armed ("rtc alarm set") at 38302 ms, 19.1 s later
- The chip matches second, minute, hour and day of month only, with no month field: a target more than about one month ahead fires early
- `hy_clock_alarm_take` logs `rtc alarm fired at <utc>` and names the source, hardware interrupt or software due-now flag (added after the 2026-10-29 board check, which could only infer the fire from the re-arm; not yet run on the board)

## Audio

- Codec path ES8311 verified: `hy_audio_init` returns ESP_OK, the chime renders to 16319 samples at 22050 Hz, played in about 850 ms including 100 ms of trailing silence (748 ms before the silence was added)
- Volume 60 judged "fine" by ear on 2026-10-29
- Not yet checked: the "Play chime" button in Sound settings, and the repeat chimes every 2 min up to 5 times (device muted in an office)

## Gestures

Thresholds that worked on the panel (hands-on session 2026-10-29, build 5867131):

- Edge zone: 40 px (top edge down, bottom edge up from y 440, left edge right)
- Swipe: dominant axis at least 60 px and at least 1.5 times the other axis
- Tap: at most 16 px of movement and under 500 ms; a long press is at most 16 px and 500 ms or more
- Double tap: 300 ms between release and the next press, 40 px between the two points
- A stroke that leaves and returns to the start is not a tap: `max_dev` (maximum deviation from the start) must also be 16 px or less, not only the end-point distance
- A stroke that starts in an edge zone is never consumed by a widget, except sliders, which take it with a forced consume

## Motion sensor

QMI8658A at 0x6b, driver `components/hy_board/src/qmi8658.c`. Sources: QMI8658A datasheet Rev A (QST, 2022), section and table numbers below, and the SensorLib QMI8658 driver (`SensorQMI8658.hpp`, master, read 2026-11-03). Nothing in this section was measured on the board yet, unless it says so.

Modes:

- Accel only (rotation, shake, Cabinet open stream path while the screen is on): CTRL2 0x26, +-8 g, 125 Hz; scale 4096 LSB/g
- Capture (Taught movement, teach and test): CTRL2 0x26 (accel 112.1 Hz, synchronised to the gyro), CTRL3 0x66 (+-1024 dps, 112.1 Hz; scale 32 LSB/dps), CTRL5 0x77 (accel and gyro low-pass on, mode 3 = 13.37 % of ODR, about 15 Hz), CTRL7 0x03; polled at 50 Hz with one 12-byte burst 0x35..0x40. 6DOF current at 112.1 Hz: 752 to 753 uA typical (DS table 17)
- Wake on motion (Cabinet open live, screen off): CTRL2 0x2D, +-8 g, low-power accel ODR code 13 = 21 Hz (47.6 ms), the lowest low-power rate with a period of at most 50 ms. Low-power accel current 42 uA typical at 21 Hz, VDD 1.8 V (DS table 15; 55, 35 and 30 uA at 128, 11 and 3 Hz)

Wake-on-motion rule: the chip compares the absolute slope between successive samples, per axis, with the threshold, not the deviation from a reference (DS 12.2 table 39 "the threshold of absolute of slope", 12.1, 9.1 "Slope(n) = Acc(n) - Acc(n-1)"). That any one axis is enough is an inference: WoM has no axis or logic setting, and the Any-Motion default is OR. The host emulation is `hy_cab_wom_hit` with `hy_cab_wom_prev`, the sensor sample 48 ms back interpolated from the 50 Hz stream.

Register sequence to arm (DS 12.5 figure 25): CTRL7 0; CTRL2 0x2D; CAL1_L 0x0B = threshold in mg (1 to 255; 0 disables WoM); CAL1_H 0x0C = pin select (bit 6: 0 INT1, 1 INT2), initial level 0 (bit 7), blanking 4 samples (bits 5..0; 190 ms at 21 Hz, past the 3 / ODR settling); CTRL9 command 0x08 with the STATUSINT 0x2D bit 7 handshake (CTRL8 bit 7 = 1, so a command never pulses INT1); CTRL1 output enable for the chosen INT pin (bit 3 INT1, bit 4 INT2, push-pull); CTRL7 0x01. Exit (DS 12.6): CTRL7 0, CAL1_L 0, command 0x08, outputs high-Z. Every mode change goes through that exit. Interrupt behaviour per DS 12.4: the line toggles on each event and stays; reading STATUS1 0x2F (bit 2) clears the WoM bit and returns the line to its initial level. The firmware arms the light-sleep GPIO wake on the level opposite the one read at arming.

Interrupt pin: selectable at run time (`imu pin [1|2 [17|21]]` on the debug console). Default INT1 on GPIO 17, INT2 on GPIO 21 as the alternative. The wiring of the chip's INT pins to the ESP32-S3 is not in the BSP pin header or any other source found; unverified on the board.

Gyro: range +-1024 dps, values in deci-dps (raw x 10 / 32). Turn-on per datasheet: t1 typical 150 ms plus 3 / ODR (27 ms at 112.1 Hz), about 177 ms (DS table 8, 7.3). The firmware masks the gyro for the first 180 ms of every sequence (`HY_CAB_GYRO_START_MS`), teach and live alike. The turn-on time on the board, the still noise and the sign of z for a clockwise turn are to be measured.

Any movement trigger counts on the motion recordings (`host_tests/data/motion`, accel only, `test_cab_rec.c`), slope per 48 ms sensor sample, 2 s holdoff:

| File | sens 1 (100 mg) | 2 (64) | 3 (40, default) | 4 (37) | 5 (34) | peak slope | still-part peak |
| --- | --- | --- | --- | --- | --- | --- | --- |
| carry_walk | 33 | 33 | 33 | 33 | 33 | 1984 | none |
| flat_spin | 12 | 12 | 13 | 13 | 13 | 431 | none |
| putdown_pickup | 8 | 8 | 8 | 8 | 8 | 1288 | 21 |
| rotate_cycle | 7 | 8 | 11 | 12 | 12 | 723 | none |
| shake_firm | 10 | 12 | 15 | 15 | 15 | 10901 | none |
| shake_light | 6 | 8 | 11 | 12 | 12 | 4167 | 17 |
| shake_live_arm | 0 | 0 | 0 | 0 | 1 | 35 | 26 |
| shake_live_combo | 6 | 7 | 7 | 7 | 7 | 15307 | 30 |
| table_knock | 0 | 1 | 1 | 1 | 2 | 90 | 7 |
| tap_desk | 6 | 7 | 9 | 10 | 11 | 908 | 31 |
| tap_double | 2 | 3 | 5 | 5 | 5 | 401 | 21 |
| tap_held | 7 | 9 | 13 | 13 | 13 | 880 | none |
| tilt_diagonal | 8 | 10 | 10 | 10 | 10 | 853 | none |

- The largest slope in a still part of any recording is 31 mg (tap_desk), under the most sensitive step, 34 mg
- A synthetic door swing peaks at 48 mg slope when it starts smoothly and at 126 mg when it starts with a jump. The default, 40 mg, catches the smooth swing down to 0.83 of its nominal speed. Catching that door and ignoring table_knock (peak 90) cannot both hold, so the default favours the door: at the default table_knock gives 1 trigger and flat_spin 13. The trigger only wakes the CPU; the device reacts only with a due dose and at most once every 2 minutes
- Wake-on-motion noise in low-power mode and the slope of a real cabinet door are to be measured

Taught movement matcher margins at sensitivity 3 (`test_cab_rec.c`, `test_cabmatch.c`):

| Model | d_self | Threshold | Largest positive | Smallest negative |
| --- | --- | --- | --- | --- |
| putdown_pickup (recording, accel only) | 1.898 | 2.000 (clamped at the upper bound) | 1.053 (augmented variants) | 3.072 (carry_walk window at 60 s) |
| synthetic door open | 0.345 | 0.552 | 0.289 | 1.880 (door close) |
| synthetic tilt | 0.706 | 1.129 | 0.629 | 4.369 (door open) |
| synthetic lift | 0.232 | 0.372 | 0.200 | 2.545 (door open) |

- Threshold bounds 0.25 and 2.0; the upper bound sits a third below the nearest recording negative. The putdown_pickup model rests on 3 takes from one recording with rests padded in the replay; no held-out real positive exists yet
- Capture: from the trigger until 400 ms of stillness (accel steps within 40 mg around the current pose, gyro under 10 dps), 220 ms minimum, 4 s maximum. The recorded taps capture as 20 to 60 ms, so a tap cannot be taught
- rotate_cycle and tap_double give fewer than 3 captures and have no model

## Splash

- Splash done in 1008 ms on both boots; maximum frame gap 74 ms inside the 900 ms animation (target about 50), measured on build 5867131 before the hidden views moved out of the splash

## Performance

Scheduler evaluation and Home refresh on the board (newlib, CLIB malloc):

- 3 active medicines: evaluation 34.3 to 35.6 ms (target 60 ms), `home_refresh` 8.8 to 9.2 ms (target 20 ms)
- 50 active medicines: evaluation about 141 ms (target 300 ms), `home_refresh` 91 to 94 ms (target 60 ms, missed). `hy_tile_view` costs about 1.8 ms per fixed medicine, 88 to 90 ms for 50 tiles; LVGL updates are 3 to 4 ms
- Picolibc was slower (3 medicines: evaluation 60 to 64 ms, `home_refresh` 33 ms; 50 medicines: 306 ms and 315 ms) because it re-parses the TZ string on every `localtime_r` and `mktime`; the project builds with newlib
- Internal heap free with 50 tiles: 44 to 49 KB (minimum 35 KB); with 3 medicines 97 to 151 KB

## Owed hardware checks

Verified by hand on firmware 2530f31 (2026-10-29): repeat chimes (5 plays at 2 min intervals, then silent), Play chime in Sound settings, detail scroll, due card dots, QWERTY note keyboard. Splash 1000 to 1009 ms with a 52 ms maximum frame gap over 3 boots. RTC alarm log line: hardware fire at the programmed second, software due-now fire within 1 s.

Still owed:
- Serial check A: waking stroke while a due card appears (`idle on`, `gesture hold`, `remindtest 0`, `gesture release left`: no snooze, nav stays `[due]`). The run on 2530f31 was inconclusive because real touches released the scripted hold
- Serial check B: idle Next after an early dose (take V early, `idle on`, the idle screen must not name the satisfied slot)

## Home grid

- Verified by hand on firmware 9c7539e: tile tap, Undo by finger, scroll with 5 medicines, and the panel's look against the mockup
- LVGL task stack minimum free: 6996 bytes

- Verified by hand on firmware 7e3466d: taps on other tiles while the logged toast shows do not log, Undo still removes the entry, and a second tap within 5 s after the too-soon warning logs the dose (serial log not captured for this check)

## Firmware update from GitHub

Measured on the board on firmware v0.4.0-16-ga1298af (debug build), 2026-11-02, on a Wi-Fi network without internet access, 3 active medicines, one live-event socket open.

- Check without internet: refused as `no_internet` after the 10 s connect timeout (10011 to 10032 ms over 10 runs); the live event shows `checking`, then `failed` with `no_internet`; `GET /api/update` follows the same states
- Check in access point mode: refused at once as `no_network`
- Internal heap for a check that ends at the connect timeout: free before 104975 to 105095 bytes, minimum during 85871 bytes (about 19 KB in use at the peak), free after within 1 KB of the value before on 7 of 10 runs and 1008 bytes lower on 3; free memory also moved by up to 3 KB with no run in progress, so no leak can be told apart at this resolution
- Update task stack: 5556 to 5596 bytes of 8192 left unused after a check (high-water mark about 2.6 KB)
- Certificate bundle (common set): 16742 bytes of flash in the release image
- Upload of the same image through `POST /api/ota`: 2408592 bytes written to the other slot in 25.6 s, worker stack 3392 bytes left, restart, confirmed 40 ms after the first scheduler pass; a text file answers 422 `not_hygeia` and an image cut at 600000 bytes answers 422 `corrupt`, both with the running slot unchanged

Not measured yet, because the network had no route to GitHub: the redirect host of a release asset and both TLS handshakes, the download duration, and the internal heap minimum and stack high-water mark during a full download and checksum
