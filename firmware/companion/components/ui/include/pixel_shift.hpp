#pragma once

#include <cstdint>

namespace t3::ui {

struct PixelShift {
  std::int8_t x = 0;
  std::int8_t y = 0;
  friend bool operator==(const PixelShift&, const PixelShift&) = default;
};

// Pixel shifting is bounded to one logical pixel and is explicitly disabled
// for deterministic snapshots.  The seed is provided by the caller so a
// runtime may vary it without making the host proof depend on wall-clock time.
[[nodiscard]] PixelShift pixel_shift(std::uint32_t seed, bool enabled, bool snapshot);

}  // namespace t3::ui
