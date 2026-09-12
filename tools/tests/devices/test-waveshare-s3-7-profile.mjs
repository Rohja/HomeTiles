import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');

function read(relativePath) {
  return fs.readFileSync(path.join(repoRoot, relativePath), 'utf8')
    .replace(/\r\n?/g, '\n');
}

function requireMarker(source, marker, label) {
  if (!source.includes(marker)) {
    throw new Error(`${label} is missing: ${marker}`);
  }
}

function constantValue(source, name) {
  const match = source.match(new RegExp(
    `constexpr\\s+(?:u?int(?:8|16|32)_t|size_t)\\s+${name}\\s*=\\s*([^;]+);`));
  if (!match) throw new Error(`Constant was not found: ${name}`);
  return match[1].trim();
}

const header = read(
  'src/devices/waveshare_s3_touch_lcd_7/device_waveshare_s3_touch_lcd_7.h');
const driver = read(
  'src/devices/waveshare_s3_touch_lcd_7/device_waveshare_s3_touch_lcd_7.cpp');
const hardwareIo = read(
  'src/devices/waveshare_s3_touch_lcd_7/hardware_io_profile.h');
const deviceSelect = read('src/devices/device_select.h');

for (const marker of [
  'namespace DeviceWaveshareS3TouchLCD7',
  '"waveshare_s3_touch_lcd_7"',
  '"Waveshare ESP32-S3-Touch-LCD-7"',
  '    800,\n    480,\n    5,\n    4,',
  'Device::RotationStepMode::FlipOnly,\n    0,\n    2,',
  'Device::Capabilities{false, false, false, false, false, false}',
]) {
  requireMarker(header, marker, 'Waveshare S3 7 device profile');
}

// Every pin/timing value here traces to the vendor's own
// BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7.h and waveshare_rgb_lcd_port.h
// (github.com/waveshareteam/ESP32-S3-Touch-LCD-7), independently confirmed
// by both files.
const expectedConstants = new Map([
  ['kPanelHsync', '46'],
  ['kPanelVsync', '3'],
  ['kPanelDe', '5'],
  ['kPanelPclk', '7'],
  ['kRgbPclkHz', '16000000'],
  ['kRgbHsyncPulseWidth', '4'],
  ['kRgbHsyncBackPorch', '8'],
  ['kRgbHsyncFrontPorch', '8'],
  ['kRgbVsyncPulseWidth', '4'],
  ['kRgbVsyncBackPorch', '8'],
  ['kRgbVsyncFrontPorch', '8'],
  ['kTouchInterruptPin', '4'],
  ['kTouchSda', '8'],
  ['kTouchScl', '9'],
  ['kTouchAddressPrimary', '0x5D'],
  ['kTouchAddressAlternate', '0x14'],
  ['kExpanderSda', '8'],
  ['kExpanderScl', '9'],
  ['kSdSck', '12'],
  ['kSdMiso', '13'],
  ['kSdMosi', '11'],
  ['kExpectedFlashBytes', '8U * 1024U * 1024U'],
  ['kExpectedPsramBytes', '8U * 1024U * 1024U'],
]);
for (const [name, expected] of expectedConstants) {
  const actual = constantValue(driver, name);
  if (actual !== expected) {
    throw new Error(`${name} mismatch: expected ${expected}, got ${actual}`);
  }
}

requireMarker(driver,
  'constexpr int8_t kPanelDataPins[16] = {14, 38, 18, 17, 10, 39, 0, 45,\n'
    + '                                       48, 47, 21, 1,  2,  42, 41, 40};',
  'RGB data pin order (vendor DATA0..DATA15)');

// CH422G EXIO pin assignments, from the vendor's own SD-card example header
// (waveshare_sd_card.h: TP_RST=1, LCD_BL=2, LCD_RST=3, SD_CS=4, USB_SEL=5).
for (const [name, expected] of [
  ['kExioTouchReset', '1U << 1'],
  ['kExioBacklight', '1U << 2'],
  ['kExioLcdReset', '1U << 3'],
  ['kExioSdCs', '1U << 4'],
]) {
  const actual = constantValue(driver, name);
  if (actual !== expected) {
    throw new Error(`${name} mismatch: expected ${expected}, got ${actual}`);
  }
}

// CH422G is a different chip/protocol from the TCA9554-class expander used
// by waveshare_s3_touch_lcd_4b: pseudo-addresses, not one register-addressed
// device.
for (const marker of [
  'constexpr uint8_t kExpanderAddrSet = 0x48 >> 1;',
  'constexpr uint8_t kExpanderAddrIo = 0x70 >> 1;',
]) {
  requireMarker(driver, marker, 'CH422G pseudo-address protocol');
}

for (const forbidden of ['Arduino_XCA9554SWSPI', 'XCA9554', 'kExpanderPanelCs', 'ledcAttach', 'ledcWrite']) {
  if (driver.includes(forbidden)) {
    throw new Error(`Waveshare S3 7 driver contains forbidden marker: ${forbidden} (wrong expander/backlight model)`);
  }
}

for (const marker of [
  'bool DeviceWaveshareS3TouchLCD7::sdReady() {\n  return g_sd_available && SD.cardType() != CARD_NONE;\n}',
  'SD.begin(-1, g_sd_spi, kSdFrequency, "/sdcard", 5)',
]) {
  requireMarker(driver, marker, 'SD-over-expander-CS contract');
}

requireMarker(hardwareIo, 'kHardwareIoProfile{}', 'Hardware I/O profile');
if (/\{\s*\d+\s*,/.test(hardwareIo)) {
  throw new Error('Reserved Waveshare S3 7 pins must not be exposed as I/O');
}

for (const marker of [
  'DEVICE_WAVESHARE_S3_TOUCH_LCD_7',
]) {
  requireMarker(deviceSelect, marker, 'Device selection wiring');
}
requireMarker(read('src/devices/device_select.h').match(
  /#if defined\(DEVICE_GUITION_ESP32_4848S040\)[^]*?#define DEVICE_ESP32_S3_RGB_480/)[0],
  'DEVICE_WAVESHARE_S3_TOUCH_LCD_7', 'ESP32-S3 RGB family membership');
requireMarker(read('src/devices/device_select.h').match(
  /#if defined\(DEVICE_LAYOUT_TEST_480X480\)[^]*?#define DEVICE_LAYOUT_480X480/)[0],
  'DEVICE_WAVESHARE_S3_TOUCH_LCD_7', 'Compact UI layout family membership');

console.log('Waveshare ESP32-S3-Touch-LCD-7 profile contract: PASS');
