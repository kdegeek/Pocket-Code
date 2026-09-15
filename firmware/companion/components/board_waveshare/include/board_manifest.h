#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace t3::board {

enum class ButtonPosition : std::uint8_t {
  Unknown = 0,
  UpperRight,
  BottomRight,
  Pwr,
  Boot,
};

struct ChipManifest {
  std::string_view model;
  std::uint16_t revision;
  std::array<std::uint8_t, 6> mac;
};

struct FlashManifest {
  std::uint32_t size_bytes;
  std::uint32_t detected_id;
  std::string_view mode;
  std::uint32_t frequency_hz;
};

struct PsramManifest {
  std::uint32_t size_bytes;
  std::string_view mode;
};

struct DisplayManifest {
  std::string_view controller;
  std::uint16_t width;
  std::uint16_t height;
  std::uint8_t column_gap;
  std::uint8_t rotation;
  bool brightness_display_off_verified;
  bool hard_power_off_avoided;
};

struct TouchManifest {
  std::string_view controller;
  std::uint8_t i2c_sda;
  std::uint8_t i2c_scl;
  std::uint8_t irq_gpio;
  std::uint8_t reset_gpio;
  bool irq_active_low;
  bool swap_xy;
  bool mirror_x;
  bool mirror_y;
};

struct ButtonBinding {
  std::string_view source;
  std::int8_t gpio;
  bool active_high;
};

struct ButtonsManifest {
  ButtonBinding pwr;
  ButtonBinding boot;
  ButtonPosition upper_right;
  ButtonPosition bottom_right;
  std::uint16_t debounce_ms;
  bool transitions_observed;
};

struct AudioManifest {
  std::string_view codec;
  std::uint8_t i2s_mclk;
  std::uint8_t i2s_bclk;
  std::uint8_t i2s_ws;
  std::uint8_t i2s_tx;
  std::uint8_t i2s_rx;
  // ES7210 MIC1|MIC2 are exposed as ordinary STD stereo channels 0 (left)
  // and 1 (right) on the verified 1.75C wiring.  These are channels, not TDM
  // slots; the runtime capture seam downmixes the two channels to mono.
  std::array<std::uint8_t, 2> front_mic_channels;
  std::array<float, 2> last_rms;
  std::array<std::int32_t, 2> last_peak;
  bool front_mic_channels_verified;
  // Retained as an explicit fail-closed guard for future board revisions.
  bool mic_unresolved;
};

struct BoardManifest {
  std::string_view sku;
  std::string_view hardware_revision;
  bool verified;
  ChipManifest chip;
  FlashManifest flash;
  PsramManifest psram;
  DisplayManifest display;
  TouchManifest touch;
  ButtonsManifest buttons;
  AudioManifest audio;
};

// The manifest is intentionally a value object so host tests can enforce the
// public board contract without pulling in ESP-IDF headers.
const BoardManifest &verified_manifest();

}  // namespace t3::board
