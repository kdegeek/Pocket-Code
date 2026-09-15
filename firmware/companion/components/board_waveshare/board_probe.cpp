#include "board_probe.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"
#include "esp_chip_info.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace t3::board {
namespace {

constexpr char kTag[] = "task2_probe";
constexpr std::uint16_t kWidth = 466;
constexpr std::uint16_t kHeight = 466;
constexpr std::size_t kAudioFrames = 256;
// The reviewed Waveshare schematic routes the upper-right PWR key through
// the AXP2101 PMU.  Its edge/short/long status bits are in INTSTS2 (0x49),
// with the corresponding enable mask in INTEN2 (0x41).  The temporary probe
// enables only those four PKEY status bits, clears only INTSTS2, and restores
// the original INTEN2 mask after the press/release capture; it never changes
// a PMU rail or power/shutdown configuration.
constexpr std::uint8_t kAxp2101Address = 0x34;
constexpr std::uint8_t kAxp2101IntEnable2 = 0x41;
constexpr std::uint8_t kAxp2101IntStatus2 = 0x49;
constexpr std::uint8_t kAxpPkeyPositiveBit = 0x01;
constexpr std::uint8_t kAxpPkeyNegativeBit = 0x02;
constexpr std::uint8_t kAxpPkeyLongBit = 0x04;
constexpr std::uint8_t kAxpPkeyShortBit = 0x08;
constexpr std::uint8_t kAxpPkeyMask = kAxpPkeyPositiveBit | kAxpPkeyNegativeBit |
                                      kAxpPkeyLongBit | kAxpPkeyShortBit;
constexpr i2s_tdm_slot_mask_t kAudioSlotMask =
    static_cast<i2s_tdm_slot_mask_t>(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                     I2S_TDM_SLOT2 | I2S_TDM_SLOT3);

// CST9217 INT is a falling-edge notification (the maintained driver notes that
// the line is not held low continuously).  Keep the event in an ISR-safe latch
// so the probe's serialized 50 ms loop cannot miss a short pulse.
volatile bool g_touch_irq_pending = false;
portMUX_TYPE g_touch_irq_mux = portMUX_INITIALIZER_UNLOCKED;

void touch_irq_callback(esp_lcd_touch_handle_t) {
  portENTER_CRITICAL_ISR(&g_touch_irq_mux);
  g_touch_irq_pending = true;
  portEXIT_CRITICAL_ISR(&g_touch_irq_mux);
}

bool take_touch_irq() {
  portENTER_CRITICAL(&g_touch_irq_mux);
  const bool pending = g_touch_irq_pending;
  g_touch_irq_pending = false;
  portEXIT_CRITICAL(&g_touch_irq_mux);
  return pending;
}

const char *esp_err_name(esp_err_t value) {
  return esp_err_to_name(value);
}

void print_chip_evidence() {
  esp_chip_info_t chip_info{};
  esp_chip_info(&chip_info);

  std::uint8_t mac[6]{};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  std::uint32_t logical_flash = 0;
  std::uint32_t physical_flash = 0;
  const esp_err_t logical_rc = esp_flash_get_size(nullptr, &logical_flash);
  const esp_err_t physical_rc = esp_flash_get_physical_size(nullptr, &physical_flash);
  const auto *flash = esp_flash_default_chip;
  const auto psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

  ESP_LOGI(kTag,
           "TASK2_CHIP sku=ESP32-S3-Touch-AMOLED-1.75C model=%d revision=%u.%02u "
           "cores=%u features=0x%08x",
           static_cast<int>(chip_info.model), chip_info.revision / 100,
           chip_info.revision % 100, chip_info.cores,
           static_cast<unsigned>(chip_info.features));
  ESP_LOGI(kTag,
           "TASK2_MAC %02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  ESP_LOGI(kTag,
           "TASK2_FLASH logical_rc=%s logical_bytes=%u physical_rc=%s physical_bytes=%u "
           "chip_id=0x%06lx read_mode=%d",
           esp_err_name(logical_rc), static_cast<unsigned>(logical_flash),
           esp_err_name(physical_rc), static_cast<unsigned>(physical_flash),
           static_cast<unsigned long>(flash ? flash->chip_id : 0),
           flash ? static_cast<int>(flash->read_mode) : -1);
  ESP_LOGI(kTag,
           "TASK2_PSRAM runtime_bytes=%u physical_expected_bytes=8388608 "
           "runtime_mode=%s (esptool flash_id is authoritative for SKU probe)",
           static_cast<unsigned>(psram), psram ? "enabled" : "disabled");
}

void draw_probe_grid(esp_lcd_panel_handle_t panel) {
  // Keep the frame buffer in a small internal DMA row.  Task 1's empty-image
  // sdkconfig deliberately leaves PSRAM runtime allocation disabled; the
  // physical 8 MiB PSRAM is still recorded from the chip probe.
  constexpr std::size_t pixel_count = kWidth;
  auto *pixels = static_cast<std::uint16_t *>(
      heap_caps_calloc(pixel_count, sizeof(std::uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (pixels == nullptr) {
    ESP_LOGE(kTag, "TASK2_DISPLAY allocation failed for one %ux1 row", kWidth);
    return;
  }

  // RGB565 bars and a dark grid make orientation and the six-pixel controller
  // column gap visible without pulling LVGL into the probe image.
  for (std::uint16_t y = 0; y < kHeight; ++y) {
    for (std::uint16_t x = 0; x < kWidth; ++x) {
      std::uint16_t color = 0x0000;
      if (x < kWidth / 4) {
        color = 0xF800;  // red
      } else if (x < kWidth / 2) {
        color = 0x07E0;  // green
      } else if (x < (kWidth * 3) / 4) {
        color = 0x001F;  // blue
      } else {
        color = 0xFFFF;  // white
      }
      if ((x % 58U) < 2U || (y % 58U) < 2U || x == 0 || y == 0 ||
          x == kWidth - 1 || y == kHeight - 1) {
        color = 0x7BEF;
      }
      pixels[x] = color;
    }
    const esp_err_t draw_rc = esp_lcd_panel_draw_bitmap(panel, 0, y, kWidth, y + 1, pixels);
    if (draw_rc != ESP_OK) {
      ESP_LOGE(kTag, "TASK2_DISPLAY row=%u draw rc=%s", y, esp_err_name(draw_rc));
      break;
    }
  }
  ESP_LOGI(kTag, "TASK2_DISPLAY grid draw h=%u v=%u gap_x=6 row_dma=true", kWidth, kHeight);
  heap_caps_free(pixels);
}

void run_display_sequence(esp_lcd_panel_handle_t panel) {
  ESP_LOGI(kTag, "TASK2_DISPLAY_BEGIN controller=CO5300 color=RGB565 gap_x=6");
  draw_probe_grid(panel);
  constexpr std::array<bsp_display_rotation_t, 4> rotations = {
      BSP_DISPLAY_ROTATE_0, BSP_DISPLAY_ROTATE_90, BSP_DISPLAY_ROTATE_180,
      BSP_DISPLAY_ROTATE_270};
  for (const auto rotation : rotations) {
    const auto rc = bsp_display_rotation_set(rotation);
    ESP_LOGI(kTag, "TASK2_DISPLAY_ROTATION rotation=%d rc=%s",
             static_cast<int>(rotation), esp_err_name(rc));
    vTaskDelay(pdMS_TO_TICKS(750));
  }
  const auto restore_rc = bsp_display_rotation_set(BSP_DISPLAY_ROTATE_0);
  ESP_LOGI(kTag, "TASK2_DISPLAY_ROTATION restored=0 rc=%s", esp_err_name(restore_rc));

  // Brightness and display-off are panel commands only.  Do not toggle the
  // No legacy TCA9554 power output or any hard power/reset rail here.
  for (const int brightness : {100, 25, 70}) {
    const auto rc = bsp_display_brightness_set(brightness);
    ESP_LOGI(kTag, "TASK2_DISPLAY_BRIGHTNESS percent=%d rc=%s", brightness,
             esp_err_name(rc));
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  ESP_LOGI(kTag, "TASK2_DISPLAY_OFF_BEGIN panel_only=true hard_power_off=false");
  auto rc = esp_lcd_panel_disp_on_off(panel, false);
  ESP_LOGI(kTag, "TASK2_DISPLAY_OFF state=off rc=%s", esp_err_name(rc));
  vTaskDelay(pdMS_TO_TICKS(900));
  rc = esp_lcd_panel_disp_on_off(panel, true);
  ESP_LOGI(kTag, "TASK2_DISPLAY_OFF state=on rc=%s", esp_err_name(rc));
  vTaskDelay(pdMS_TO_TICKS(400));
  rc = bsp_display_brightness_set(70);
  ESP_LOGI(kTag, "TASK2_DISPLAY_BRIGHTNESS restored=70 rc=%s", esp_err_name(rc));
  ESP_LOGI(kTag, "TASK2_DISPLAY_END");
}

esp_err_t init_touch(esp_lcd_touch_handle_t *touch) {
  bsp_display_cfg_t cfg{};
  cfg.touch_flags.swap_xy = 0;
  cfg.touch_flags.mirror_x = 1;
  cfg.touch_flags.mirror_y = 1;
  const auto touch_rc = bsp_touch_new(&cfg, touch);
  if (touch_rc != ESP_OK || touch == nullptr || *touch == nullptr) {
    return touch_rc;
  }
  g_touch_irq_pending = false;
  return esp_lcd_touch_register_interrupt_callback(*touch, touch_irq_callback);
}

struct AudioCapture {
  // This mirrors the maintained bsp_extra voice path: one shared I2S data
  // interface, ES8311 playback opened first, then an explicitly diagnostic
  // ES7210 TDM recording path (never the 1.75C product mapping).
  esp_codec_dev_handle_t speaker = nullptr;
  esp_codec_dev_handle_t codec = nullptr;
  i2s_chan_handle_t tx = nullptr;
  i2s_chan_handle_t rx = nullptr;
};

struct AxpPwrCapture {
  i2c_master_dev_handle_t dev = nullptr;
  std::uint8_t previous_status = 0xFF;
  std::uint8_t original_enable_mask = 0;
  std::uint8_t active_enable_mask = 0;
  std::uint8_t seen_status = 0;
  bool config_changed = false;
  bool capture_ready = false;
  bool capture_complete = false;
  bool read_error_reported = false;
};

esp_err_t axp2101_read_register(const AxpPwrCapture &pwr, std::uint8_t reg,
                                std::uint8_t *value) {
  if (pwr.dev == nullptr || value == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return i2c_master_transmit_receive(pwr.dev, &reg, 1, value, 1, 20);
}

esp_err_t axp2101_write_register(const AxpPwrCapture &pwr, std::uint8_t reg,
                                 std::uint8_t value) {
  if (pwr.dev == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  const std::uint8_t payload[2] = {reg, value};
  return i2c_master_transmit(pwr.dev, payload, sizeof(payload), 20);
}

AxpPwrCapture init_axp_pwr(i2c_master_bus_handle_t i2c_bus) {
  AxpPwrCapture result{};
  i2c_device_config_t pmu_cfg{};
  pmu_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  pmu_cfg.device_address = kAxp2101Address;
  pmu_cfg.scl_speed_hz = 400000;
  const auto add_rc = i2c_master_bus_add_device(i2c_bus, &pmu_cfg, &result.dev);
  if (add_rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_BUTTON PWR_PATH_BLOCKED AXP2101 add rc=%s",
             esp_err_name(add_rc));
    result.dev = nullptr;
    return result;
  }

  std::uint8_t initial_status = 0;
  const auto status_rc = axp2101_read_register(result, kAxp2101IntStatus2, &initial_status);
  if (status_rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_BUTTON PWR_PATH initial_status rc=%s", esp_err_name(status_rc));
  } else {
    ESP_LOGI(kTag,
             "TASK2_BUTTON PWR_STATUS2 raw=0x%02X positive=%u negative=%u long=%u short=%u "
             "initial=true",
             initial_status, (initial_status & kAxpPkeyPositiveBit) != 0,
             (initial_status & kAxpPkeyNegativeBit) != 0,
             (initial_status & kAxpPkeyLongBit) != 0,
             (initial_status & kAxpPkeyShortBit) != 0);
  }

  std::uint8_t original_enable_mask = 0;
  const auto enable_read_rc =
      axp2101_read_register(result, kAxp2101IntEnable2, &original_enable_mask);
  if (enable_read_rc != ESP_OK) {
    ESP_LOGE(kTag,
             "TASK2_BUTTON PWR_PATH source=AXP2101.PKEY address=0x34 status_reg=0x49 "
             "enable_reg=0x41 enable_read_rc=%s",
             esp_err_name(enable_read_rc));
    return result;
  }
  result.original_enable_mask = original_enable_mask;
  result.active_enable_mask = static_cast<std::uint8_t>(original_enable_mask | kAxpPkeyMask);
  ESP_LOGI(kTag,
           "TASK2_BUTTON PWR_CONFIG before_inten2=0x%02X requested_inten2=0x%02X "
           "operation=read_modify_write_pkey_only",
           original_enable_mask, result.active_enable_mask);
  const auto enable_write_rc =
      axp2101_write_register(result, kAxp2101IntEnable2, result.active_enable_mask);
  if (enable_write_rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_BUTTON PWR_CONFIG enable_write rc=%s", esp_err_name(enable_write_rc));
    return result;
  }
  std::uint8_t enabled_after = 0;
  const auto enabled_after_rc =
      axp2101_read_register(result, kAxp2101IntEnable2, &enabled_after);
  ESP_LOGI(kTag, "TASK2_BUTTON PWR_CONFIG after_inten2=0x%02X read_rc=%s", enabled_after,
           esp_err_name(enabled_after_rc));
  if (enabled_after_rc != ESP_OK || enabled_after != result.active_enable_mask) {
    ESP_LOGE(kTag, "TASK2_BUTTON PWR_CONFIG enable_readback_mismatch expected=0x%02X",
             result.active_enable_mask);
    return result;
  }
  result.config_changed = true;
  return result;
}

bool prepare_axp_pwr_capture(AxpPwrCapture *pwr) {
  if (pwr == nullptr || pwr->dev == nullptr || !pwr->config_changed) {
    return false;
  }

  // INTSTS2 is write-one-to-clear in the reviewed AXP2101 sequence.  Clear
  // only this PKEY status byte immediately before capture; do not touch the
  // other status registers or PMU configuration.
  const auto clear_rc = axp2101_write_register(*pwr, kAxp2101IntStatus2, 0xFF);
  ESP_LOGI(kTag, "TASK2_BUTTON PWR_CONFIG clear_intsts2 value=0xFF rc=%s",
           esp_err_name(clear_rc));
  if (clear_rc != ESP_OK) {
    return false;
  }
  std::uint8_t cleared_status = 0;
  const auto cleared_rc = axp2101_read_register(*pwr, kAxp2101IntStatus2, &cleared_status);
  ESP_LOGI(kTag, "TASK2_BUTTON PWR_CONFIG cleared_intsts2=0x%02X read_rc=%s", cleared_status,
           esp_err_name(cleared_rc));
  if (cleared_rc != ESP_OK || cleared_status != 0) {
    ESP_LOGE(kTag, "TASK2_BUTTON PWR_CONFIG clear_readback_mismatch");
    return false;
  }

  pwr->previous_status = 0;
  pwr->seen_status = 0;
  pwr->capture_ready = true;
  ESP_LOGI(kTag,
           "TASK2_BUTTON PWR_CAPTURE_READY source=AXP2101.PKEY status2=0x49 "
           "inten2_transient=true restore_after_edges=true");
  return true;
}

void restore_axp_inten2(AxpPwrCapture *pwr) {
  if (pwr == nullptr || pwr->dev == nullptr || !pwr->config_changed || pwr->capture_complete) {
    return;
  }
  const auto restore_rc = axp2101_write_register(*pwr, kAxp2101IntEnable2,
                                                 pwr->original_enable_mask);
  std::uint8_t restored = 0;
  const auto readback_rc = axp2101_read_register(*pwr, kAxp2101IntEnable2, &restored);
  ESP_LOGI(kTag,
           "TASK2_BUTTON PWR_CONFIG restore_inten2=0x%02X expected=0x%02X write_rc=%s "
           "read_rc=%s transient_done=true",
           restored, pwr->original_enable_mask, esp_err_name(restore_rc),
           esp_err_name(readback_rc));
  pwr->capture_complete = true;
  pwr->capture_ready = false;
  pwr->config_changed = false;
}

void log_audio_metrics(const AudioCapture &audio);

void poll_touch_and_buttons(esp_lcd_touch_handle_t touch, AxpPwrCapture *pwr,
                            const AudioCapture *audio) {
  ESP_LOGI(kTag,
           "TASK2_INPUT_READY touch=CST9217 irq_gpio=11 irq_active_low=true "
           "pwr=AXP2101.PKEY address=0x34 status_reg=0x49 boot=GPIO0 active_low=true");
  int previous_boot = -1;
  std::int64_t next_periodic = 0;
  std::int64_t next_audio = 0;
  for (;;) {
    const auto now = esp_timer_get_time();

    std::uint8_t pwr_status = 0;
    const auto pwr_rc = axp2101_read_register(
        pwr == nullptr ? AxpPwrCapture{} : *pwr, kAxp2101IntStatus2, &pwr_status);
    const int boot_level = gpio_get_level(GPIO_NUM_0);
    const int irq_level = gpio_get_level(GPIO_NUM_11);
    const bool touch_irq_pending = take_touch_irq();
    if (pwr_rc != ESP_OK) {
      if (pwr != nullptr && !pwr->read_error_reported) {
        ESP_LOGE(kTag, "TASK2_BUTTON PWR_STATUS2 read rc=%s", esp_err_name(pwr_rc));
        pwr->read_error_reported = true;
      }
    } else if (pwr != nullptr && pwr_status != pwr->previous_status) {
      ESP_LOGI(kTag,
               "TASK2_BUTTON PWR_STATUS2 raw=0x%02X positive=%u negative=%u long=%u short=%u "
               "changed=true",
               pwr_status, (pwr_status & kAxpPkeyPositiveBit) != 0,
               (pwr_status & kAxpPkeyNegativeBit) != 0,
               (pwr_status & kAxpPkeyLongBit) != 0,
               (pwr_status & kAxpPkeyShortBit) != 0);
      pwr->previous_status = pwr_status;
      pwr->seen_status = static_cast<std::uint8_t>(pwr->seen_status | (pwr_status & kAxpPkeyMask));
      if ((pwr->seen_status & (kAxpPkeyPositiveBit | kAxpPkeyNegativeBit)) ==
          (kAxpPkeyPositiveBit | kAxpPkeyNegativeBit)) {
        restore_axp_inten2(pwr);
      }
    }
    if (boot_level != previous_boot) {
      ESP_LOGI(kTag, "TASK2_BUTTON BOOT_RAW level=%d active=%d", boot_level,
               boot_level == 0 ? 1 : 0);
      previous_boot = boot_level;
    }

    // The maintained CST9217 driver returns ESP_ERR_INVALID_RESPONSE (ACK
    // byte != 0xAB) for its idle frame.  Its reviewed interrupt polarity is
    // active-low, so only transact when the controller asserts IRQ; this
    // keeps the passive probe quiet while preserving real touch captures.
    if (touch != nullptr && (irq_level == 0 || touch_irq_pending)) {
      const auto touch_rc = esp_lcd_touch_read_data(touch);
      if (touch_rc != ESP_OK) {
        ESP_LOGW(kTag, "TASK2_TOUCH read rc=%s irq=%d edge=%d", esp_err_name(touch_rc),
                 irq_level, touch_irq_pending ? 1 : 0);
      } else {
        std::array<esp_lcd_touch_point_data_t, 2> points{};
        std::uint8_t point_count = 0;
        const auto data_rc = esp_lcd_touch_get_data(touch, points.data(), &point_count,
                                                    static_cast<std::uint8_t>(points.size()));
        if (data_rc == ESP_OK && point_count > 0) {
          ESP_LOGI(kTag, "TASK2_TOUCH irq=%d edge=%d points=%u x=%u y=%u strength=%u",
                   irq_level, touch_irq_pending ? 1 : 0, point_count, points[0].x, points[0].y,
                   points[0].strength);
        }
      }
    }

    if (now >= next_periodic) {
      ESP_LOGI(kTag, "TASK2_INPUT_HEARTBEAT irq=%d pwr_status=%s boot=%d", irq_level,
               pwr_rc == ESP_OK ? "readable" : "unavailable", boot_level);
      next_periodic = now + 2'000'000;
    }
    if (audio != nullptr && now >= next_audio) {
      log_audio_metrics(*audio);
      next_audio = now + 2'000'000;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

AudioCapture init_audio() {
  AudioCapture result{};
  ESP_LOGI(kTag,
           "TASK2_AUDIO_VENDOR_INIT path=bsp_extra_codec_init_equivalent "
           "voice_fs=24k/16-bit/4-slot "
           "i2s_port=%d pins=mclk16,bclk9,ws45,tx8,rx10", CONFIG_BSP_I2S_NUM);

  i2s_chan_config_t channel_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(static_cast<i2s_port_t>(CONFIG_BSP_I2S_NUM), I2S_ROLE_MASTER);
  // Match the maintained BSP voice path: keep TX clocks/data flowing with
  // zero-filled DMA when no playback payload is queued.
  channel_cfg.auto_clear = true;
  auto rc = i2s_new_channel(&channel_cfg, &result.tx, &result.rx);
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_AUDIO i2s_new_channel rc=%s", esp_err_name(rc));
    return result;
  }
  i2s_std_config_t tx_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = GPIO_NUM_16,
          .bclk = BSP_I2S_SCLK,
          .ws = BSP_I2S_LCLK,
          .dout = BSP_I2S_DOUT,
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  i2s_tdm_config_t tdm_cfg = {
      .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(24000),
      .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, kAudioSlotMask),
      .gpio_cfg = {
          .mclk = GPIO_NUM_16,
          .bclk = BSP_I2S_SCLK,
          .ws = BSP_I2S_LCLK,
          .dout = I2S_GPIO_UNUSED,
          .din = BSP_I2S_DSIN,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  tdm_cfg.slot_cfg.total_slot = 4;
  tx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  tdm_cfg.clk_cfg.bclk_div = 8;
  rc = i2s_channel_init_std_mode(result.tx, &tx_cfg);
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_AUDIO i2s_tx_std_init rc=%s", esp_err_name(rc));
    return result;
  }
  rc = i2s_channel_init_tdm_mode(result.rx, &tdm_cfg);
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_AUDIO i2s_tdm_init rc=%s", esp_err_name(rc));
    return result;
  }
  rc = i2s_channel_enable(result.tx);
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_AUDIO i2s_tx_enable rc=%s", esp_err_name(rc));
    return result;
  }
  rc = i2s_channel_enable(result.rx);
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_AUDIO i2s_rx_enable rc=%s", esp_err_name(rc));
    return result;
  }

  const auto i2c_bus = bsp_i2c_get_handle();
  audio_codec_i2s_cfg_t data_cfg = {
      .port = CONFIG_BSP_I2S_NUM,
      .rx_handle = result.rx,
      .tx_handle = result.tx,
      .clk_src = 0,
  };
  const auto *data_if = audio_codec_new_i2s_data(&data_cfg);
  if (data_if == nullptr) {
    ESP_LOGE(kTag, "TASK2_AUDIO_VENDOR_BLOCKED codec interfaces unavailable");
    return result;
  }

  // Keep the maintained bsp_extra ordering: create both codec devices on the
  // shared data interface, open ES8311 playback first, then ES7210 capture.
  // ES8311_CODEC_DEFAULT_ADDR and ES7210_CODEC_DEFAULT_ADDR are the vendor
  // driver's 8-bit addresses (0x30/0x80), corresponding to I2C ACKs 0x18/0x40.
  audio_codec_i2c_cfg_t out_ctrl_cfg = {
      .port = BSP_I2C_NUM,
      .addr = ES8311_CODEC_DEFAULT_ADDR,
      .bus_handle = i2c_bus,
  };
  const auto *out_ctrl_if = audio_codec_new_i2c_ctrl(&out_ctrl_cfg);
  const auto *gpio_if = audio_codec_new_gpio();
  esp_codec_dev_hw_gain_t hw_gain = {
      .pa_voltage = 5.0,
      .codec_dac_voltage = 3.3,
      .pa_gain = 0.0,
  };
  es8311_codec_cfg_t es8311_cfg = {
      .ctrl_if = out_ctrl_if,
      .gpio_if = gpio_if,
      .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
      .pa_pin = BSP_POWER_AMP_IO,
      .pa_reverted = false,
      .master_mode = false,
      .use_mclk = true,
      .digital_mic = false,
      .invert_mclk = false,
      .invert_sclk = false,
      .hw_gain = hw_gain,
      .no_dac_ref = false,
      .mclk_div = 0,
  };
  const auto *out_codec_if =
      (out_ctrl_if != nullptr && gpio_if != nullptr) ? es8311_codec_new(&es8311_cfg) : nullptr;
  esp_codec_dev_cfg_t out_dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_OUT,
      .codec_if = out_codec_if,
      .data_if = data_if,
  };
  result.speaker = out_codec_if == nullptr ? nullptr : esp_codec_dev_new(&out_dev_cfg);

  audio_codec_i2c_cfg_t in_ctrl_cfg = {
      .port = BSP_I2C_NUM,
      .addr = ES7210_CODEC_DEFAULT_ADDR,
      .bus_handle = i2c_bus,
  };
  const auto *in_ctrl_if = audio_codec_new_i2c_ctrl(&in_ctrl_cfg);
  es7210_codec_cfg_t es7210_cfg = {
      .ctrl_if = in_ctrl_if,
      .master_mode = false,
      // The enhanced voice path uses the populated three-input profile
      // (front MIC1/MIC2 plus MIC3/reference).  The wire format remains the
      // maintained four-slot TDM diagnostic stream so the reference and
      // unused positions remain visible; no product channel is inferred.
      .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3,
      .mclk_src = ES7210_MCLK_FROM_PAD,
      .mclk_div = I2S_MCLK_MULTIPLE_256,
  };
  const auto *in_codec_if = in_ctrl_if == nullptr ? nullptr : es7210_codec_new(&es7210_cfg);
  esp_codec_dev_cfg_t in_dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_IN,
      .codec_if = in_codec_if,
      .data_if = data_if,
  };
  result.codec = in_codec_if == nullptr ? nullptr : esp_codec_dev_new(&in_dev_cfg);
  if (result.speaker == nullptr || result.codec == nullptr) {
    ESP_LOGE(kTag,
             "TASK2_AUDIO_VENDOR_BLOCKED codec devices unavailable speaker=%d record=%d",
             result.speaker != nullptr, result.codec != nullptr);
    return result;
  }

  esp_codec_dev_sample_info_t output_fs = {
      .bits_per_sample = 16,
      .channel = I2S_SLOT_MODE_STEREO,
      .channel_mask = 0,
      .sample_rate = 24000,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  const auto output_open_rc = esp_codec_dev_open(result.speaker, &output_fs);
  if (output_open_rc != ESP_CODEC_DEV_OK) {
    ESP_LOGE(kTag, "TASK2_AUDIO_VENDOR_BLOCKED playback_open_rc=%d", output_open_rc);
    return result;
  }
  const auto output_mute_rc = esp_codec_dev_set_out_mute(result.speaker, false);
  const auto output_volume_rc = esp_codec_dev_set_out_vol(result.speaker, 0);
  ESP_LOGI(kTag,
           "TASK2_AUDIO_VENDOR_FS playback_open_rc=%d mute_rc=%d volume_rc=%d "
           "sample_rate=24000 bits=16 channels=2",
           output_open_rc, output_mute_rc, output_volume_rc);

  esp_codec_dev_sample_info_t input_fs = {
      .bits_per_sample = 16,
      .channel = 4,
      .channel_mask = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
      .sample_rate = 24000,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  const auto input_open_rc = esp_codec_dev_open(result.codec, &input_fs);
  if (input_open_rc != ESP_CODEC_DEV_OK) {
    ESP_LOGE(kTag, "TASK2_AUDIO_VENDOR_BLOCKED record_open_rc=%d", input_open_rc);
    result.codec = nullptr;
    return result;
  }
  constexpr std::uint16_t kVoiceMicGainMask = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
  const auto gain_rc = esp_codec_dev_set_in_channel_gain(result.codec, kVoiceMicGainMask, 24.0F);
  ESP_LOGI(kTag,
           "TASK2_AUDIO_VENDOR_FS record_open_rc=%d gain_rc=%d gain_mask=0x%02X "
           "sample_rate=24000 bits=16 channels=4 slot_mask=0x0F",
           input_open_rc, gain_rc, kVoiceMicGainMask);

  // The register reads below are intentionally read-only.  They provide
  // direct ES7210 state evidence after the vendor init/open/gain sequence,
  // without dumping or changing any persistent device state.
  constexpr std::array<int, 9> kStatusRegisters = {
      0x01, 0x06, 0x11, 0x12, 0x14, 0x15, 0x40, 0x43, 0x44};
  for (const int reg : kStatusRegisters) {
    int value = 0;
    const int read_rc = esp_codec_dev_read_reg(result.codec, reg, &value);
    ESP_LOGI(kTag, "TASK2_AUDIO_ES7210_STATUS reg=0x%02X read_rc=%d value=0x%02X", reg,
             read_rc, value & 0xFF);
  }
  i2s_chan_info_t rx_info{};
  const auto rx_info_rc = i2s_channel_get_info(result.rx, &rx_info);
  ESP_LOGI(kTag,
           "TASK2_AUDIO_I2S_STATUS rc=%s port=%d dir=%d mode=%d enabled=%d "
           "pair=%p mclk_hz=%u bclk_hz=%u",
           esp_err_name(rx_info_rc), static_cast<int>(rx_info.id), static_cast<int>(rx_info.dir),
           static_cast<int>(rx_info.mode), rx_info.is_enabled ? 1 : 0, rx_info.pair_chan,
           static_cast<unsigned>(rx_info.mclk_hz), static_cast<unsigned>(rx_info.bclk_hz));
  return result;
}

// One four-slot wire frame is retained only for the diagnostic experiment.
// The product path is ordinary STD stereo; no TDM slot is treated as a
// microphone mapping here.
constexpr std::size_t kToneCaptureFrames = 128;
constexpr std::size_t kToneCaptureSlots = 4;
constexpr std::size_t kToneCaptureSamples = kToneCaptureFrames * kToneCaptureSlots;
constexpr std::size_t kTonePlaybackFrames = 512;
constexpr std::size_t kTonePlaybackChannels = 2;
constexpr std::size_t kTonePlaybackSamples = kTonePlaybackFrames * kTonePlaybackChannels;
constexpr std::uint32_t kToneSampleRate = 24000;
constexpr std::uint32_t kToneFrequencyHz = 1000;
constexpr std::int16_t kToneAmplitude = 1800;
constexpr std::uint32_t kToneDurationMs = 2000;
constexpr std::int16_t kToneSentinel = static_cast<std::int16_t>(0x5A5A);

struct ToneSlotAggregate {
  std::size_t overwritten_words = 0;
  std::size_t nonzero_words = 0;
  std::int32_t min_sample = 0;
  std::int32_t max_sample = 0;
  std::int32_t peak = 0;
  double sum_squares = 0.0;
};

struct ToneAggregate {
  std::uint32_t reads = 0;
  std::uint32_t successful_reads = 0;
  std::uint32_t failed_reads = 0;
  int last_read_rc = ESP_CODEC_DEV_OK;
  std::size_t overwritten_words = 0;
  std::array<ToneSlotAggregate, kToneCaptureSlots> slots{};
};

void capture_four_slot_frame(esp_codec_dev_handle_t record, ToneAggregate &aggregate) {
  static std::array<std::int16_t, kToneCaptureSamples> capture{};
  capture.fill(kToneSentinel);
  const int read_rc = esp_codec_dev_read(record, capture.data(), sizeof(capture));
  ++aggregate.reads;
  aggregate.last_read_rc = read_rc;
  if (read_rc == ESP_CODEC_DEV_OK) {
    ++aggregate.successful_reads;
  } else {
    ++aggregate.failed_reads;
  }

  for (std::size_t index = 0; index < capture.size(); ++index) {
    const auto value = capture[index];
    if (value == kToneSentinel) {
      continue;
    }
    ++aggregate.overwritten_words;
    auto &slot = aggregate.slots[index % kToneCaptureSlots];
    ++slot.overwritten_words;
    const auto sample = static_cast<std::int32_t>(value);
    if (sample != 0) {
      ++slot.nonzero_words;
    }
    if (slot.overwritten_words == 1) {
      slot.min_sample = sample;
      slot.max_sample = sample;
    } else {
      slot.min_sample = std::min(slot.min_sample, sample);
      slot.max_sample = std::max(slot.max_sample, sample);
    }
    const auto magnitude = sample < 0 ? -sample : sample;
    slot.peak = std::max(slot.peak, magnitude);
    slot.sum_squares += static_cast<double>(sample) * sample;
  }
}

void log_four_slot_aggregate(const char *phase, const ToneAggregate &aggregate) {
  ESP_LOGI(kTag,
           "TASK2_TONE_CAPTURE phase=%s reads=%u successful=%u failed=%u last_read_rc=%d "
           "requested_bytes=%u overwritten_words=%u requested_words=%u sentinel=0x%04X",
           phase, static_cast<unsigned>(aggregate.reads),
           static_cast<unsigned>(aggregate.successful_reads),
           static_cast<unsigned>(aggregate.failed_reads),
           aggregate.last_read_rc,
           static_cast<unsigned>(sizeof(std::array<std::int16_t, kToneCaptureSamples>)),
           static_cast<unsigned>(aggregate.overwritten_words),
           static_cast<unsigned>(aggregate.reads * kToneCaptureSamples),
           static_cast<unsigned>(static_cast<std::uint16_t>(kToneSentinel)));
  for (std::size_t slot_index = 0; slot_index < aggregate.slots.size(); ++slot_index) {
    const auto &slot = aggregate.slots[slot_index];
    const int rms10 = slot.overwritten_words == 0
                          ? 0
                          : static_cast<int>(std::sqrt(slot.sum_squares / slot.overwritten_words) * 10.0);
    ESP_LOGI(kTag,
             "TASK2_TONE_SLOT phase=%s slot=%u nonzero=%u overwritten=%u min=%d max=%d "
             "rms10=%d peak=%d",
             phase, static_cast<unsigned>(slot_index),
             static_cast<unsigned>(slot.nonzero_words),
             static_cast<unsigned>(slot.overwritten_words), static_cast<int>(slot.min_sample),
             static_cast<int>(slot.max_sample), rms10, static_cast<int>(slot.peak));
  }
}

struct Gpio10Aggregate {
  std::uint32_t high = 0;
  std::uint32_t low = 0;
  int first = -1;
  int last = -1;
};

void sample_gpio10(const char *phase, Gpio10Aggregate &aggregate, std::size_t samples,
                   bool delay_between_samples) {
  gpio_io_config_t io_config{};
  const auto direction_rc = gpio_get_io_config(BSP_I2S_DSIN, &io_config);
  for (std::size_t index = 0; index < samples; ++index) {
    const int level = gpio_get_level(BSP_I2S_DSIN);
    if (index == 0) {
      aggregate.first = level;
    }
    aggregate.last = level;
    if (level == 0) {
      ++aggregate.low;
    } else {
      ++aggregate.high;
    }
    if (delay_between_samples) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  ESP_LOGI(kTag,
           "TASK2_TONE_GPIO10 phase=%s direction_rc=%s input_enabled=%d output_enabled=%d "
           "periph_oe=%d high=%u low=%u first=%d last=%d samples=%u",
           phase, esp_err_name(direction_rc), io_config.ie ? 1 : 0, io_config.oe ? 1 : 0,
           io_config.oe_ctrl_by_periph ? 1 : 0, static_cast<unsigned>(aggregate.high),
           static_cast<unsigned>(aggregate.low), aggregate.first, aggregate.last,
           static_cast<unsigned>(aggregate.high + aggregate.low));
}

void log_four_slot_es7210_registers(esp_codec_dev_handle_t record) {
  constexpr std::array<std::uint8_t, 28> kRegisters = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
      0x11, 0x12, 0x14, 0x15,
      0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
      0x4B, 0x4C,
  };
  for (const auto reg : kRegisters) {
    int value = 0;
    const int read_rc = esp_codec_dev_read_reg(record, reg, &value);
    ESP_LOGI(kTag, "TASK2_TONE_ES7210_REG reg=0x%02X read_rc=%d value=0x%02X", reg,
             read_rc, value & 0xFF);
  }
}

std::array<std::int16_t, kTonePlaybackSamples> &tone_buffer();

void generate_tone_buffer() {
  auto &tone = tone_buffer();
  constexpr double kTwoPi = 6.28318530717958647692;
  for (std::size_t frame = 0; frame < kTonePlaybackFrames; ++frame) {
    const double phase = kTwoPi * static_cast<double>(kToneFrequencyHz * frame) /
                         static_cast<double>(kToneSampleRate);
    const auto sample = static_cast<std::int16_t>(std::lround(std::sin(phase) * kToneAmplitude));
    tone[frame * kTonePlaybackChannels] = sample;
    tone[frame * kTonePlaybackChannels + 1] = sample;
  }
}

std::array<std::int16_t, kTonePlaybackSamples> &tone_buffer() {
  static std::array<std::int16_t, kTonePlaybackSamples> tone{};
  return tone;
}

void log_four_slot_i2s_effective(const AudioCapture &audio) {
  i2s_chan_info_t rx_info{};
  const auto rx_info_rc = i2s_channel_get_info(audio.rx, &rx_info);
  ESP_LOGI(kTag,
           "TASK2_TONE_I2S effective_rc=%s port=%d dir=%d mode=%d enabled=%d "
           "mclk_hz=%u bclk_hz=%u sample_rate=%u slot_bits=16 total_slots=4 slot_mask=0x0F "
           "expected_mclk_hz=%u expected_bclk_hz=%u",
           esp_err_name(rx_info_rc), static_cast<int>(rx_info.id), static_cast<int>(rx_info.dir),
           static_cast<int>(rx_info.mode), rx_info.is_enabled ? 1 : 0,
           static_cast<unsigned>(rx_info.mclk_hz), static_cast<unsigned>(rx_info.bclk_hz),
           static_cast<unsigned>(kToneSampleRate),
           static_cast<unsigned>(kToneSampleRate * 256),
           static_cast<unsigned>(kToneSampleRate * 16 * kToneCaptureSlots));
}

void log_audio_metrics(const AudioCapture &audio) {
  if (audio.codec == nullptr || audio.rx == nullptr) {
    ESP_LOGE(kTag, "TASK2_AUDIO_BLOCKED codec unavailable; no slot claim recorded");
    return;
  }
  // Keep capture buffers out of the default ESP-IDF main-task stack.  The
  // probe runs its polling loop on `main`, whose stack is intentionally small;
  // a four-slot frame window plus floating-point accumulators otherwise
  // trips the FreeRTOS stack-overflow hook immediately after the first read.
  static std::array<std::int16_t, kAudioFrames * 4> samples{};
  static std::array<std::int16_t, kAudioFrames * 2> tx_silence{};
  std::size_t tx_bytes_written = 0;
  auto tx_rc = ESP_ERR_INVALID_STATE;
  if (audio.tx != nullptr) {
    tx_rc = i2s_channel_write(audio.tx, tx_silence.data(), sizeof(tx_silence),
                              &tx_bytes_written, pdMS_TO_TICKS(100));
  }
  if (tx_rc != ESP_OK) {
    ESP_LOGW(kTag, "TASK2_AUDIO_TX_CLOCK write_rc=%s bytes=%u", esp_err_name(tx_rc),
             static_cast<unsigned>(tx_bytes_written));
  }
  samples.fill(0);
  std::size_t bytes_read = 0;
  const auto read_rc = i2s_channel_read(audio.rx, samples.data(), sizeof(samples), &bytes_read,
                                        pdMS_TO_TICKS(100));
  if (read_rc != ESP_OK || bytes_read == 0) {
    ESP_LOGW(kTag, "TASK2_AUDIO_READ read_rc=%s bytes=%u", esp_err_name(read_rc),
             static_cast<unsigned>(bytes_read));
    return;
  }
  const std::size_t sample_count =
      std::min(bytes_read / sizeof(samples[0]), samples.size());
  const std::size_t frame_count = sample_count / 4;
  if (frame_count == 0) {
    ESP_LOGW(kTag, "TASK2_AUDIO_READ read_rc=%s bytes=%u frames=0", esp_err_name(read_rc),
             static_cast<unsigned>(bytes_read));
    return;
  }
  static std::array<double, 4> sum_squares{};
  static std::array<std::int32_t, 4> peaks{};
  sum_squares.fill(0.0);
  peaks.fill(0);
  std::size_t nonzero_samples = 0;
  std::int32_t min_sample = 0;
  std::int32_t max_sample = 0;
  bool have_sample = false;
  for (std::size_t frame = 0; frame < frame_count * 4; frame += 4) {
    for (std::size_t slot = 0; slot < 4; ++slot) {
      const auto value = static_cast<std::int32_t>(samples[frame + slot]);
      if (value != 0) {
        ++nonzero_samples;
      }
      if (!have_sample) {
        min_sample = value;
        max_sample = value;
        have_sample = true;
      } else {
        min_sample = std::min(min_sample, value);
        max_sample = std::max(max_sample, value);
      }
      const auto magnitude = value < 0 ? -value : value;
      peaks[slot] = std::max(peaks[slot], magnitude);
      sum_squares[slot] += static_cast<double>(value) * value;
    }
  }
  static std::array<int, 4> rms10{};
  for (std::size_t slot = 0; slot < rms10.size(); ++slot) {
    rms10[slot] = static_cast<int>(std::sqrt(sum_squares[slot] / frame_count) * 10.0);
  }
  ESP_LOGI(kTag,
           "TASK2_AUDIO_METRICS tx_rc=%s tx_bytes=%u read_rc=%s bytes=%u frames=%u nonzero=%u "
           "min=%d max=%d",
           esp_err_name(tx_rc), static_cast<unsigned>(tx_bytes_written),
           esp_err_name(read_rc), static_cast<unsigned>(bytes_read),
           static_cast<unsigned>(frame_count), static_cast<unsigned>(nonzero_samples),
           static_cast<int>(min_sample), static_cast<int>(max_sample));
  for (int slot = 0; slot < 4; ++slot) {
    ESP_LOGI(kTag, "TASK2_AUDIO_SLOT slot=%d rms10=%d peak=%d", slot, rms10[slot],
             static_cast<int>(peaks[slot]));
  }
}

}  // namespace

void run_probe() {
  ESP_LOGI(kTag, "TASK2_PROBE_START build=temporary-safe-no-nvs-read-no-erase");
  print_chip_evidence();

  const bsp_display_config_t display_cfg = {
      .max_transfer_sz = static_cast<int>(kWidth) * kHeight * BSP_LCD_BITS_PER_PIXEL / 8,
  };
  esp_lcd_panel_handle_t panel = nullptr;
  esp_lcd_panel_io_handle_t io = nullptr;
  auto rc = bsp_display_new(&display_cfg, &panel, &io);
  if (rc != ESP_OK || panel == nullptr || io == nullptr) {
    ESP_LOGE(kTag, "TASK2_DISPLAY_BLOCKED bsp_display_new rc=%s", esp_err_name(rc));
    return;
  }
  run_display_sequence(panel);

  // Scan only the I2C address ACK surface before creating any device handles.
  // This is read-only and avoids the BSP's fatal ESP_ERROR_CHECK wrapper when
  // the 1.75C board has no legacy TCA9554 at 0x20.
  const auto i2c_bus = bsp_i2c_get_handle();
  ESP_LOGI(kTag, "TASK2_I2C_SCAN_BEGIN sda=15 scl=14");
  constexpr std::array<std::uint8_t, 6> reviewed_i2c_addresses = {
      0x18, 0x20, 0x34, 0x40, 0x5A, 0x6B};
  for (const auto address : reviewed_i2c_addresses) {
    const auto probe_rc = i2c_master_probe(i2c_bus, address, 20);
    if (probe_rc == ESP_OK) {
      ESP_LOGI(kTag, "TASK2_I2C_ACK address=0x%02X", address);
    }
  }
  ESP_LOGI(kTag, "TASK2_I2C_SCAN_END");

  // The live board ACKs AXP2101 at 0x34 but not the schematic's optional
  // Legacy TCA9554 SYS_OUT path at 0x20.  Add only an AXP2101 device handle and read
  // its documented PWRKEY status register; do not call the BSP's fatal
  // expander initializer or write any PMU register.
  AxpPwrCapture pwr = init_axp_pwr(i2c_bus);

  esp_lcd_touch_handle_t touch = nullptr;
  rc = init_touch(&touch);
  if (rc != ESP_OK || touch == nullptr) {
    ESP_LOGE(kTag, "TASK2_TOUCH_BLOCKED bsp_touch_new rc=%s", esp_err_name(rc));
    restore_axp_inten2(&pwr);
    return;
  }

  const gpio_config_t boot_cfg = {
      .pin_bit_mask = 1ULL << GPIO_NUM_0,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  rc = gpio_config(&boot_cfg);
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_BUTTON_BLOCKED BOOT GPIO config rc=%s", esp_err_name(rc));
    restore_axp_inten2(&pwr);
    return;
  }

  const AudioCapture audio = init_audio();
  if (audio.codec == nullptr) {
    ESP_LOGE(kTag, "TASK2_PROBE_BLOCKED microphone initialization failed; no slot inference");
    restore_axp_inten2(&pwr);
    return;
  }

  // Clear INTSTS2 only after all other probe setup is complete, immediately
  // before entering the passive/user capture loop.
  if (!prepare_axp_pwr_capture(&pwr)) {
    ESP_LOGE(kTag, "TASK2_BUTTON PWR_CAPTURE_BLOCKED prepare_failed");
    restore_axp_inten2(&pwr);
    return;
  }

  ESP_LOGI(kTag,
           "TASK2_READY Press upper-right then bottom-right one at a time; tap display corners; "
           "speak near each front microphone. Do not hold BOOT during reset.");
  poll_touch_and_buttons(touch, &pwr, &audio);
}

void run_tone_diagnostic() {
  ESP_LOGI(kTag,
           "TASK2_TONE_START runtime=audio_only diagnostic_only=true topology=STD_TX+TDM_RX "
           "no_display_touch_buttons_network_nvs_ota");
  ESP_LOGI(kTag,
           "TASK2_TONE_SPEC frequency_hz=%u amplitude_peak=%d duration_ms=%u "
           "sample_rate_hz=%u bits=16 playback_channels=2 volume_db=0 output=ES8311 "
           "capture=ES7210_MIC1_MIC2_MIC3_reference diagnostic_tdm_slots=4 slot_mask=0x0F",
           static_cast<unsigned>(kToneFrequencyHz), static_cast<int>(kToneAmplitude),
           static_cast<unsigned>(kToneDurationMs), static_cast<unsigned>(kToneSampleRate));

  const auto i2c_rc = bsp_i2c_init();
  ESP_LOGI(kTag, "TASK2_TONE_I2C init_rc=%s", esp_err_name(i2c_rc));
  if (i2c_rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_TONE_BLOCKED i2c_init");
    return;
  }

  const AudioCapture audio = init_audio();
  if (audio.speaker == nullptr || audio.codec == nullptr || audio.rx == nullptr) {
    ESP_LOGE(kTag,
             "TASK2_TONE_BLOCKED codec_or_i2s_init speaker=%d record=%d rx=%d",
             audio.speaker != nullptr, audio.codec != nullptr, audio.rx != nullptr);
    return;
  }
  log_four_slot_i2s_effective(audio);
  log_four_slot_es7210_registers(audio.codec);

  ToneAggregate before{};
  Gpio10Aggregate gpio_before{};
  sample_gpio10("before", gpio_before, 128, true);
  for (int index = 0; index < 3; ++index) {
    capture_four_slot_frame(audio.codec, before);
  }
  log_four_slot_aggregate("before", before);

  generate_tone_buffer();
  ToneAggregate during{};
  Gpio10Aggregate gpio_during{};
  const auto tone_start_us = esp_timer_get_time();
  std::uint32_t writes = 0;
  int first_write_rc = ESP_CODEC_DEV_OK;
  int first_read_rc = ESP_CODEC_DEV_OK;
  bool tone_error = false;
  while (esp_timer_get_time() - tone_start_us <
         static_cast<int64_t>(kToneDurationMs) * 1000) {
    const int level = gpio_get_level(BSP_I2S_DSIN);
    if (gpio_during.high + gpio_during.low == 0) {
      gpio_during.first = level;
    }
    gpio_during.last = level;
    if (level == 0) {
      ++gpio_during.low;
    } else {
      ++gpio_during.high;
    }

    const int write_rc = esp_codec_dev_write(audio.speaker, tone_buffer().data(),
                                             sizeof(tone_buffer()));
    if (writes == 0) {
      first_write_rc = write_rc;
    }
    if (write_rc != ESP_CODEC_DEV_OK) {
      ESP_LOGE(kTag, "TASK2_TONE_ABORT write_rc=%d after_writes=%u", write_rc,
               static_cast<unsigned>(writes));
      tone_error = true;
      break;
    }
    ++writes;
    capture_four_slot_frame(audio.codec, during);
    if (during.reads == 1) {
      first_read_rc = static_cast<int>(during.last_read_rc);
    }
    if (during.failed_reads != 0) {
      ESP_LOGE(kTag, "TASK2_TONE_ABORT read_failures=%u last_read_rc=%d",
               static_cast<unsigned>(during.failed_reads),
               during.last_read_rc);
      tone_error = true;
      break;
    }
  }
  const auto tone_end_us = esp_timer_get_time();
  const auto elapsed_us = tone_end_us - tone_start_us;
  const int mute_rc = esp_codec_dev_set_out_mute(audio.speaker, true);
  ESP_LOGI(kTag,
           "TASK2_TONE_RESULT writes=%u write_bytes_each=%u first_write_rc=%d "
           "first_read_rc=%d elapsed_us=%lld elapsed_ms=%lld mute_rc=%d error=%d",
           static_cast<unsigned>(writes), static_cast<unsigned>(sizeof(tone_buffer())),
           first_write_rc, first_read_rc, static_cast<long long>(elapsed_us),
           static_cast<long long>(elapsed_us / 1000), mute_rc, tone_error ? 1 : 0);
  log_four_slot_aggregate("during", during);
  ESP_LOGI(kTag,
           "TASK2_TONE_GPIO10 phase=during direction_sample=bounded high=%u low=%u "
           "first=%d last=%d samples=%u",
           static_cast<unsigned>(gpio_during.high), static_cast<unsigned>(gpio_during.low),
           gpio_during.first, gpio_during.last,
           static_cast<unsigned>(gpio_during.high + gpio_during.low));

  ToneAggregate after{};
  Gpio10Aggregate gpio_after{};
  sample_gpio10("after", gpio_after, 128, true);
  for (int index = 0; index < 3; ++index) {
    capture_four_slot_frame(audio.codec, after);
  }
  log_four_slot_aggregate("after", after);
  ESP_LOGI(kTag,
           "TASK2_TONE_CAPTURE_SUMMARY diagnostic_only=true "
           "tdm_slot_mapping_claimed=false std_stereo_product_path=true");
  if (tone_error) {
    ESP_LOGE(kTag, "TASK2_TONE_BLOCKED capture_or_playback_error");
    return;
  }
  ESP_LOGI(kTag, "TASK2_TONE_DONE stable_runtime=1");
}

void run_tdm_diagnostic() {
  ESP_LOGI(kTag,
           "TASK2_TDM_START runtime=audio_only variant=1.75C topology=STD_TX+TDM_RX "
           "mclk_gpio=16 no_display_touch_buttons_network_nvs_ota");
  ESP_LOGI(kTag,
           "TASK2_TDM_SPEC sample_rate_hz=24000 bits=16 slots=4 slot_mask=0x0F "
           "mic_selected=MIC1|MIC2|MIC3 diagnostic_tdm_only=true "
           "std_product_channels=0,1 read_api=esp_codec_dev_read");

  const auto i2c_rc = bsp_i2c_init();
  ESP_LOGI(kTag, "TASK2_TDM_I2C init_rc=%s", esp_err_name(i2c_rc));
  if (i2c_rc != ESP_OK) {
    ESP_LOGE(kTag, "TASK2_TDM_BLOCKED i2c_init");
    return;
  }
  const AudioCapture audio = init_audio();
  if (audio.speaker == nullptr || audio.codec == nullptr || audio.rx == nullptr) {
    ESP_LOGE(kTag,
             "TASK2_TDM_BLOCKED codec_or_i2s_init speaker=%d record=%d rx=%d",
             audio.speaker != nullptr, audio.codec != nullptr, audio.rx != nullptr);
    return;
  }
  const int mute_rc = esp_codec_dev_set_out_mute(audio.speaker, true);
  ESP_LOGI(kTag, "TASK2_TDM_PLAYBACK muted_rc=%d no_tone=true", mute_rc);
  log_four_slot_i2s_effective(audio);
  log_four_slot_es7210_registers(audio.codec);

  Gpio10Aggregate gpio_idle{};
  sample_gpio10("tdm_idle", gpio_idle, 128, true);
  ToneAggregate idle{};
  for (int index = 0; index < 3; ++index) {
    capture_four_slot_frame(audio.codec, idle);
    char phase[24]{};
    std::snprintf(phase, sizeof(phase), "tdm_idle_%d", index);
    log_four_slot_aggregate(phase, idle);
    idle = ToneAggregate{};
  }
  ESP_LOGI(kTag,
           "TASK2_TDM_IDLE_READY stable_samples=3 awaiting_parent_speech_instruction=true "
           "speech_required_for_slot_mapping=true");

  for (std::uint32_t sequence = 0;; ++sequence) {
    ToneAggregate sample{};
    capture_four_slot_frame(audio.codec, sample);
    char phase[24]{};
    std::snprintf(phase, sizeof(phase), "tdm_seq_%u", static_cast<unsigned>(sequence));
    log_four_slot_aggregate(phase, sample);
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

}  // namespace t3::board
