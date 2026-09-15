#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace t3::companion::runtime {

/**
 * Reassembles the bounded text payload chunks emitted by esp_websocket_client.
 * The IDF adapter reports the total payload length and the offset of each
 * buffer-sized read; a completed view is valid until the next reset or start.
 */
class WsTextMessageAssembler final {
 public:
  static constexpr std::uint8_t kContinuationOpcode = 0x0U;
  static constexpr std::uint8_t kTextOpcode = 0x1U;

  explicit WsTextMessageAssembler(std::size_t max_message_bytes = 65'536U);

  /**
   * Accept one IDF event chunk. A value is returned only once the declared
   * payload is contiguous, bounded, and marked final. Any malformed chunk
   * clears the partial message and returns no value.
   */
  [[nodiscard]] std::optional<std::string_view> accept(
      std::string_view chunk, std::int32_t payload_len,
      std::int32_t payload_offset, std::uint8_t opcode, bool fin);

  /** Discard a partial or completed payload, including its retained bytes. */
  void reset();

  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] std::size_t buffered_bytes() const { return buffer_.size(); }

 private:
  [[nodiscard]] std::optional<std::string_view> reject();

  std::size_t max_message_bytes_ = 0;
  std::size_t expected_bytes_ = 0;
  std::string buffer_;
  bool active_ = false;
};

}  // namespace t3::companion::runtime
