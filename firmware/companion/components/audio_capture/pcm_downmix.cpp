#include "pcm_downmix.hpp"

#include <algorithm>

namespace t3::companion {

std::int16_t saturating_pcm16(std::int32_t value) {
  constexpr std::int32_t kMin = -32'768;
  constexpr std::int32_t kMax = 32'767;
  return static_cast<std::int16_t>(std::clamp(value, kMin, kMax));
}

std::int16_t saturating_average(std::int16_t left, std::int16_t right) {
  // Widen before adding.  Rounding toward zero is deterministic on both
  // host and ESP32 and avoids undefined signed overflow at the endpoints.
  return saturating_pcm16((static_cast<std::int32_t>(left) +
                           static_cast<std::int32_t>(right)) /
                          2);
}

DownmixResult downmix_stereo_to_mono(std::span<const std::int16_t> interleaved,
                                     std::span<std::int16_t> output,
                                     const StereoDownmixConfig& config,
                                     std::size_t& produced) {
  produced = 0;
  if (config.channel_count != 2 || config.left_channel >= config.channel_count ||
      config.right_channel >= config.channel_count ||
      config.left_channel == config.right_channel ||
      (interleaved.size() % config.channel_count) != 0) {
    return DownmixResult::InvalidLayout;
  }
  const std::size_t frames = interleaved.size() / config.channel_count;
  if (output.size() < frames) return DownmixResult::OutputTooSmall;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const auto base = frame * config.channel_count;
    output[frame] = saturating_average(interleaved[base + config.left_channel],
                                       interleaved[base + config.right_channel]);
  }
  produced = frames;
  return DownmixResult::Accepted;
}

std::vector<std::int16_t> downmix_stereo_to_mono(std::span<const std::int16_t> interleaved,
                                                  const StereoDownmixConfig& config) {
  const std::size_t frames = config.channel_count == 0
                                 ? 0
                                 : interleaved.size() / config.channel_count;
  std::vector<std::int16_t> output(frames);
  std::size_t produced = 0;
  if (downmix_stereo_to_mono(interleaved, output, config, produced) != DownmixResult::Accepted) {
    output.clear();
  } else {
    output.resize(produced);
  }
  return output;
}

}  // namespace t3::companion
