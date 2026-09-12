# Waveshare ESP32-S3-Touch-LCD-7

This profile targets the Waveshare ESP32-S3-Touch-LCD-7 (800x480 ST7262 RGB
panel, GT911 capacitive touch, CH422G IO expander). It is unrelated to the
ESP32-P4 "Waveshare Touch LCD 7" profile (`waveshare_touch_lcd_7`), which is
a different chip, display interface (MIPI-DSI), and panel controller
(ILI9881C) despite the similar product name.

**No maintainer or contributor hardware exists for this board.** This
profile is added from vendor documentation and the vendor's official example
firmware only; it compiles but is entirely unverified on real hardware.

## Profile and build

- Profile and release key: `waveshare_s3_touch_lcd_7`
- Device define: `DEVICE_WAVESHARE_S3_TOUCH_LCD_7`
- ESP32-S3-WROOM-1-N8R8: 8MB flash, 8MB octal PSRAM (smaller than every other
  HomeTiles device; uses its own `partitions_8mb.csv`, not the shared
  16MB+ `partitions.csv`)
- 800×480 ST7262 RGB-parallel display (no command interface), 16MHz pixel
  clock
- GT911 capacitive touch, I2C on SDA GPIO8 / SCL GPIO9, interrupt on GPIO4
- CH422G IO expander (I2C, same bus as touch) drives LCD reset (EXIO3),
  backlight on/off (EXIO2), touch reset (EXIO1), and SD chip-select (EXIO4)
- Backlight is an on/off switch line only. There is no PWM dimming on this
  board; `setBrightness()` treats any non-zero value as "on".
- microSD over SPI: MOSI GPIO11, SCK GPIO12, MISO GPIO13. Chip-select is on
  the CH422G expander rather than a native GPIO; it is asserted once and
  held low for the session, since nothing else shares this SPI bus.
- Internal LittleFS for runtime files, with SD-to-LittleFS migration like
  the other SD-capable ESP32-S3 profiles

Use the exact Arduino IDE settings in
[BOARD_SETTINGS.md](../../../BOARD_SETTINGS.md). The local build profile is
`waveshare_s3_touch_lcd_7`.

## Hardware validation and limitations

**Validation pending — no maintainer hardware.** Display bring-up, touch,
backlight, SD card, Wi-Fi, and Web OTA are all unverified on physical
hardware. Only a successful compile has been confirmed; per this
repository's rules, a compile does not establish runtime or hardware
behavior.

The board also exposes RS485 (GPIO16/15) and CAN/TWAI (GPIO20/19, shared
electrically with USB and switched by CH422G EXIO5) headers. Neither is
wired into any HomeTiles tile or IO-option feature in this first bring-up.

## Primary hardware references

- [Waveshare product documentation](https://docs.waveshare.com/ESP32-S3-Touch-LCD-7)
- [Waveshare product page](https://www.waveshare.com/esp32-s3-touch-lcd-7.htm)
- [Official board examples](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7)
- [ST7262 datasheet](https://files.waveshare.com/wiki/common/ST7262.pdf)
- [GT911 datasheet](https://files.waveshare.com/wiki/common/GT911_EN_Datasheet.pdf)
