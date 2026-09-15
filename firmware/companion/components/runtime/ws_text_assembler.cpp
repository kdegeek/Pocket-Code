#include "ws_text_assembler.hpp"

namespace t3::companion::runtime {

WsTextMessageAssembler::WsTextMessageAssembler(std::size_t max_message_bytes)
    : max_message_bytes_(max_message_bytes) {}

std::optional<std::string_view> WsTextMessageAssembler::reject() {
  reset();
  return std::nullopt;
}

void WsTextMessageAssembler::reset() {
  buffer_.clear();
  expected_bytes_ = 0;
  active_ = false;
}

std::optional<std::string_view> WsTextMessageAssembler::accept(
    std::string_view chunk, std::int32_t payload_len,
    std::int32_t payload_offset, std::uint8_t opcode, bool fin) {
  if (payload_len <= 0 || payload_offset < 0) return reject();

  const auto total = static_cast<std::size_t>(payload_len);
  const auto offset = static_cast<std::size_t>(payload_offset);
  if (total > max_message_bytes_ || offset > total ||
      chunk.size() > total - offset) {
    return reject();
  }

  if (offset == 0U) {
    // A new text start while a message is incomplete would discard bytes
    // without an explicit connection reset, so treat it as malformed.
    if (active_ || opcode != kTextOpcode) return reject();
    reset();
    expected_bytes_ = total;
    buffer_.reserve(expected_bytes_);
    active_ = true;
  } else if (!active_ || expected_bytes_ != total ||
             (opcode != kTextOpcode && opcode != kContinuationOpcode) ||
             offset != buffer_.size()) {
    return reject();
  }

  buffer_.append(chunk.data(), chunk.size());
  if (buffer_.size() > expected_bytes_) return reject();
  if (buffer_.size() != expected_bytes_) return std::nullopt;

  // IDF may report fin=true for every read of one large frame. Therefore fin
  // gates emission only after the declared payload has been fully copied.
  if (!fin) return reject();

  active_ = false;
  expected_bytes_ = 0;
  return std::string_view(buffer_);
}

}  // namespace t3::companion::runtime
