#include "src/devices/waveshare_s3_touch_lcd_7/device_waveshare_s3_touch_lcd_7.h"
#include "src/devices/device_select.h"

#if defined(DEVICE_WAVESHARE_S3_TOUCH_LCD_7)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_phy_init.h>
#include <esp_private/periph_ctrl.h>
#include <hal/lcd_ll.h>

#include <algorithm>
#include <cstring>
#include <iterator>

// Primary sources for every pin/timing/protocol constant in this file:
// - https://docs.waveshare.com/ESP32-S3-Touch-LCD-7 and its Resources page
//   (ST7262 + GT911 + ESP32-S3-WROOM-1 datasheets, CH422G EXIO pin table).
// - https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7 official example
//   repository, specifically:
//   examples/Arduino/libraries/ESP32_Display_Panel/src/board/supported/
//   waveshare/BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7.h (pin/timing macros),
//   examples/ESP-IDF/09_lvgl_v9_demo/components/waveshare_rgb_lcd_port.{c,h}
//   (independent confirmation + CH422G bring-up sequence),
//   examples/Arduino/libraries/ESP32_IO_Expander/src/port/
//   esp_io_expander_ch422g.c (CH422G register protocol), and
//   examples/Arduino/examples/03_SD_Test/waveshare_sd_card.h (SD pins and
//   the exact CH422G EXIO-pin numbering used below).

namespace {

// --- CH422G IO expander -----------------------------------------------
// This board uses a CH422G, not the TCA9554-class expander used by the
// Waveshare ESP32-S3-Touch-LCD-4B. The CH422G is not register-addressed:
// each function is its own pseudo-I2C address. Only the two addresses
// HomeTiles needs are implemented here (WR_SET to switch all EXIO pins to
// push-pull output, WR_IO to drive EXIO0-7).
constexpr uint8_t kExpanderAddrSet = 0x48 >> 1;  // WR_SET
constexpr uint8_t kExpanderAddrIo = 0x70 >> 1;   // WR_IO (EXIO0-7 outputs)
constexpr uint8_t kExpanderSetAllOutputs = 0x01; // IO_OE bit

// EXIO pin assignments (from the vendor SD-card example header).
constexpr uint8_t kExioTouchReset = 1U << 1;  // EXIO1 = TP_RST
constexpr uint8_t kExioBacklight = 1U << 2;   // EXIO2 = LCD_BL
constexpr uint8_t kExioLcdReset = 1U << 3;    // EXIO3 = LCD_RST
constexpr uint8_t kExioSdCs = 1U << 4;        // EXIO4 = SD_CS (active low)

constexpr int8_t kExpanderSda = 8;
constexpr int8_t kExpanderScl = 9;
constexpr uint32_t kExpanderI2cFrequency = 400000;

bool g_expander_ready = false;
uint8_t g_expander_output_bits = 0;

bool expanderWriteOutputs(uint8_t bits) {
  Wire.beginTransmission(kExpanderAddrIo);
  Wire.write(bits);
  if (Wire.endTransmission() != 0) return false;
  g_expander_output_bits = bits;
  return true;
}

bool expanderSetPin(uint8_t mask, bool level) {
  const uint8_t next = level ? static_cast<uint8_t>(g_expander_output_bits | mask)
                             : static_cast<uint8_t>(g_expander_output_bits & ~mask);
  return expanderWriteOutputs(next);
}

bool initExpander() {
  if (g_expander_ready) return true;
  Wire.begin(kExpanderSda, kExpanderScl, kExpanderI2cFrequency);

  Wire.beginTransmission(kExpanderAddrSet);
  Wire.write(kExpanderSetAllOutputs);
  if (Wire.endTransmission() != 0) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] CH422G not responding");
    return false;
  }

  // Hold the LCD and touch controller in reset, keep the backlight off and
  // the SD card deselected (CS idle-high) until each subsystem is brought
  // up explicitly.
  if (!expanderWriteOutputs(kExioSdCs)) return false;
  g_expander_ready = true;
  return true;
}

// --- RGB panel (ST7262) -------------------------------------------------
constexpr int8_t kPanelHsync = 46;
constexpr int8_t kPanelVsync = 3;
constexpr int8_t kPanelDe = 5;
constexpr int8_t kPanelPclk = 7;
// B0-4, G0-5, R0-4 for 16-bit RGB565, in vendor DATA0..DATA15 order.
constexpr int8_t kPanelDataPins[16] = {14, 38, 18, 17, 10, 39, 0, 45,
                                       48, 47, 21, 1,  2,  42, 41, 40};

constexpr uint16_t kScreenWidth = 800;
constexpr uint16_t kScreenHeight = 480;

// Exact values from the vendor BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7.h and
// independently confirmed by the ESP-IDF example's waveshare_rgb_lcd_port.h.
constexpr uint32_t kRgbPclkHz = 16000000;
constexpr uint32_t kRgbHsyncPulseWidth = 4;
constexpr uint32_t kRgbHsyncBackPorch = 8;
constexpr uint32_t kRgbHsyncFrontPorch = 8;
constexpr uint32_t kRgbVsyncPulseWidth = 4;
constexpr uint32_t kRgbVsyncBackPorch = 8;
constexpr uint32_t kRgbVsyncFrontPorch = 8;
constexpr uint32_t kRgbHorizontalTotal = kScreenWidth + kRgbHsyncPulseWidth +
                                         kRgbHsyncBackPorch +
                                         kRgbHsyncFrontPorch;
constexpr uint32_t kRgbVerticalTotal = kScreenHeight + kRgbVsyncPulseWidth +
                                       kRgbVsyncBackPorch +
                                       kRgbVsyncFrontPorch;
constexpr uint32_t kRgbFramePeriodMs =
    ((kRgbHorizontalTotal * kRgbVerticalTotal * 1000U) + kRgbPclkHz - 1U) /
    kRgbPclkHz;

constexpr int8_t kTouchInterruptPin = 4;  // Direct ESP32 GPIO (TP_IRQ).
constexpr int8_t kTouchSda = 8;
constexpr int8_t kTouchScl = 9;
constexpr uint32_t kTouchFrequency = 400000;
constexpr uint8_t kTouchAddressPrimary = 0x5D;
constexpr uint8_t kTouchAddressAlternate = 0x14;
constexpr uint16_t kTouchProductIdRegister = 0x8140;
constexpr uint16_t kTouchStatusRegister = 0x814E;
constexpr uint16_t kTouchPointRegister = 0x814F;
// See waveshare_s3_touch_lcd_4b for the rationale: keep a held point across
// normal GT911 "no new frame" polls, but do not let a broken I2C/data path
// leave LVGL pressed forever.
constexpr uint8_t kTouchErrorReleaseThreshold = 16;

// ESP32-S3-WROOM-1-N8R8 per the vendor docs: 8 MB flash, 8 MB octal PSRAM.
constexpr uint32_t kExpectedFlashBytes = 8U * 1024U * 1024U;
constexpr uint32_t kExpectedPsramBytes = 8U * 1024U * 1024U;

constexpr int8_t kSdSck = 12;
constexpr int8_t kSdMiso = 13;
constexpr int8_t kSdMosi = 11;
// CS lives on the CH422G expander, not a native GPIO; SPIClass and SD.begin()
// are given no CS pin of their own (matches the vendor's own SD example,
// which asserts CS once through the expander and leaves it low for the
// whole session since nothing else shares this SPI bus).
constexpr uint32_t kSdFrequency = 20000000;
constexpr uint32_t kSdFallbackFrequency = 1000000;
constexpr uint32_t kSdRetryMs = 1500;

#if (defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) && CONFIG_SPIRAM_XIP_FROM_PSRAM) || \
    ((defined(CONFIG_SPIRAM_FETCH_INSTRUCTIONS) && CONFIG_SPIRAM_FETCH_INSTRUCTIONS) && \
     (defined(CONFIG_SPIRAM_RODATA) && CONFIG_SPIRAM_RODATA))
#define HOMETILES_WAVESHARE_S3_7_HAS_PSRAM_XIP 1
#else
#define HOMETILES_WAVESHARE_S3_7_HAS_PSRAM_XIP 0
#endif

#if defined(CONFIG_ESP32S3_DATA_CACHE_LINE_64B) && \
    CONFIG_ESP32S3_DATA_CACHE_LINE_64B
#define HOMETILES_WAVESHARE_S3_7_HAS_CACHE_LINE_64B 1
#else
#define HOMETILES_WAVESHARE_S3_7_HAS_CACHE_LINE_64B 0
#endif

#if HOMETILES_WAVESHARE_S3_7_HAS_PSRAM_XIP && \
    HOMETILES_WAVESHARE_S3_7_HAS_CACHE_LINE_64B
#define HOMETILES_WAVESHARE_S3_7_RGB_MODE_LABEL "xip-bounce10"
#define HOMETILES_WAVESHARE_S3_7_RGB_BOUNCE_ROWS 10
#else
#define HOMETILES_WAVESHARE_S3_7_RGB_MODE_LABEL "direct-flash-guard"
#define HOMETILES_WAVESHARE_S3_7_RGB_BOUNCE_ROWS 0
#endif

constexpr size_t kRgbBounceBufferPixels =
    kScreenWidth * HOMETILES_WAVESHARE_S3_7_RGB_BOUNCE_ROWS;
constexpr bool kHasPsramXip = HOMETILES_WAVESHARE_S3_7_HAS_PSRAM_XIP != 0;
constexpr bool kHasCacheLine64 =
    HOMETILES_WAVESHARE_S3_7_HAS_CACHE_LINE_64B != 0;

// ST7262 has no command/register interface: there is no panel init table.
// Reset is driven entirely through the CH422G (EXIO3) before this class'
// begin() runs.
class WaveshareRgbDisplay final : public Arduino_RGB_Display {
 public:
  explicit WaveshareRgbDisplay(uint8_t rotation)
      : Arduino_RGB_Display(kScreenWidth, kScreenHeight, nullptr, rotation,
                            true, nullptr, GFX_NOT_DEFINED, nullptr, 0) {}

  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    (void)speed;

    esp_lcd_rgb_panel_config_t config{};
    config.clk_src = LCD_CLK_SRC_DEFAULT;
    config.timings.pclk_hz = kRgbPclkHz;
    config.timings.h_res = kScreenWidth;
    config.timings.v_res = kScreenHeight;
    config.timings.hsync_pulse_width = kRgbHsyncPulseWidth;
    config.timings.hsync_back_porch = kRgbHsyncBackPorch;
    config.timings.hsync_front_porch = kRgbHsyncFrontPorch;
    config.timings.vsync_pulse_width = kRgbVsyncPulseWidth;
    config.timings.vsync_back_porch = kRgbVsyncBackPorch;
    config.timings.vsync_front_porch = kRgbVsyncFrontPorch;
    config.timings.flags.hsync_idle_low = 0;
    config.timings.flags.vsync_idle_low = 0;
    config.timings.flags.de_idle_high = 0;
    config.timings.flags.pclk_active_neg = 1;
    config.timings.flags.pclk_idle_high = 0;
    config.data_width = 16;
    config.bits_per_pixel = 16;
    config.num_fbs = 2;
    config.bounce_buffer_size_px = kRgbBounceBufferPixels;
    config.sram_trans_align = 8;
    config.psram_trans_align = 64;
    config.hsync_gpio_num = kPanelHsync;
    config.vsync_gpio_num = kPanelVsync;
    config.de_gpio_num = kPanelDe;
    config.pclk_gpio_num = kPanelPclk;
    config.disp_gpio_num = GPIO_NUM_NC;
    std::copy(std::begin(kPanelDataPins), std::end(kPanelDataPins),
              config.data_gpio_nums);
    config.flags.disp_active_low = false;
    config.flags.refresh_on_demand = false;
    config.flags.fb_in_psram = true;
    config.flags.double_fb = true;
    config.flags.no_fb = false;
    config.flags.bb_invalidate_cache = false;

    esp_err_t err = esp_lcd_new_rgb_panel(&config, &panel_handle_);
    if (err == ESP_OK) {
      esp_lcd_rgb_panel_event_callbacks_t callbacks{};
      callbacks.on_vsync = onVsync;
      callbacks.on_frame_buf_complete = onFrameComplete;
      err = esp_lcd_rgb_panel_register_event_callbacks(panel_handle_,
                                                        &callbacks, this);
    }
    if (err == ESP_OK) err = esp_lcd_panel_reset(panel_handle_);
    if (err == ESP_OK) err = esp_lcd_panel_init(panel_handle_);
    if (err == ESP_OK) {
      // panel_init() starts the stream and enables VSYNC. Mask immediately,
      // before any further setup work can let Arduino-ESP32's automatic
      // CONFIG_LCD_RGB_RESTART_IN_VSYNC path fire once at a random phase.
      maskVsyncInterrupt();
    }
    if (err == ESP_OK) {
      err = esp_lcd_rgb_panel_get_frame_buffer(
          panel_handle_, 2, reinterpret_cast<void**>(&framebuffers_[0]),
          reinterpret_cast<void**>(&framebuffers_[1]));
    }
    if (err != ESP_OK || !framebuffers_[0] || !framebuffers_[1]) {
      Serial.printf(
          "[Display/S3] Double framebuffer init failed: %s (0x%X)\n",
          esp_err_to_name(err), static_cast<unsigned>(err));
      return false;
    }

    active_index_ = 0;
    pending_index_ = 0;
    atomic_pending_ = false;
    canonical_fb0_valid_ = true;
    _framebuffer = framebuffers_[0];
    return true;
  }

  bool beginAtomicFrame(const char* reason) {
    if (!panel_handle_ || !framebuffers_[0] || !framebuffers_[1]) {
      return false;
    }
    if (storage_transition_) return false;
    if (atomic_pending_) return true;

    pending_index_ = active_index_ ^ 1U;
    if (pending_index_ == 0) canonical_fb0_valid_ = false;
    _framebuffer = framebuffers_[pending_index_];
    atomic_pending_ = true;
    atomic_started_ms_ = millis();
    atomic_reason_ = reason ? reason : "unknown";
    return true;
  }

  bool commitAtomicFrame() {
    if (!atomic_pending_ || !panel_handle_) return false;

    flush(true);
    if (pending_index_ == 0) canonical_fb0_valid_ = true;
    const uint32_t eof_start = frame_complete_count_;
    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        panel_handle_, 0, 0, _fb_width, _fb_height,
        framebuffers_[pending_index_]);
    const bool presented =
        err == ESP_OK && waitForFrameCompletions(eof_start, 3,
                                                 kRgbFramePeriodMs * 5U + 20U);
    if (err == ESP_OK) {
      active_index_ = pending_index_;
      canonical_fb0_valid_ = active_index_ == 0;
    }
    _framebuffer = framebuffers_[active_index_];
    atomic_pending_ = false;
    atomic_reason_ = "none";
    atomic_started_ms_ = 0;
    return err == ESP_OK && presented;
  }

  void service() {
    if (!atomic_pending_ || atomic_started_ms_ == 0 ||
        millis() - atomic_started_ms_ < 15000U) {
      return;
    }
    _framebuffer = framebuffers_[active_index_];
    atomic_pending_ = false;
    atomic_started_ms_ = 0;
    Serial.printf(
        "[Display/S3] Atomic redraw timeout, keeping framebuffer %u\n",
        static_cast<unsigned>(active_index_));
    atomic_reason_ = "none";
  }

  bool canonicalizeForStorage() {
    if (!panel_handle_) return false;
    storage_transition_ = true;
    if (atomic_pending_) {
      commitAtomicFrame();
    }
    if (active_index_ == 0) {
      _framebuffer = framebuffers_[0];
      canonical_fb0_valid_ = true;
      return true;
    }

    memcpy(framebuffers_[0], framebuffers_[active_index_], _framebuffer_size);
    Cache_WriteBack_Addr(reinterpret_cast<uint32_t>(framebuffers_[0]),
                         _framebuffer_size);
    canonical_fb0_valid_ = true;
    const uint32_t eof_start = frame_complete_count_;
    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        panel_handle_, 0, 0, _fb_width, _fb_height, framebuffers_[0]);
    if (err == ESP_OK) {
      waitForFrameCompletions(eof_start, 3, kRgbFramePeriodMs * 5U + 20U);
    }
    if (err == ESP_OK) {
      active_index_ = 0;
    }
    _framebuffer = framebuffers_[active_index_];
    return canonical_fb0_valid_;
  }

  esp_err_t restartAfterStorage(uint32_t& wait_ms) {
    wait_ms = 0;
    if (!panel_handle_ || !canonical_fb0_valid_) {
      storage_transition_ = false;
      return ESP_ERR_INVALID_STATE;
    }

    restart_vsync_seen_ = false;
    restart_one_shot_armed_ = true;
    const uint32_t started_ms = millis();
    esp_err_t err = esp_lcd_rgb_panel_restart(panel_handle_);
    if (err == ESP_OK) {
      enableVsyncInterruptOneShot();
      while (!restart_vsync_seen_ &&
             millis() - started_ms < kRgbFramePeriodMs * 3U + 20U) {
        delay(1);
      }
      if (!restart_vsync_seen_) {
        err = ESP_ERR_TIMEOUT;
      } else {
        const uint32_t eof_start = restart_eof_baseline_;
        if (!waitForFrameCompletions(eof_start, 2,
                                     kRgbFramePeriodMs * 4U + 20U)) {
          err = ESP_ERR_TIMEOUT;
        }
      }
    }
    maskVsyncInterrupt();
    restart_one_shot_armed_ = false;
    if (restart_vsync_seen_) {
      active_index_ = 0;
      _framebuffer = framebuffers_[0];
    }
    wait_ms = millis() - started_ms;
    storage_transition_ = false;
    return err;
  }

 private:
  static bool IRAM_ATTR onFrameComplete(
      esp_lcd_panel_handle_t panel,
      const esp_lcd_rgb_panel_event_data_t* event_data, void* user_ctx) {
    (void)panel;
    (void)event_data;
    auto* self = static_cast<WaveshareRgbDisplay*>(user_ctx);
    if (self) ++self->frame_complete_count_;
    return false;
  }

  static bool IRAM_ATTR onVsync(
      esp_lcd_panel_handle_t panel,
      const esp_lcd_rgb_panel_event_data_t* event_data, void* user_ctx) {
    (void)panel;
    (void)event_data;
    auto* self = static_cast<WaveshareRgbDisplay*>(user_ctx);
    if (!self || !self->restart_one_shot_armed_) return false;

    PERIPH_RCC_ATOMIC() {
      lcd_ll_enable_interrupt(&LCD_CAM, LCD_LL_EVENT_RGB, false);
    }
    self->restart_one_shot_armed_ = false;
    self->restart_eof_baseline_ = self->frame_complete_count_;
    self->restart_vsync_seen_ = true;
    return false;
  }

  static void maskVsyncInterrupt() {
    PERIPH_RCC_ATOMIC() {
      lcd_ll_enable_interrupt(&LCD_CAM, LCD_LL_EVENT_RGB, false);
      lcd_ll_clear_interrupt_status(&LCD_CAM, LCD_LL_EVENT_RGB);
    }
  }

  static void enableVsyncInterruptOneShot() {
    PERIPH_RCC_ATOMIC() {
      lcd_ll_clear_interrupt_status(&LCD_CAM, LCD_LL_EVENT_RGB);
      lcd_ll_enable_interrupt(&LCD_CAM, LCD_LL_EVENT_RGB, true);
    }
  }

  bool waitForFrameCompletions(uint32_t start, uint32_t count,
                               uint32_t timeout_ms) const {
    const uint32_t started_ms = millis();
    while (static_cast<uint32_t>(frame_complete_count_ - start) < count &&
           millis() - started_ms < timeout_ms) {
      delay(1);
    }
    return static_cast<uint32_t>(frame_complete_count_ - start) >= count;
  }

  esp_lcd_panel_handle_t panel_handle_ = nullptr;
  uint16_t* framebuffers_[2] = {nullptr, nullptr};
  uint8_t active_index_ = 0;
  uint8_t pending_index_ = 0;
  bool atomic_pending_ = false;
  bool storage_transition_ = false;
  bool canonical_fb0_valid_ = true;
  uint32_t atomic_started_ms_ = 0;
  const char* atomic_reason_ = "none";
  volatile uint32_t frame_complete_count_ = 0;
  volatile bool restart_one_shot_armed_ = false;
  volatile bool restart_vsync_seen_ = false;
  volatile uint32_t restart_eof_baseline_ = 0;
};

WaveshareRgbDisplay* g_gfx = nullptr;
SPIClass g_sd_spi(FSPI);

bool g_display_ready = false;
bool g_backlight_ready = false;
bool g_backlight_on = false;
bool g_touch_ready = false;
bool g_littlefs_ready = false;
bool g_sd_available = false;
bool g_sd_init_attempted = false;
uint32_t g_sd_retry_tick_ms = 0;
uint8_t g_brightness = 0;
uint8_t g_rotation = DeviceWaveshareS3TouchLCD7::kProfile.rotation_default;
uint8_t g_touch_address = 0;
uint16_t g_storage_write_depth = 0;
bool g_storage_blackout_active = false;
bool g_storage_restart_required = false;
uint8_t g_storage_restore_brightness = 0;
bool g_touch_active = false;
int16_t g_touch_last_x = 0;
int16_t g_touch_last_y = 0;
uint8_t g_touch_status_error_streak = 0;
uint8_t g_touch_point_error_streak = 0;

void ensureStorageLayout() {
  if (!g_littlefs_ready) return;
  LittleFS.mkdir("/_tile_grids");
  LittleFS.mkdir("/_tile_links");
  LittleFS.mkdir("/icons");
}

bool copyFile(fs::FS& src_fs, fs::FS& dst_fs, const char* path) {
  File src = src_fs.open(path, FILE_READ);
  if (!src) return false;
  File dst = dst_fs.open(path, FILE_WRITE);
  if (!dst) {
    src.close();
    return false;
  }
  uint8_t buffer[512];
  while (src.available()) {
    const size_t count = src.read(buffer, sizeof(buffer));
    if (!count || dst.write(buffer, count) != count) break;
  }
  dst.close();
  src.close();
  return true;
}

void copyDirectory(fs::FS& src_fs, fs::FS& dst_fs, const char* dir_path) {
  File dir = src_fs.open(dir_path);
  if (!dir || !dir.isDirectory()) return;
  dst_fs.mkdir(dir_path);

  File entry = dir.openNextFile();
  while (entry) {
    const String path = String(dir_path) + "/" + entry.name();
    const bool directory = entry.isDirectory();
    entry.close();
    if (directory) {
      copyDirectory(src_fs, dst_fs, path.c_str());
    } else if (copyFile(src_fs, dst_fs, path.c_str())) {
      Serial.printf("[Storage] Migrated: %s\n", path.c_str());
    }
    entry = dir.openNextFile();
  }
  dir.close();
}

bool writeTouchRegister(uint16_t reg, uint8_t value) {
  if (!g_touch_address) return false;
  Wire.beginTransmission(g_touch_address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readTouchRegisters(uint16_t reg, uint8_t* data, size_t len) {
  if (!g_touch_address || !data || len == 0 || len > 32) return false;
  Wire.beginTransmission(g_touch_address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  const size_t received =
      Wire.requestFrom(static_cast<int>(g_touch_address), static_cast<int>(len));
  if (received != len) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    data[i] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

bool probeTouchAddress(uint8_t address) {
  Wire.beginTransmission(address);
  if (Wire.endTransmission() != 0) return false;
  g_touch_address = address;
  uint8_t product_id[4] = {};
  if (!readTouchRegisters(kTouchProductIdRegister, product_id,
                          sizeof(product_id))) {
    g_touch_address = 0;
    return false;
  }
  Serial.printf(
      "[Device/Waveshare ESP32-S3-Touch-LCD-7] GT911 at 0x%02X, id=%c%c%c%c\n",
      address, product_id[0], product_id[1], product_id[2], product_id[3]);
  return true;
}

bool initTouch() {
  if (g_touch_ready) return true;
  if (!initExpander()) return false;
  Wire.begin(kTouchSda, kTouchScl, kTouchFrequency);

  pinMode(kTouchInterruptPin, OUTPUT);
  digitalWrite(kTouchInterruptPin, LOW);
  delay(10);
  expanderSetPin(kExioTouchReset, false);
  delay(100);
  expanderSetPin(kExioTouchReset, true);
  delay(200);
  pinMode(kTouchInterruptPin, INPUT);

  if (!probeTouchAddress(kTouchAddressPrimary) &&
      !probeTouchAddress(kTouchAddressAlternate)) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] GT911 not found at 0x5D/0x14");
    return false;
  }

  writeTouchRegister(kTouchStatusRegister, 0);
  g_touch_active = false;
  g_touch_ready = true;
  return true;
}

// The backlight is a CH422G on/off switch line: no PWM dimming is possible
// on this board (see AGENTS.md guidance against inventing a fake curve).
bool initBacklight() {
  if (g_backlight_ready) return true;
  if (!initExpander()) return false;
  expanderSetPin(kExioBacklight, false);
  g_backlight_ready = true;
  return true;
}

void applyBrightness(uint8_t value, bool remember = true) {
  if (remember) g_brightness = value;
  if (!g_backlight_ready && !initBacklight()) return;
  g_backlight_on = value != 0;
  expanderSetPin(kExioBacklight, g_backlight_on);
}

bool resetPanel() {
  if (!initExpander()) return false;
  expanderSetPin(kExioLcdReset, false);
  delay(10);
  expanderSetPin(kExioLcdReset, true);
  delay(100);
  return true;
}

bool initDisplay() {
  if (g_display_ready) return true;

  if (!resetPanel()) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] CH422G panel reset failed");
    return false;
  }

  g_gfx = new WaveshareRgbDisplay(g_rotation);
  if (!g_gfx || !g_gfx->begin()) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] ST7262 RGB display init failed");
    return false;
  }

  g_gfx->fillScreen(0x0000);
  g_display_ready = true;
  Serial.printf(
      "[Device/Waveshare ESP32-S3-Touch-LCD-7] Display ready, mode=%s, "
      "PCLK=%u MHz, bounce=%u rows/%u px, XIP=%u, cache-line=%u B, "
      "PSRAM free=%u KB\n",
      HOMETILES_WAVESHARE_S3_7_RGB_MODE_LABEL,
      static_cast<unsigned>(kRgbPclkHz / 1000000),
      static_cast<unsigned>(HOMETILES_WAVESHARE_S3_7_RGB_BOUNCE_ROWS),
      static_cast<unsigned>(kRgbBounceBufferPixels), kHasPsramXip ? 1U : 0U,
      kHasCacheLine64 ? 64U : 32U,
      static_cast<unsigned>(
          heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  return true;
}

void mapTouch(uint16_t raw_x, uint16_t raw_y, int16_t& x, int16_t& y) {
  constexpr int16_t kMaxX = kScreenWidth - 1;
  constexpr int16_t kMaxY = kScreenHeight - 1;
  // Non-square 800x480 panel: only the 0/180 degree flip is supported (see
  // waveshare_touch_lcd_4_3 for the same constraint on the same resolution).
  if ((g_rotation & 0x03) == 2) {
    x = kMaxX - static_cast<int16_t>(raw_x);
    y = kMaxY - static_cast<int16_t>(raw_y);
  } else {
    x = static_cast<int16_t>(raw_x);
    y = static_cast<int16_t>(raw_y);
  }
  x = std::max<int16_t>(0, std::min<int16_t>(kMaxX, x));
  y = std::max<int16_t>(0, std::min<int16_t>(kMaxY, y));
}

}  // namespace

bool DeviceWaveshareS3TouchLCD7::init() {
  Serial.println("[Device/Waveshare ESP32-S3-Touch-LCD-7] Initialising board...");

  if (!psramFound()) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] ERROR: octal PSRAM not detected");
    return false;
  }
  const uint32_t flash_bytes = ESP.getFlashChipSize();
  const uint32_t psram_bytes = ESP.getPsramSize();
  if (flash_bytes != kExpectedFlashBytes || psram_bytes != kExpectedPsramBytes) {
    Serial.printf(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] ERROR: expected N8R8, "
        "detected flash=%u MB, PSRAM=%u MB\n",
        static_cast<unsigned>(flash_bytes / (1024U * 1024U)),
        static_cast<unsigned>(psram_bytes / (1024U * 1024U)));
    return false;
  }
  Serial.printf(
      "[Device/Waveshare ESP32-S3-Touch-LCD-7] Flash=%u MB, PSRAM=%u MB\n",
      static_cast<unsigned>(flash_bytes / (1024U * 1024U)),
      static_cast<unsigned>(psram_bytes / (1024U * 1024U)));

  if (!initBacklight()) return false;
  applyBrightness(0, false);

  // Mount/format LittleFS and create its base directories before the RGB
  // peripheral starts scanning PSRAM. Backlight is already hard-off, so even
  // a long first-install format cannot expose an uninitialised panel.
  if (!initLittleFS()) return false;

  // The first WiFi start after an erased NVS performs a full RF calibration
  // and commits roughly 2 KB of PHY data. Do that one-time operation before
  // RGB/GDMA starts; WiFi.persistent(false) alone only protects WiFi config,
  // not the IDF PHY calibration namespace.
  auto* phy_calibration = static_cast<esp_phy_calibration_data_t*>(
      heap_caps_malloc(sizeof(esp_phy_calibration_data_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  esp_err_t phy_load_err = phy_calibration
                               ? esp_phy_load_cal_data_from_nvs(phy_calibration)
                               : ESP_ERR_NO_MEM;
  if (phy_load_err != ESP_OK) {
    WiFi.persistent(false);
    const bool wifi_started = WiFi.mode(WIFI_STA);
    const bool wifi_stopped = wifi_started && WiFi.mode(WIFI_OFF);
    if (wifi_started && wifi_stopped && phy_calibration) {
      phy_load_err = esp_phy_load_cal_data_from_nvs(phy_calibration);
    }
  }
  if (phy_calibration) heap_caps_free(phy_calibration);

  if (!initDisplay()) return false;

  if (!initTouch()) {
    Serial.println(
        "[Device/Waveshare ESP32-S3-Touch-LCD-7] Touch unavailable; continuing");
  }

  return true;
}

void DeviceWaveshareS3TouchLCD7::update() {
  if (g_gfx) g_gfx->service();
}

void DeviceWaveshareS3TouchLCD7::displayPushPixels(
    int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* data) {
  if (!g_display_ready || !g_gfx || !data || w <= 0 || h <= 0) return;
  g_gfx->draw16bitRGBBitmap(static_cast<int16_t>(x), static_cast<int16_t>(y),
                           const_cast<uint16_t*>(data),
                           static_cast<int16_t>(w), static_cast<int16_t>(h));
}

void DeviceWaveshareS3TouchLCD7::displayPushPixelsDMA(
    int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* data) {
  displayPushPixels(x, y, w, h, data);
}

bool DeviceWaveshareS3TouchLCD7::displayTryFullFramePreview(
    int32_t x, int32_t y, int32_t w, int32_t h, int32_t source_stride,
    const uint16_t* data, size_t data_size, bool byte_swap) {
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)source_stride;
  (void)data;
  (void)data_size;
  (void)byte_swap;
  return false;
}

bool DeviceWaveshareS3TouchLCD7::displayBeginAtomicFrame(const char* reason) {
  return g_display_ready && g_gfx && g_gfx->beginAtomicFrame(reason);
}

void DeviceWaveshareS3TouchLCD7::displayWaitDMA() {}

void DeviceWaveshareS3TouchLCD7::displayFillScreen(uint16_t color) {
  if (g_display_ready && g_gfx) g_gfx->fillScreen(color);
}

void DeviceWaveshareS3TouchLCD7::displaySetRotation(uint8_t rotation) {
  g_rotation = rotation & 0x03;
  if (g_display_ready && g_gfx) g_gfx->setRotation(g_rotation);
}

void DeviceWaveshareS3TouchLCD7::setBrightness(uint8_t value) {
  applyBrightness(value);
}

uint8_t DeviceWaveshareS3TouchLCD7::getBrightness() { return g_brightness; }

bool DeviceWaveshareS3TouchLCD7::getTouch(int16_t& x, int16_t& y) {
  if (!g_touch_ready && !initTouch()) return false;

  const auto return_held_point = [&]() {
    if (!g_touch_active) return false;
    x = g_touch_last_x;
    y = g_touch_last_y;
    return true;
  };
  const auto return_held_or_fail_safe = [&](uint8_t& error_streak) {
    if (error_streak < UINT8_MAX) ++error_streak;
    if (!g_touch_active || error_streak < kTouchErrorReleaseThreshold) {
      return return_held_point();
    }
    g_touch_active = false;
    g_touch_status_error_streak = 0;
    g_touch_point_error_streak = 0;
    return false;
  };

  uint8_t status = 0;
  if (!readTouchRegisters(kTouchStatusRegister, &status, 1)) {
    return return_held_or_fail_safe(g_touch_status_error_streak);
  }
  g_touch_status_error_streak = 0;
  if ((status & 0x80) == 0) {
    return return_held_point();
  }

  const uint8_t points = status & 0x0F;
  if (points == 0) {
    writeTouchRegister(kTouchStatusRegister, 0);
    g_touch_active = false;
    g_touch_point_error_streak = 0;
    return false;
  }
  if (points > 5) {
    writeTouchRegister(kTouchStatusRegister, 0);
    return return_held_or_fail_safe(g_touch_point_error_streak);
  }

  uint8_t point[8] = {};
  const bool read_ok =
      readTouchRegisters(kTouchPointRegister, point, sizeof(point));
  writeTouchRegister(kTouchStatusRegister, 0);
  if (!read_ok) {
    return return_held_or_fail_safe(g_touch_point_error_streak);
  }

  const uint16_t raw_x = static_cast<uint16_t>(point[1] | (point[2] << 8));
  const uint16_t raw_y = static_cast<uint16_t>(point[3] | (point[4] << 8));
  if (raw_x >= kScreenWidth || raw_y >= kScreenHeight) {
    return return_held_or_fail_safe(g_touch_point_error_streak);
  }

  g_touch_point_error_streak = 0;
  mapTouch(raw_x, raw_y, x, y);
  g_touch_last_x = x;
  g_touch_last_y = y;
  if (!g_touch_active) {
    g_touch_active = true;
  }
  return true;
}

void DeviceWaveshareS3TouchLCD7::displaySleep() { applyBrightness(0, false); }

void DeviceWaveshareS3TouchLCD7::displayWake() {
  applyBrightness(g_brightness ? g_brightness : 160, false);
}

void DeviceWaveshareS3TouchLCD7::displayWakeDark() {
  applyBrightness(0, false);
}

void DeviceWaveshareS3TouchLCD7::displayPowerSaveOn() { displaySleep(); }

void DeviceWaveshareS3TouchLCD7::displayPowerSaveOff() { displayWake(); }

void DeviceWaveshareS3TouchLCD7::displayWaitDisplay() {
  if (g_display_ready && g_gfx) g_gfx->commitAtomicFrame();
}

void DeviceWaveshareS3TouchLCD7::prepareForRestart() {
  applyBrightness(0, false);
  if (g_display_ready && g_gfx) {
    g_gfx->fillScreen(0x0000);
    g_gfx->flush(true);
  }
  if (g_sd_available) {
    SD.end();
    expanderSetPin(kExioSdCs, true);
    g_sd_available = false;
  }
  delay(20);
}

bool DeviceWaveshareS3TouchLCD7::initSDCard() {
  if (g_sd_available && SD.cardType() != CARD_NONE) return true;

  const uint32_t now = millis();
  if (g_sd_init_attempted && (now - g_sd_retry_tick_ms) < kSdRetryMs) {
    return false;
  }
  g_sd_init_attempted = true;
  g_sd_retry_tick_ms = now;
  if (!initExpander()) return false;
  SD.end();

  // CS lives on the CH422G, not a native GPIO. Assert it once and leave it
  // low for the whole session: nothing else shares this SPI bus, matching
  // the vendor's own SD example (SPI.setHwCs(false), SD.begin(-1, ...)).
  expanderSetPin(kExioSdCs, false);
  delay(10);
  g_sd_spi.begin(kSdSck, kSdMiso, kSdMosi, -1);
  bool mounted = SD.begin(-1, g_sd_spi, kSdFrequency, "/sdcard", 5);
  if (!mounted && kSdFrequency != kSdFallbackFrequency) {
    SD.end();
    g_sd_spi.end();
    g_sd_spi.begin(kSdSck, kSdMiso, kSdMosi, -1);
    mounted = SD.begin(-1, g_sd_spi, kSdFallbackFrequency, "/sdcard", 5);
  }
  if (!mounted) {
    expanderSetPin(kExioSdCs, true);
    g_sd_available = false;
    Serial.println("[Device/Waveshare ESP32-S3-Touch-LCD-7] SD card mount failed");
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    SD.end();
    expanderSetPin(kExioSdCs, true);
    g_sd_available = false;
    Serial.println("[Device/Waveshare ESP32-S3-Touch-LCD-7] SD card absent");
    return false;
  }

  g_sd_available = true;
  Serial.printf(
      "[Device/Waveshare ESP32-S3-Touch-LCD-7] SD card OK, size=%llu MB\n",
      static_cast<unsigned long long>(SD.cardSize() / (1024ULL * 1024ULL)));
  return true;
}

bool DeviceWaveshareS3TouchLCD7::storageReady() { return g_littlefs_ready; }

fs::FS& DeviceWaveshareS3TouchLCD7::storageFS() { return LittleFS; }

void DeviceWaveshareS3TouchLCD7::storageWriteBegin() {
  if (g_storage_write_depth < UINT16_MAX) {
    ++g_storage_write_depth;
  }
  if (g_storage_write_depth != 1) return;

  // Espressif's supported bounce mode is safe across main-flash writes only
  // with PSRAM XIP and a 64-byte S3 cache line. The stock Arduino SDK has
  // neither. Mark the continuous RGB stream for an explicit restart and hide
  // the short underflow while the flash cache is unavailable.
  if (kHasPsramXip && kHasCacheLine64) return;
  if (!g_display_ready) return;

  g_storage_restart_required = true;
  const bool blackout = g_backlight_ready && g_backlight_on;
  if (blackout) {
    g_storage_blackout_active = true;
    g_storage_restore_brightness = g_brightness;
    applyBrightness(0, false);
    delay(2);
  }

  if (g_gfx && !g_gfx->canonicalizeForStorage()) {
    Serial.println(
        "[Display/S3] Failed to canonicalize framebuffer before flash write");
  }
}

void DeviceWaveshareS3TouchLCD7::storageWriteEnd() {
  if (g_storage_write_depth == 0) return;
  --g_storage_write_depth;
  if (g_storage_write_depth != 0) return;

  const bool restart_required = g_storage_restart_required;
  const bool restore_backlight = g_storage_blackout_active;
  const uint8_t restore_brightness = g_storage_restore_brightness;
  g_storage_restart_required = false;
  g_storage_blackout_active = false;
  g_storage_restore_brightness = 0;

  if (restart_required) {
    uint32_t restart_wait_ms = 0;
    const esp_err_t restart_result =
        g_gfx ? g_gfx->restartAfterStorage(restart_wait_ms)
              : ESP_ERR_INVALID_STATE;
    if (restart_result != ESP_OK) {
      Serial.printf(
          "[Display/S3] RGB restart after flash write failed: %s (0x%X)\n",
          esp_err_to_name(restart_result),
          static_cast<unsigned>(restart_result));
    }
    (void)restart_wait_ms;
  }

  if (restore_backlight) applyBrightness(restore_brightness, false);
}

bool DeviceWaveshareS3TouchLCD7::sdReady() {
  return g_sd_available && SD.cardType() != CARD_NONE;
}

fs::FS& DeviceWaveshareS3TouchLCD7::sdFS() { return SD; }

bool DeviceWaveshareS3TouchLCD7::suspendSDCardForNetworkTransition() {
  // Native S3 WiFi does not share the P4 ESP-Hosted SDIO bus with the card.
  return false;
}

bool DeviceWaveshareS3TouchLCD7::resumeSDCardAfterNetworkTransition() {
  return initSDCard();
}

bool DeviceWaveshareS3TouchLCD7::initLittleFS() {
  if (g_littlefs_ready) return true;
  if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
    Serial.println("[Device/Waveshare ESP32-S3-Touch-LCD-7] LittleFS mount failed");
    return false;
  }
  g_littlefs_ready = true;
  ensureStorageLayout();
  Serial.printf(
      "[Device/Waveshare ESP32-S3-Touch-LCD-7] LittleFS ready, total=%u, used=%u\n",
      static_cast<unsigned>(LittleFS.totalBytes()),
      static_cast<unsigned>(LittleFS.usedBytes()));
  return true;
}

void DeviceWaveshareS3TouchLCD7::migrateStorageFromSD() {
  if (!initLittleFS() || LittleFS.exists("/_migrated")) return;

  const bool have_sd = initSDCard();
  storageWriteBegin();
  ensureStorageLayout();
  if (have_sd) {
    Serial.println("[Storage] Migrating data from SD to LittleFS...");
    copyDirectory(SD, LittleFS, "/_tile_grids");
    copyDirectory(SD, LittleFS, "/_tile_links");
    copyDirectory(SD, LittleFS, "/icons");
    Serial.println("[Storage] Migration complete");
  } else {
    Serial.println("[Storage] No SD card, starting fresh");
  }

  File flag = LittleFS.open("/_migrated", FILE_WRITE);
  if (flag) {
    flag.print("1");
    flag.close();
  }
  storageWriteEnd();
}

#endif  // defined(DEVICE_WAVESHARE_S3_TOUCH_LCD_7)
