#pragma once

#include "board_manifest.h"
#include "encoder.hpp"
#include "pcm_downmix.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace t3::companion {

enum class CaptureResult : std::uint8_t {
  Started,
  Accepted,
  Stopped,
  Unavailable,
  NotStarted,
  Disconnected,
  InvalidLayout,
};

struct Es7210CaptureConfig {
  std::uint32_t sample_rate_hz = kPcmCaptureSampleRateHz;
  StereoDownmixConfig downmix{};
};

// Host-safe capture abstraction.  It deliberately has no ESP-IDF/I2S side
// effects: physical capture remains disabled until the board manifest records
// an attributable, verified microphone mapping.
class Es7210Capture {
 public:
  explicit Es7210Capture(const t3::board::AudioManifest& manifest,
                         Es7210CaptureConfig config = {});

  [[nodiscard]] bool available() const { return available_; }
  [[nodiscard]] bool capturing() const { return capturing_; }
  [[nodiscard]] const Es7210CaptureConfig& config() const { return config_; }
  [[nodiscard]] CaptureResult start();
  [[nodiscard]] CaptureResult push_stereo(std::span<const std::int16_t> interleaved,
                                          std::vector<std::int16_t>& mono);
  [[nodiscard]] CaptureResult stop();
  [[nodiscard]] CaptureResult disconnect();

 private:
  bool available_ = false;
  bool capturing_ = false;
  bool disconnected_ = false;
  Es7210CaptureConfig config_;
};

[[nodiscard]] bool is_verified_es7210_mapping(const t3::board::AudioManifest& manifest);

}  // namespace t3::companion
