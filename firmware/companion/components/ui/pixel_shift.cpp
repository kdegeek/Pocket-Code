#include "pixel_shift.hpp"

namespace t3::ui {

PixelShift pixel_shift(std::uint32_t seed, bool enabled, bool snapshot) {
  if (!enabled || snapshot) {
    return {};
  }
  // A tiny integer mixer keeps the runtime shift deterministic for a caller's
  // supplied seed while avoiding a wall-clock dependency in the renderer.
  std::uint32_t mixed = seed ^ 0x9E3779B9U;
  mixed ^= mixed >> 16U;
  mixed *= 0x7FEB352DU;
  mixed ^= mixed >> 15U;
  const auto x = static_cast<std::int8_t>(static_cast<int>((mixed >> 1U) % 3U) - 1);
  const auto y = static_cast<std::int8_t>(static_cast<int>((mixed >> 5U) % 3U) - 1);
  return PixelShift{x, y};
}

}  // namespace t3::ui
