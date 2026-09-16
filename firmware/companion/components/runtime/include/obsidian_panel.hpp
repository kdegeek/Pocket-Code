#pragma once

#include "bsp/esp32_s3_touch_amoled_1_75c.h"

namespace t3::companion::runtime {

// Starts the board's public panel/touch drivers with a full-size PSRAM drawing
// buffer and a bounded internal-DMA presenter. Call once during startup.
bool initialize_obsidian_panel(bsp_display_cfg_t& config, lv_display_t*& display,
                              lv_indev_t*& input);
bool obsidian_panel_healthy();

}  // namespace t3::companion::runtime
