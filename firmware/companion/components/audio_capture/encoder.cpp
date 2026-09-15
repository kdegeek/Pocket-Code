#include "encoder.hpp"

#include <limits>

namespace t3::companion {

EncoderResult PcmS16leEncoder::encode(std::span<const std::int16_t> samples,
                                      std::vector<std::uint8_t>& output) const {
  output.clear();
  if (sample_rate_hz_ != kPcmCaptureSampleRateHz || samples.size() > max_samples_ ||
      samples.size() > (std::numeric_limits<std::size_t>::max() / 2U)) {
    return samples.size() > max_samples_ ? EncoderResult::OutputTooLarge
                                         : EncoderResult::InvalidFormat;
  }
  output.reserve(samples.size() * 2U);
  for (const auto sample : samples) {
    const auto value = static_cast<std::uint16_t>(sample);
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
  }
  return EncoderResult::Encoded;
}

std::vector<std::uint8_t> encode_pcm_s16le_24khz_mono(
    std::span<const std::int16_t> samples) {
  std::vector<std::uint8_t> output;
  PcmS16leEncoder encoder;
  if (encoder.encode(samples, output) != EncoderResult::Encoded) output.clear();
  return output;
}

}  // namespace t3::companion
