#include "obsidian_display.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace {
std::array<uint16_t, 466 * 466> pixels{};
// Match the board's full-size PSRAM composition buffer. The board presents
// completed rectangles through a separate bounded internal-DMA buffer.
std::array<uint16_t, 466 * 466> buffer{};
size_t flushed_pixels = 0;

void flush(lv_display_t* display, const lv_area_t* area, uint8_t* data) {
  flushed_pixels += lv_area_get_size(area);
  const auto* source = reinterpret_cast<const uint16_t*>(data);
  for (int y = area->y1; y <= area->y2; ++y) {
    for (int x = area->x1; x <= area->x2; ++x) pixels[y * 466 + x] = *source++;
  }
  lv_display_flush_ready(display);
}
}  // namespace

int main(int argc, char** argv) {
  lv_init();
  auto* display = lv_display_create(466, 466);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display, buffer.data(), nullptr, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display, flush);
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x03070E), 0);
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
  pocket_code::ObsidianDisplay face;
  face.create(lv_screen_active());
  t3::ui::UiModel model;
  model.live_handshake = true;
  model.tokens.today_value = "75.5";
  model.tokens.today_unit = "M";
  model.tokens.month_value = "2.87B";
  model.rings.codex.weekly = {true, false, 31};
  model.rings.claude.weekly = {true, false, 55};
  model.rings.xai.weekly = {true, false, 6};
  const std::string mode = argc > 2 ? argv[2] : "sample";
  if (mode == "unavailable") {
    model.tokens = {};
    model.rings = {};
    model.live_handshake = false;
  } else if (mode == "limits") {
    model.tokens.today_value = "999.9";
    model.rings.codex.weekly.percent = 0;
    model.rings.claude.weekly.percent = 100;
    model.rings.xai.weekly.percent = 99.9;
  }
  if (mode == "animation-damage") {
    auto empty = model;
    empty.rings.codex.weekly.percent = 0;
    empty.rings.claude.weekly.percent = 0;
    empty.rings.xai.weekly.percent = 0;
    face.render(empty, false);
    lv_refr_now(display);
    flushed_pixels = 0;
    face.render(model, true);
    lv_tick_inc(33);
    lv_timer_handler();
    lv_tick_inc(33);
    lv_timer_handler();
    // The first two small steps should update ring tips, not repaint most of
    // the 466x466 face. Hiding zero-length arc objects violated this bound.
    if (flushed_pixels == 0 || flushed_pixels >= 466U * 466U / 2U) {
      std::cerr << "Animation startup repainted " << flushed_pixels << " pixels\n";
      return 4;
    }
    std::cout << "Animation startup repainted only " << flushed_pixels << " pixels\n";
  } else {
    face.render(model, mode == "animation");
  }
  // Exercise the actual LVGL animation path as well as the settled renderer.
  if (mode == "animation") {
    for (int elapsed = 0; elapsed < 1000; elapsed += 33) {
      lv_tick_inc(33);
      lv_timer_handler();
    }
  }
  lv_refr_now(display);
  if (mode == "limits") {
    // Regression for LVGL's textured 360-degree arc mask: a full Claude ring
    // must paint all four quadrants, while the zero-percent outer track stays dim.
    for (const auto& point : {std::array{233, 46}, std::array{420, 233},
                             std::array{233, 420}, std::array{46, 233}}) {
      if (((pixels[point[1] * 466 + point[0]] >> 11) & 31) < 12) {
        std::cerr << "Full weekly ring missing at " << point[0] << ',' << point[1] << '\n';
        return 2;
      }
    }
    if (((pixels[24 * 466 + 233] >> 11) & 31) >= 12) {
      std::cerr << "Zero-percent ring painted an active arc\n";
      return 3;
    }
  }
  std::ofstream out(argc > 1 ? argv[1] : "obsidian.ppm", std::ios::binary);
  out << "P6\n466 466\n255\n";
  for (auto pixel : pixels) {
    const char rgb[] = {static_cast<char>(((pixel >> 11) & 31) * 255 / 31),
                        static_cast<char>(((pixel >> 5) & 63) * 255 / 63),
                        static_cast<char>((pixel & 31) * 255 / 31)};
    out.write(rgb, 3);
  }
  if (!out) return 1;
  std::cout << "Rendered native Obsidian face (" << mode << ") at 466x466\n";
}
