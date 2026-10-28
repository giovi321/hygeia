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
- RTC kept time over 74 s with USB unplugged and, pending owner confirmation, the battery unplugged (`osc_stopped=0` afterwards); this implies a backup supply for the PCF85063

## Home grid

- Owner-verified on firmware a319c4e: tile tap, Undo by finger, scroll with 5 medicines, and the panel's look against the mockup
- LVGL task stack minimum free: 6996 bytes

