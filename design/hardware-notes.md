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
- Cabinet open cues are rendered at boot from note lists (sine, 5 ms ramps, 22050 Hz, then 100 ms of silence): GO 1319 Hz 120 ms; TAKE 880 Hz 80 ms twice with a 60 ms gap; REDO 440 Hz 400 ms; DONE 659, 880, 1319 Hz 120 ms each with 40 ms gaps; FAIL 659 then 440 Hz 250 ms each with a 60 ms gap. Each plays as its own open, write and close of the codec; up to 4 requests wait, the chime is dropped while anything plays. Every cue request logs `cue <name>`. Not yet heard on the board: that two cues in a row both play and that the rising and falling cues are told apart by ear

## Gestures

Thresholds that worked on the panel (hands-on session 2026-10-29, build 5867131):

- Hard edge zone: 40 px (top edge down, bottom edge up from y 440, left edge right). 16 px at the bottom over scrolling content. No widget can take a stroke that starts there, except sliders
- Swipe: dominant axis at least 60 px and at least 1.5 times the other axis
- Tap: at most 16 px of movement and under 500 ms; a long press is at most 16 px and 500 ms or more
- Double tap: 300 ms between release and the next press, 40 px between the two points
- A stroke that leaves and returns to the start is not a tap: `max_dev` (maximum deviation from the start) must also be 16 px or less, not only the end-point distance
- A stroke that starts in the hard edge zone is never consumed by a widget, except sliders, which take it with a forced consume

Not yet measured on the panel:

- Edge gesture zone: 80 px (`HY_EDGE_GESTURE_PX`), provisional. `hy_gesture_classify` gives EDGE_RIGHT, EDGE_DOWN and EDGE_UP for a swipe toward the inside that starts less than 80 px from the edge (the bottom edge from y 400). Widgets that consume their strokes (lists, keyboard keys, rollers, text fields) still take them from 40 px inward, so the wider band only reaches strokes no widget took. A stroke that stopped a glide is never an edge gesture
- Over a scrolling list the exception is a stroke that starts in the band and moves inward while the list is already at that end (top band moving down with the list at its top, bottom band moving up with the list at its bottom): the list does not consume it (`hy_edge_stroke_passes`, called from `ui_input_consume` at the scroll start), its elastic over-pull is switched off for the stroke (the `LV_OBJ_FLAG_SCROLL_ELASTIC` flag, restored at the release), and the classifier gives the edge kind. Owed on the board: that the list does not stretch on such a stroke and that a stroke at the other end of the list still scrolls it
- Device check without a finger: `gesture bandright|banddown|bandup consumed` simulates a band stroke that a widget consumes (it stays), without `consumed` the stroke navigates

### Edge gesture zone basis and measurement

Why 80 px: the logged corner touches (TL 33,28 and BL 24,460, see "Touch orientation") put the first reported point of a finger pressed against the edge 13 to 33 px in (the corner log is TL 33,28; TR 466,29; BL 24,460; BR 460,466, so the bottom and right edges report 13 to 19 px in; the 16 px bottom hard zone over lists sits inside that range). A fingertip pad is 10 to 14 mm wide and the panel has about 12.4 px per mm (480 px over 38.8 mm), so a finger that must start fully on the glass reports its centroid 5 to 7 mm in, 62 to 87 px. 80 px covers most of that range and stays clear of the middle of the screen. The measurement replaces it.

Measurement with `touch trace on` (debug build, console; `touch trace off` stops it; off at boot, not saved). Each stroke logs one line with the prefix `trace`: the first reported point, its time after the touch interrupt, the second reported point and its time after the first, and the end point. Procedure, done by the owner with the device in its corner or case:

1. `touch trace on`
2. 10 swipes per edge (left to right, top down, bottom up) that start off the glass, the finger sliding in from the border
3. 10 swipes per edge that start on the glass against the obstacle, the finger placed as close to the edge as the corner or case allows
4. Note the distance of the first point from the edge for each stroke (x for the left edge, y for the top edge, 479 minus y for the bottom edge) and which gesture the `stroke` line after it reports
5. Set `HY_EDGE_GESTURE_PX` to cover the large majority of the on-glass strokes, then replace "provisional" here and write the measured values in the style of the "Firmware update from GitHub" section

Not yet measured on the board.

## Motion sensor

QMI8658A at 0x6b, driver `components/hy_board/src/qmi8658.c`. Sources: QMI8658A datasheet Rev A (QST, 2022), section and table numbers below, and the SensorLib QMI8658 driver (`SensorQMI8658.hpp`, master, read 2026-11-03). Values marked as measured come from the board on firmware v0.4.1-24-g7ce90fb (debug build), 2026-11-04, device lying flat on an office desk, on USB; everything else is from the sources.

Modes:

- Accel only (rotation, shake, Cabinet open stream path while the screen is on): CTRL2 0x26, +-8 g, 125 Hz; scale 4096 LSB/g
- Capture (Taught movement, teach and test): CTRL2 0x26 (accel 112.1 Hz, synchronised to the gyro), CTRL3 0x66 (+-1024 dps, 112.1 Hz; scale 32 LSB/dps), CTRL5 0x77 (accel and gyro low-pass on, mode 3 = 13.37 % of ODR, about 15 Hz), CTRL7 0x03; polled at 50 Hz with one 12-byte burst 0x35..0x40. 6DOF current at 112.1 Hz: 752 to 753 uA typical (DS table 17)
- Wake on motion (Cabinet open live, screen off): CTRL2 0x2D, +-8 g, low-power accel ODR code 13 = 21 Hz (47.6 ms), the lowest low-power rate with a period of at most 50 ms. Low-power accel current 42 uA typical at 21 Hz, VDD 1.8 V (DS table 15; 55, 35 and 30 uA at 128, 11 and 3 Hz)

Wake-on-motion rule: the chip compares the absolute slope between successive samples, per axis, with the threshold, not the deviation from a reference (DS 12.2 table 39 "the threshold of absolute of slope", 12.1, 9.1 "Slope(n) = Acc(n) - Acc(n-1)"). That any one axis is enough is an inference: WoM has no axis or logic setting, and the Any-Motion default is OR. The host emulation is `hy_cab_wom_hit` with `hy_cab_wom_prev`, the sensor sample 48 ms back interpolated from the 50 Hz stream.

Register sequence to arm (DS 12.5 figure 25): CTRL7 0; CTRL2 0x2D; CAL1_L 0x0B = threshold in mg (1 to 255; 0 disables WoM); CAL1_H 0x0C = pin select (bit 6: 0 INT1, 1 INT2), initial level 0 (bit 7), blanking 4 samples (bits 5..0; 190 ms at 21 Hz, past the 3 / ODR settling); CTRL9 command 0x08 with the STATUSINT 0x2D bit 7 handshake (CTRL8 bit 7 = 1, so a command never pulses INT1); CTRL1 output enable for the chosen INT pin (bit 3 INT1, bit 4 INT2, push-pull); CTRL7 0x01. Exit (DS 12.6): CTRL7 0, CAL1_L 0, command 0x08, outputs high-Z. Every mode change goes through that exit. Interrupt behaviour per DS 12.4: the line toggles on each event and stays; reading STATUS1 0x2F (bit 2) clears the WoM bit and returns the line to its initial level. The firmware arms the light-sleep GPIO wake on the level opposite the one read at arming.

Interrupt pin: selectable at run time (`imu pin [1|2 [17|21]]` on the debug console). Default INT1 on GPIO 17, INT2 on GPIO 21 as the alternative. Measured: the chip's INT1 is wired to GPIO 17 (`imu wom 1`: GPIO 17 follows the chip's INT1 bit in STATUSINT, GPIO 21 stays low). The line toggles on each event and holds the new level: over 30 s at 1 mg, 655 level changes by 10 ms polling and 655 interrupt edges, one change every 40 to 50 ms (each 21 Hz sample over the threshold), STATUS1 0x04 at the end. Registers measured: powered down CTRL1 0x41, CTRL7 0x00, CTRL8 0x80; accel only CTRL1 0x40, CTRL2 0x26, CTRL7 0x01; armed CTRL1 0x48, CTRL2 0x2d, CTRL7 0x01.

Gyro: range +-1024 dps, values in deci-dps (raw x 10 / 32). Turn-on per datasheet: t1 typical 150 ms plus 3 / ODR (27 ms at 112.1 Hz), about 177 ms (DS table 8, 7.3). The firmware masks the gyro for the first 220 ms of every sequence (`HY_CAB_GYRO_START_MS`), teach and live alike, and holds the accel samples of the same span at the trigger sample, the last good reading taken before the switch to accel and gyro (on the wake-on-motion path one reading from the armed low-power stream, taken right after the interrupt and before the switch, which starts the capture and is its sample 0 as on the stream path; the rest vector stands in when that read fails; the debug log prints that read minus the rest vector as `trigger read ok, minus rest x y z mg`, or `failed, rest used`), so the capture segmenter and the features never see the start-up disturbance (`hy_cab_cap_feed_held`).

Gyro measured (`imu cap`, 11 runs, board still):

- After `hy_board_imu_capture_on` the data registers keep their last values (not zero) until the first new sample, 100 to 140 ms later. The `imu cap` line therefore reports the first new sample (the first read that differs from the registers read right after the switch) and the settle time (end of the last sample with an accel step of 200 mg or more or a gyro over 10 dps)
- The first new samples are disturbed on accel and gyro alike (accel up to 1.7 g, gyro up to 108 dps on a still board); the last disturbed sample was at 180 to 201 ms, settled from 220 ms on every run. `HY_CAB_GYRO_START_MS` is 220 (11 runs, rounded up to 20 ms), and the accel is held over the same time
- Re-measured on firmware v0.4.1-28-g8b697c9 (debug build), 2026-11-04, 10 runs of `imu cap 1`: first new sample at 119 to 141 ms, settled at 199 to 220 ms by the firmware's rule (6 runs at 220 ms, 4 at 199 to 200 ms). The 220 ms mask covers every run, with no margin on 6 of them
- Still noise over 5 s after settling: largest absolute value 1.2, 2.3 and 1.0 dps (x, y, z), mean offset 0.8, -2.0 and -0.5 dps
- The sign of z for a clockwise turn is not measured yet (needs the board turned by hand)

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
- Wake-on-motion noise measured in low-power mode (`imu wom`, 30 s each, device still on a desk): 655 events at 1 mg, 534 at 4 mg, 225 at 8 mg, 10 at 16 mg, 0 at 40 mg. Live at 40 mg over about 20 minutes in an office with people at the desk: 7 interrupts. The 50 Hz stream on the same desk: largest slope between successive samples 8 mg over 60 s
- The slope of a real cabinet door is to be measured

Taught movement matcher margins at sensitivity 3 (`test_cab_rec.c`, `test_cabmatch.c`; these were made with 3 takes, the device now teaches 5, and the host tests build the 3-take models with `hy_cab_model_build_n`):

| Model | d_self | Threshold | Largest positive | Smallest negative |
| --- | --- | --- | --- | --- |
| putdown_pickup (recording, accel only) | 1.898 | 2.000 (clamped at the upper bound) | 1.053 (augmented variants) | 3.072 (carry_walk window at 60 s) |
| synthetic door open | 0.345 | 0.552 | 0.289 | 1.880 (door close) |
| synthetic tilt | 0.706 | 1.129 | 0.629 | 4.369 (door open) |
| synthetic lift | 0.232 | 0.372 | 0.200 | 2.545 (door open) |

- Threshold bounds 0.25 and 2.0; the upper bound sits a third below the nearest recording negative. The putdown_pickup model rests on 3 takes from one recording with rests padded in the replay; no held-out real positive exists yet
- Capture: from the trigger until 400 ms of stillness (accel steps within 40 mg around the current pose, gyro under 10 dps), 220 ms minimum, 4 s maximum. The recorded taps capture as 20 to 60 ms, so a tap cannot be taught
- rotate_cycle and tap_double give fewer than 3 captures and have no model

The template file holds 5 takes: at most 32 + 5 x (8 + 200 x 12) + 4 = 12076 bytes (`HY_CAB_FILE_MAX`); the model in memory is about 12 KB, held in PSRAM, never on a stack. A file with 3 or 4 takes still loads.

Measured on the board: motion task stack free minimum 3736 bytes of 6144 (debug build, after reactions, test mode and a teach); the release build was not measured. DIRAM used: 114977 bytes (debug), 113921 bytes (release). At boot the motion task arms wake on motion for about 100 ms before the screen comes on (`cabinet: armed any 40 mg`, then `motion: on`).

CTRL9 handshake wedge and recovery:

- Observed on the board (debug build, USB, no flag, 2026-11-05, once, not reproduced in three further cycles): after `scr off` and a wake, `motion: imu power up failed: ESP_ERR_TIMEOUT`, then `imu: ctrl9 0x08: ESP_ERR_TIMEOUT` every 1.1 s, then `cabinet: imu fault, feature paused`. `imu regs` read CTRL1 0x41, CTRL2 0x2D, CTRL7 0x00, CTRL8 0x80, CTRL9 0x00, STATUSINT 0x80: the chip was still in the wake-on-motion setting (CTRL2 0x2D), the acknowledge 0x00 was in CTRL9 and CmdDone (STATUSINT bit 7) was set and did not clear. A reset cleared it
- Path: the screen wake runs the motion task's power up, which first leaves wake on motion (`mode_exit`: CTRL7 0, threshold 0, the WoM command 0x08 through CTRL9). The CTRL9 handshake timed out (100 ms), the mode stayed "wake on motion" so every later call repeated the same handshake and timed out the same way, and three I2C errors in a row paused Cabinet open until a settings change. The trigger is unknown. The handshake is command, wait for STATUSINT bit 7, write 0x00, wait for bit 7 to clear (DS 5.10.1, 5.10.4); a lost or half done handshake leaves bit 7 set, which also passes for the answer of the next command if it is not acknowledged first
- Recovery in the driver (`ctrl9`): a CmdDone that is already set is acknowledged before a command; a timed out handshake is retried twice, each after an acknowledge (the retry count is in the log: `ctrl9 0x08: ESP_ERR_TIMEOUT, retry 1 of 2 after an acknowledge`, `recovered on retry N`); if it is still stuck the chip gets a soft reset (RESET 0xB0, RST_RESULT 0x80) and the powered-down configuration of the start-up, which drops the WoM setting (log: `soft reset and configure again`). A WoM exit counts as done after that reset; a WoM arm is repeated once on the reset chip. A failed exit that is not recovered still leaves the mode "wake on motion" so the next power change repeats it, as before
- Recovery in Cabinet open: a paused feature is tried again at the next screen wake, at most four times an hour and at least 60 s apart (`hy_cabretry`), with the sensor start-up repeated if the sensor was lost. Each decision is logged (`cabinet: imu fault, retrying at the screen wake (attempt 1 of 4 this hour, ...)` or `no retry at this wake (next in N s ...)`). A settings change still retries at once
- Owed (board): the wedge cannot be provoked on purpose with a known recipe. To test the recovery, hold the I2C bus off the chip during a WoM exit (for example a long LVGL block from `shot` while the screen wakes from a sleep mode, as in the run where it appeared) and check the `ctrl9` retry and reset lines and that Cabinet open is live after the wake

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

## Battery investigation tools

Debug console only (the release build has none of this code). The tools split the battery draw into parts so each part can be measured on the board. No value is measured yet: every number below is owed to the owner runs, and no battery estimate is published until they exist.

The AXP2101 has no battery current register and no coulomb counter. Its ADC result registers (0x34 to 0x3D) hold battery voltage, TS, VBUS voltage, VSYS voltage and die temperature, and the fuel gauge (0xA4) moves in 1% steps. The battery voltage at 1 mV resolution, one reading a minute, is the proxy: a run is read as the slope of that voltage, not as a current.

Always on in the debug build, nothing to switch:
- `lsstat` (printed by `sleepstat` and by `pwrx`): light-sleep entries, the entries that ran, the entries that were rejected or woke at once (IDF reports the time spent in the reject path as a sleep, so a sleep under 1 ms counts here, apart from the sleeps that ran, and a wake pin that is already active shows up as many of these), the entries too short to try (an idle period under the minimum, no sleep attempted), the real time in light sleep, that time as a share of the screen-OFF-asleep time (residency; with `lsvbus` it can pass 100%, because the sleep on USB counts here and the per-state clock drops VBUS time), a histogram of the requested sleep length (under 50 ms, 50 to 200, 200 to 600, 600 to 1100, 1100 to 2100, 2100 ms and over; the bucket names the period of the waker), the hardware wake cause of each sleep that ran (timer, GPIO, other), the loops of the power, net and console tasks while a light-sleep mode is on, and the time on battery in each screen state (ON, DIM, OFF asleep, OFF held by the radio, OFF awake for another reason). The counters sit in RTC slow memory and follow the `sleepstat` rules: kept over a software, panic, watchdog or brownout reset, cleared by a power-on reset. They need `CONFIG_PM_LIGHT_SLEEP_CALLBACKS`, set in the debug build only; an existing local `sdkconfig` keeps the old value, and `lsstat` then warns and prints zeros
- `pwrx mv`: the last 256 battery readings, one a minute (awake or asleep), in RTC memory, as CSV lines (`mv,<seconds since boot>,<millivolts>,<percent>,<screen state>` under the header `tag,t_s,mv,pct,scr`). The point carries no VBUS state: after a replug a charging point looks like a battery point until the voltage jumps. 256 points cover 4 h 16 min. `pwrx mv clear` empties the ring. The seconds restart at each boot
- `pmu rails`: read-only dump of the rail enable bits (0x80 DCDC1 to 5, 0x90 ALDO1 to 4, BLDO1 and 2, CPUSLDO, DLDO1, 0x91 DLDO2), the raw voltage codes (0x82 to 0x86, 0x92 to 0x9A) and the charge indicator 0x69. The bit order is from XPowersLib and is not yet checked against this board; the voltage codes are not converted

Flags (`pwrx <flag> on|off`, `pwrx off`, `pwrx`): kept in RAM, all off at every boot, named in the first line of every `sleepstat`. A flag acts from the next sleep or screen off, not retroactively.

| Flag | What it does | Wake-source risk |
|---|---|---|
| `touch` | At light-sleep entry (battery only, or with `lsvbus`) the touch controller is held in reset (GPIO 40 low, held through the sleep) and GPIO 11 is left out of the wake set; at the end of the sleep the controller gets its start sequence again (reset released, 50 ms, command mode write 0xD101), which makes each wake about 60 ms longer. The touch driver can release the reset line by itself after a failed read; the power task checks the line in the sleep loop, counts each release, pulls the line low again and logs a warning, and `sleepstat` and `pwrx` then print "pwrx touch: INVALID" until the flag is turned on again. A run with that line is not used | No touch wake and no double tap while on. PWR key, user key, RTC alarm and the motion sensor still wake. A touch that fell inside the sleep is not seen |
| `nonet` | On battery the radio counts as idle at once and Keep Wi-Fi on battery is ignored, so the sleep mode is entered at the first screen off | None. The web UI is unreachable on battery. Turning the flag off does not restore the radio: it stays off on battery until USB or a Wi-Fi mode change |
| `panel` | After Sleep In the panel reset line (GPIO 39) is held low; at the next panel on the full board support package init runs (reset pulse, vendor table, brightness back to 0, held MADCTL resent) | None for wakes. Waking the screen takes about 1.4 s longer, and the vendor table switches the panel on at full brightness for part of that time, so the panel shows a full-brightness flash of undefined or old content for about 0.6 s (certain, not possible; the measurement is not affected) |
| `wakers` | The net task evaluates every 30 s instead of 1 s while the radio is off on battery, the console poll without a USB host waits 30 s instead of 2 s, and the power task loop in light sleep waits 5 s instead of 1 s | A PWR press, a VBUS change or a battery low or critical crossing is noticed up to 5 s late (the PMU interrupt is not a wake pin; the AXP2101 keeps the status bits until they are read). The radio can start up to 30 s late after a USB plug, a Wi-Fi start from Quick settings on battery waits for that tick, and the console answers up to 30 s late after a replug (type the command before replugging). The lwIP timers (ARP, ND6, DHCP coarse, 1 s each) are not changed |
| `lsvbus` | Light sleep is allowed with VBUS present, and the W_USB end of the sleep mode on a VBUS mismatch is skipped for that sleep mode | The USB Serial/JTAG pads are disabled in light sleep, so the PC console drops while the screen is off: use a wall charger, and wake with PWR. Turn Wi-Fi off first (`wifi mode off`): on USB the radio follows the saved mode and holds the sleep mode off, so the run would measure the awake current; the log warns when that happens. Charging current is part of the supply current unless the charger reports done |

Left out: a `codec` flag (ES7210 power down and an ES8311 open and close cycle). The ES7210 is never initialised by the firmware and the driver call needs a device handle the firmware does not create; the change was neither cheap nor verifiable without the board.

Owed to the owner runs: the screen-off floor, the touch controller scan current (slope with and without `touch`), the share of time awake in normal use, the radio tail after unplugging, the per-flag effect of `panel` and `wakers`, and whether the rail dump shows a rail that feeds only the panel, the SD slot or the codecs. The USB meter path (a wall charger, a full battery and `lsvbus` on) gives a per-flag current in minutes if a meter with 0.1 mA resolution is available.
