#pragma once

#include "audio_frame.hpp"
#include "encoder.hpp"
#include "pcm_downmix.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace t3::companion {

enum class StreamResult : std::uint8_t {
  Started,
  Accepted,
  Stopped,
  Disconnected,
  Backpressure,
  UnknownStream,
  InvalidPayload,
};

struct AudioStreamLimits {
  AudioStreamId stream_id;
  std::size_t max_chunks = 8;
  std::size_t max_bytes = 16 * 1024;
};

// A bounded, ephemeral sender queue.  It owns only in-flight binary frames;
// stop/release/disconnect clear the queue before returning to the caller.
class AudioStreamSession {
 public:
  explicit AudioStreamSession(AudioStreamLimits limits);

  [[nodiscard]] StreamResult start(std::uint32_t first_chunk = 0);
  [[nodiscard]] StreamResult push(AudioStreamId stream_id,
                                  std::span<const std::uint8_t> pcm_payload);
  [[nodiscard]] StreamResult push_pcm(AudioStreamId stream_id,
                                      std::span<const std::int16_t> stereo_samples,
                                      const StereoDownmixConfig& downmix = {});
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> pop_frame();
  [[nodiscard]] StreamResult on_server_stop(AudioStreamId stream_id);
  [[nodiscard]] StreamResult stop();
  [[nodiscard]] StreamResult release() { return stop(); }
  [[nodiscard]] StreamResult on_disconnect();

  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] std::size_t pending_chunks() const { return frames_.size(); }
  [[nodiscard]] std::size_t pending_bytes() const { return pending_bytes_; }
  [[nodiscard]] std::uint32_t next_chunk() const { return next_chunk_; }
  [[nodiscard]] const AudioStreamId& stream_id() const { return limits_.stream_id; }

 private:
  [[nodiscard]] StreamResult inactive_result() const;
  void clear_pending();

  AudioStreamLimits limits_;
  std::deque<std::vector<std::uint8_t>> frames_;
  std::size_t pending_bytes_ = 0;
  std::uint32_t next_chunk_ = 0;
  bool active_ = false;
  bool disconnected_ = false;
  bool stopped_ = false;
};

}  // namespace t3::companion
