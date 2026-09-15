#pragma once

#include "envelope.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace t3::companion {

inline constexpr std::size_t kAudioFrameHeaderBytes = 22;
inline constexpr std::size_t COMPANION_AUDIO_FRAME_HEADER_BYTES = kAudioFrameHeaderBytes;
inline constexpr std::uint8_t kAudioFrameVersion = 1;
inline constexpr std::uint8_t kAudioFrameTypePcm = 1;

struct AudioStreamId {
  std::array<std::uint8_t, 16> bytes{};
  friend bool operator==(const AudioStreamId&, const AudioStreamId&) = default;
};

struct AudioFrameHeader {
  std::uint8_t version = kAudioFrameVersion;
  std::uint8_t frame_type = kAudioFrameTypePcm;
  AudioStreamId stream_id;
  std::uint32_t chunk_number = 0;
};

struct AudioFrame {
  AudioFrameHeader header;
  std::span<const std::uint8_t> payload;
};

DecodeResult<AudioFrame> decode_audio_frame(std::span<const std::uint8_t> frame,
                                             std::size_t max_chunk_bytes);
DecodeResult<std::vector<std::uint8_t>> encode_audio_frame(
    const AudioFrameHeader& header, std::span<const std::uint8_t> payload,
    std::size_t max_chunk_bytes);

class AudioStreamGuard {
 public:
  [[nodiscard]] bool start(const AudioStreamId& stream_id, std::uint32_t first_chunk = 0);
  [[nodiscard]] bool accept(std::uint32_t chunk_number);
  [[nodiscard]] bool accept(const AudioStreamId& stream_id, std::uint32_t chunk_number);
  [[nodiscard]] bool stop();
  [[nodiscard]] bool active() const { return active_; }

 private:
  bool active_ = false;
  AudioStreamId stream_id_;
  std::uint32_t next_chunk_ = 0;
};

}  // namespace t3::companion
