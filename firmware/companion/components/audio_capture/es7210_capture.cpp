#include "es7210_capture.hpp"

#include <string_view>

namespace t3::companion {

bool is_verified_es7210_mapping(const t3::board::AudioManifest& manifest) {
  // Official 1.75C wiring and the corrected MCLK16 probe establish the two
  // front inputs as ordinary STD stereo channels 0/1.  Keep the manifest
  // gate so an unverified board revision still fails closed.
  return manifest.codec == std::string_view("ES7210") &&
         manifest.front_mic_channels_verified && !manifest.mic_unresolved &&
         manifest.front_mic_channels[0] < 2 && manifest.front_mic_channels[1] < 2 &&
         manifest.front_mic_channels[0] != manifest.front_mic_channels[1];
}

Es7210Capture::Es7210Capture(const t3::board::AudioManifest& manifest,
                             Es7210CaptureConfig config)
    : available_(is_verified_es7210_mapping(manifest)), config_(config) {
  config_.downmix.left_channel = manifest.front_mic_channels[0];
  config_.downmix.right_channel = manifest.front_mic_channels[1];
  if (config_.sample_rate_hz != kPcmCaptureSampleRateHz ||
      config_.downmix.channel_count != 2) {
    available_ = false;
  }
}

CaptureResult Es7210Capture::start() {
  if (!available_) return CaptureResult::Unavailable;
  if (disconnected_) return CaptureResult::Disconnected;
  if (capturing_) return CaptureResult::Started;
  capturing_ = true;
  return CaptureResult::Started;
}

CaptureResult Es7210Capture::push_stereo(std::span<const std::int16_t> interleaved,
                                         std::vector<std::int16_t>& mono) {
  if (disconnected_) return CaptureResult::Disconnected;
  if (!capturing_) return CaptureResult::NotStarted;
  const auto result = downmix_stereo_to_mono(interleaved, config_.downmix);
  if (result.empty() && !interleaved.empty()) return CaptureResult::InvalidLayout;
  mono = result;
  return CaptureResult::Accepted;
}

CaptureResult Es7210Capture::stop() {
  if (!capturing_) return disconnected_ ? CaptureResult::Disconnected : CaptureResult::NotStarted;
  capturing_ = false;
  return CaptureResult::Stopped;
}

CaptureResult Es7210Capture::disconnect() {
  capturing_ = false;
  disconnected_ = true;
  return CaptureResult::Disconnected;
}

}  // namespace t3::companion
