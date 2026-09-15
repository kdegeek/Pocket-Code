#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace t3::companion {

inline constexpr std::uint32_t kPcmCaptureSampleRateHz = 24'000;
inline constexpr std::uint32_t kPcmCaptureChannels = 1;

enum class EncoderResult : std::uint8_t {
  Encoded,
  InvalidFormat,
  OutputTooLarge,
};

class PcmS16leEncoder {
 public:
  explicit PcmS16leEncoder(std::uint32_t sample_rate_hz = kPcmCaptureSampleRateHz,
                           std::size_t max_samples = 2'048)
      : sample_rate_hz_(sample_rate_hz), max_samples_(max_samples) {}

  [[nodiscard]] EncoderResult encode(std::span<const std::int16_t> samples,
                                     std::vector<std::uint8_t>& output) const;
  [[nodiscard]] std::uint32_t sample_rate_hz() const { return sample_rate_hz_; }
  [[nodiscard]] std::uint32_t channels() const { return kPcmCaptureChannels; }
  [[nodiscard]] std::size_t max_samples() const { return max_samples_; }

 private:
  std::uint32_t sample_rate_hz_;
  std::size_t max_samples_;
};

[[nodiscard]] std::vector<std::uint8_t> encode_pcm_s16le_24khz_mono(
    std::span<const std::int16_t> samples);

}  // namespace t3::companion
