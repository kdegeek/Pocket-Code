#pragma once

#include <cstdint>

namespace t3::companion {

inline constexpr std::uint8_t kAxpPkeyPositiveBit = 0x01;
inline constexpr std::uint8_t kAxpPkeyNegativeBit = 0x02;
inline constexpr std::uint8_t kAxpPkeyStatusMask =
    kAxpPkeyPositiveBit | kAxpPkeyNegativeBit;

struct AxpPkeyEdges {
  bool pressed = false;
  bool released = false;
};

// AXP2101 INTSTS2 is an edge/status byte, not a level register. Positive and
// negative PKEY bits are the verified press/release transitions.
[[nodiscard]] constexpr AxpPkeyEdges decode_axp_pkey_status(
    std::uint8_t status) noexcept {
  return AxpPkeyEdges{(status & kAxpPkeyPositiveBit) != 0U,
                      (status & kAxpPkeyNegativeBit) != 0U};
}

}  // namespace t3::companion
