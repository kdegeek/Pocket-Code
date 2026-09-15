#include "backpressure.hpp"

namespace t3::companion {

AudioStreamSession::AudioStreamSession(AudioStreamLimits limits) : limits_(limits) {}

StreamResult AudioStreamSession::start(std::uint32_t first_chunk) {
  clear_pending();
  disconnected_ = false;
  stopped_ = false;
  next_chunk_ = first_chunk;
  active_ = true;
  return StreamResult::Started;
}

StreamResult AudioStreamSession::inactive_result() const {
  if (disconnected_) return StreamResult::Disconnected;
  return StreamResult::Stopped;
}

StreamResult AudioStreamSession::push(AudioStreamId stream_id,
                                      std::span<const std::uint8_t> pcm_payload) {
  if (stream_id != limits_.stream_id) return StreamResult::UnknownStream;
  if (!active_) return inactive_result();
  if (pcm_payload.empty() || pcm_payload.size() > limits_.max_bytes) {
    return StreamResult::InvalidPayload;
  }
  if (frames_.size() >= limits_.max_chunks ||
      pcm_payload.size() > limits_.max_bytes - pending_bytes_) {
    return StreamResult::Backpressure;
  }
  AudioFrameHeader header;
  header.stream_id = stream_id;
  header.chunk_number = next_chunk_++;
  const auto encoded = encode_audio_frame(header, pcm_payload, limits_.max_bytes);
  if (!encoded.ok()) return StreamResult::InvalidPayload;
  pending_bytes_ += encoded.value.size();
  frames_.push_back(encoded.value);
  return StreamResult::Accepted;
}

StreamResult AudioStreamSession::push_pcm(AudioStreamId stream_id,
                                          std::span<const std::int16_t> stereo_samples,
                                          const StereoDownmixConfig& downmix) {
  const auto mono = downmix_stereo_to_mono(stereo_samples, downmix);
  if (mono.empty() && !stereo_samples.empty()) return StreamResult::InvalidPayload;
  const auto encoded = encode_pcm_s16le_24khz_mono(mono);
  return push(stream_id, encoded);
}

std::optional<std::vector<std::uint8_t>> AudioStreamSession::pop_frame() {
  if (frames_.empty()) return std::nullopt;
  std::vector<std::uint8_t> frame = std::move(frames_.front());
  frames_.pop_front();
  pending_bytes_ -= frame.size();
  return frame;
}

StreamResult AudioStreamSession::on_server_stop(AudioStreamId stream_id) {
  if (stream_id != limits_.stream_id) return StreamResult::UnknownStream;
  if (!active_) return inactive_result();
  active_ = false;
  stopped_ = true;
  clear_pending();
  return StreamResult::Stopped;
}

StreamResult AudioStreamSession::stop() {
  if (!active_) return inactive_result();
  active_ = false;
  stopped_ = true;
  clear_pending();
  return StreamResult::Stopped;
}

StreamResult AudioStreamSession::on_disconnect() {
  active_ = false;
  disconnected_ = true;
  stopped_ = false;
  clear_pending();
  return StreamResult::Disconnected;
}

void AudioStreamSession::clear_pending() {
  frames_.clear();
  pending_bytes_ = 0;
}

}  // namespace t3::companion
