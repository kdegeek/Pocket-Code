#include "audio_frame.hpp"

#include <algorithm>

namespace t3::companion {
namespace {

std::uint32_t read_be32(const std::uint8_t* bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

}  // namespace

DecodeResult<AudioFrame> decode_audio_frame(std::span<const std::uint8_t> frame,
                                             std::size_t max_chunk_bytes) {
  DecodeResult<AudioFrame> result;
  if (frame.size() < kAudioFrameHeaderBytes) {
    result.error = {DecodeError::Truncated, frame.size(), "audio frame header is truncated"};
    return result;
  }
  if (frame[0] != kAudioFrameVersion || frame[1] != kAudioFrameTypePcm) {
    result.error = {DecodeError::UnsupportedVersion, 0, "unsupported audio frame version or type"};
    return result;
  }
  const std::size_t payload_size = frame.size() - kAudioFrameHeaderBytes;
  if (payload_size > max_chunk_bytes || payload_size > 65'536) {
    result.error = {DecodeError::LimitExceeded, kAudioFrameHeaderBytes,
                    "audio payload exceeds negotiated chunk bound"};
    return result;
  }
  result.value.header.version = frame[0];
  result.value.header.frame_type = frame[1];
  std::copy_n(frame.begin() + 2, result.value.header.stream_id.bytes.size(),
              result.value.header.stream_id.bytes.begin());
  result.value.header.chunk_number = read_be32(frame.data() + 18);
  result.value.payload = frame.subspan(kAudioFrameHeaderBytes);
  return result;
}

DecodeResult<std::vector<std::uint8_t>> encode_audio_frame(
    const AudioFrameHeader& header, std::span<const std::uint8_t> payload,
    std::size_t max_chunk_bytes) {
  DecodeResult<std::vector<std::uint8_t>> result;
  if (header.version != kAudioFrameVersion || header.frame_type != kAudioFrameTypePcm) {
    result.error = {DecodeError::UnsupportedVersion, 0, "unsupported audio frame version or type"};
    return result;
  }
  if (payload.size() > max_chunk_bytes || payload.size() > 65'536) {
    result.error = {DecodeError::LimitExceeded, kAudioFrameHeaderBytes,
                    "audio payload exceeds negotiated chunk bound"};
    return result;
  }
  result.value.resize(kAudioFrameHeaderBytes + payload.size());
  result.value[0] = header.version;
  result.value[1] = header.frame_type;
  std::copy(header.stream_id.bytes.begin(), header.stream_id.bytes.end(), result.value.begin() + 2);
  result.value[18] = static_cast<std::uint8_t>(header.chunk_number >> 24U);
  result.value[19] = static_cast<std::uint8_t>(header.chunk_number >> 16U);
  result.value[20] = static_cast<std::uint8_t>(header.chunk_number >> 8U);
  result.value[21] = static_cast<std::uint8_t>(header.chunk_number);
  std::copy(payload.begin(), payload.end(), result.value.begin() + kAudioFrameHeaderBytes);
  return result;
}

bool AudioStreamGuard::start(const AudioStreamId& stream_id, std::uint32_t first_chunk) {
  stream_id_ = stream_id;
  next_chunk_ = first_chunk;
  active_ = true;
  return true;
}

bool AudioStreamGuard::accept(std::uint32_t chunk_number) {
  if (!active_ || chunk_number != next_chunk_) {
    return false;
  }
  ++next_chunk_;
  return true;
}

bool AudioStreamGuard::accept(const AudioStreamId& stream_id, std::uint32_t chunk_number) {
  if (!active_ || stream_id != stream_id_) {
    return false;
  }
  return accept(chunk_number);
}

bool AudioStreamGuard::stop() {
  if (!active_) {
    return false;
  }
  active_ = false;
  return true;
}

}  // namespace t3::companion
