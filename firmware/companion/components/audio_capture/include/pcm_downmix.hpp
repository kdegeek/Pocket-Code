#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace t3::companion {

// The verified 1.75C voice path is ordinary ES7210 STD stereo.  Keep the
// channel indices explicit so this seam cannot silently regress to a TDM
// slot assumption.
struct StereoDownmixConfig {
  std::size_t channel_count = 2;
  std::uint8_t left_channel = 0;
  std::uint8_t right_channel = 1;
};

using DownmixConfig = StereoDownmixConfig;
using PcmDownmixConfig = StereoDownmixConfig;

enum class DownmixResult : std::uint8_t {
  Accepted,
  InvalidLayout,
  OutputTooSmall,
};

[[nodiscard]] std::int16_t saturating_pcm16(std::int32_t value);
[[nodiscard]] std::int16_t saturating_average(std::int16_t left, std::int16_t right);

[[nodiscard]] DownmixResult downmix_stereo_to_mono(std::span<const std::int16_t> interleaved,
                                                    std::span<std::int16_t> output,
                                                    const StereoDownmixConfig& config,
                                                    std::size_t& produced);

[[nodiscard]] std::vector<std::int16_t> downmix_stereo_to_mono(
    std::span<const std::int16_t> interleaved,
    const StereoDownmixConfig& config = {});

}  // namespace t3::companion
