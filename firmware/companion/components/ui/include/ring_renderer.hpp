#pragma once

#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace t3::ui {

using t3::companion::Provider;
using t3::companion::RateWindow;
using t3::companion::UsageProvider;
using t3::companion::UsageSnapshot;
using t3::companion::UsageStatus;

inline constexpr std::uint16_t kDisplayWidth = 466;
inline constexpr std::uint16_t kDisplayHeight = 466;
inline constexpr std::uint16_t kDisplayCenter = 233;
inline constexpr std::uint16_t kOuterRingRadius = 205;
inline constexpr std::uint16_t kMiddleRingRadius = 187;
inline constexpr std::uint16_t kInnerRingRadius = 169;
inline constexpr std::uint8_t kRingStroke = 9;
inline constexpr std::uint8_t kColumnGap = 6;
inline constexpr std::uint8_t kDisplayRotation = 0;

struct FlushRect {
  std::uint16_t x_start = 0;
  std::uint16_t y_start = 0;
  std::uint16_t x_end = kDisplayWidth - 1;
  std::uint16_t y_end = kDisplayHeight - 1;
};

struct Color {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
  std::uint8_t alpha = 255;

  friend bool operator==(const Color&, const Color&) = default;
};

class Framebuffer {
 public:
  Framebuffer();
  Framebuffer(std::uint16_t width, std::uint16_t height);

  [[nodiscard]] std::uint16_t width() const { return width_; }
  [[nodiscard]] std::uint16_t height() const { return height_; }
  [[nodiscard]] const std::vector<Color>& pixels() const { return pixels_; }
  [[nodiscard]] std::vector<Color>& pixels() { return pixels_; }

  void clear(Color color);
  void set(int x, int y, Color color);
  [[nodiscard]] Color at(int x, int y) const;
  [[nodiscard]] bool write_ppm(const std::string& path) const;

 private:
  std::uint16_t width_ = kDisplayWidth;
  std::uint16_t height_ = kDisplayHeight;
  std::vector<Color> pixels_;
};

struct RingGeometry {
  Provider provider = Provider::Codex;
  std::uint16_t radius = 0;
  std::uint8_t stroke = kRingStroke;
  Color color;
  Color track;
};

struct UsageValue {
  bool available = false;
  bool stale = false;
  double percent = 0.0;

  [[nodiscard]] std::string display_text() const;
};

struct ProviderRingValues {
  UsageValue weekly;
  UsageValue five_hour;
};

struct RingValues {
  ProviderRingValues codex;
  ProviderRingValues claude;
  ProviderRingValues xai;
};

[[nodiscard]] std::array<RingGeometry, 3> ring_geometry();
[[nodiscard]] RingValues make_ring_values(const UsageSnapshot& usage);
[[nodiscard]] FlushRect round_flush_rect(int x_start, int y_start, int x_end, int y_end);

// The software framebuffer adapter is deliberately separate from LVGL.  The
// same geometry and usage projection are used by host snapshots and by a
// future BSP view, so ring labels/percentages can never leak onto the rings.
void render_ring_layers(Framebuffer& framebuffer, const RingValues& values,
                        int center_x = kDisplayCenter, int center_y = kDisplayCenter,
                        int shift_x = 0, int shift_y = 0);

}  // namespace t3::ui
