#include "obsidian_panel.hpp"

#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_attr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace t3::companion::runtime {
namespace {

constexpr int kWidth = 466;
constexpr int kHeight = 466;
constexpr int kDmaRows = 12;
static_assert(LV_COLOR_DEPTH == 16 && LV_DRAW_BUF_STRIDE_ALIGN == 1,
              "Obsidian presenter requires packed RGB565 drawing buffers");
DMA_ATTR uint16_t dma_pixels[kWidth * kDmaRows];
esp_lcd_panel_handle_t panel = nullptr;
SemaphoreHandle_t transfer_done = nullptr;
std::atomic<bool> transfer_failed{false};

bool IRAM_ATTR on_transfer_done(esp_lcd_panel_io_handle_t,
                               esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t wake = pdFALSE;
  xSemaphoreGiveFromISR(transfer_done, &wake);
  return wake == pdTRUE;
}

void round_area(lv_event_t* event) {
  auto* area = static_cast<lv_area_t*>(lv_event_get_param(event));
  area->x1 &= ~1;
  area->y1 &= ~1;
  area->x2 |= 1;
  area->y2 |= 1;
}

void present(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
  if (transfer_failed.load()) {
    lv_display_flush_ready(display);
    return;
  }
  const int width = lv_area_get_width(area);
  const int height = lv_area_get_height(area);
  // RGB565, even coordinates and stride alignment 1 produce packed rows.
  // Keep each panel window even-sized as required by the CO5300 driver.
  const int rows_per_transfer = (kWidth * kDmaRows / width) & ~1;
  for (int row = 0; row < height;) {
    const int rows = std::min(rows_per_transfer, height - row);
    const size_t count = static_cast<size_t>(width) * rows;
    std::memcpy(dma_pixels, pixels + static_cast<size_t>(row) * width * 2, count * 2);
    lv_draw_sw_rgb565_swap(dma_pixels, count);
    const auto result = esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1 + row,
                                                 area->x2 + 1, area->y1 + row + rows, dma_pixels);
    // Do not overwrite the persistent DMA source until its completion ISR.
    // A bus failure must stop the presenter instead of reusing an in-flight buffer.
    if (result != ESP_OK || xSemaphoreTake(transfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
      transfer_failed.store(true);
      ESP_LOGE("obsidian_panel", "Display transfer stopped: %s",
               esp_err_to_name(result == ESP_OK ? ESP_ERR_TIMEOUT : result));
      // Retain the DMA staging buffer forever after a failure, including a
      // possibly late completion. The separate PSRAM buffer is safe to release.
      lv_display_flush_ready(display);
      return;
    }
    row += rows;
  }
  // LVGL owns one PSRAM buffer: release it only after every stripe was sent.
  lv_display_flush_ready(display);
}

}  // namespace

bool obsidian_panel_healthy() { return !transfer_failed.load(); }

bool initialize_obsidian_panel(bsp_display_cfg_t& config, lv_display_t*& display,
                              lv_indev_t*& input) {
  if (esp_lv_adapter_init(&config.lv_adapter_cfg) != ESP_OK) return false;
  transfer_done = xSemaphoreCreateBinary();
  if (!transfer_done) return false;
  esp_lcd_panel_io_handle_t io = nullptr;
  const bsp_display_config_t panel_config{.max_transfer_sz = sizeof(dma_pixels)};
  if (bsp_display_new(&panel_config, &panel, &io) != ESP_OK) return false;
  if (esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false) != ESP_OK) return false;
  esp_lv_adapter_display_config_t adapter_config{};
  adapter_config.panel = panel;
  adapter_config.panel_io = io;
  adapter_config.profile.interface = ESP_LV_ADAPTER_PANEL_IF_OTHER;
  adapter_config.profile.rotation = config.rotation;
  adapter_config.profile.hor_res = kWidth;
  adapter_config.profile.ver_res = kHeight;
  adapter_config.profile.buffer_height = kHeight;
  adapter_config.profile.use_psram = true;
  adapter_config.profile.require_double_buffer = false;
  adapter_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
  display = esp_lv_adapter_register_display(&adapter_config);
  if (!display) return false;
  lv_display_set_flush_cb(display, present);
  lv_display_add_event_cb(display, round_area, LV_EVENT_INVALIDATE_AREA, nullptr);
  esp_lcd_panel_io_callbacks_t callbacks{};
  callbacks.on_color_trans_done = on_transfer_done;
  if (esp_lcd_panel_io_register_event_callbacks(io, &callbacks, nullptr) != ESP_OK) return false;
  esp_lcd_touch_handle_t touch = nullptr;
  if (bsp_touch_new(&config, &touch) != ESP_OK) return false;
  const esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, touch);
  input = esp_lv_adapter_register_touch(&touch_config);
  if (!input || bsp_display_brightness_init() != ESP_OK) return false;
  return esp_lv_adapter_start() == ESP_OK;
}

}  // namespace t3::companion::runtime
