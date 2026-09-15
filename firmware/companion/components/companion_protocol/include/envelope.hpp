#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace t3::companion {

enum class DecodeError {
  None,
  Truncated,
  MalformedJson,
  UnsupportedVersion,
  UnknownType,
  MissingField,
  InvalidValue,
  LimitExceeded,
  Duplicate,
  OutOfOrder,
};

struct DecodeFailure {
  DecodeError code = DecodeError::None;
  std::size_t offset = 0;
  const char* message = "";
};

template <typename T>
struct DecodeResult {
  T value{};
  DecodeFailure error{};

  [[nodiscard]] bool ok() const { return error.code == DecodeError::None; }
  explicit operator bool() const { return ok(); }
};

DecodeResult<Hello> decode_hello(std::string_view json);
DecodeResult<HelloAck> decode_hello_ack(std::string_view json);
DecodeResult<Snapshot> decode_snapshot(std::string_view json);
DecodeResult<BoundedArray<PendingRequest, kMaxPendingRequests>>
decode_pending_requests(std::string_view json);
DecodeResult<CommandStatuses> decode_command_statuses(std::string_view json);
DecodeResult<DeviceCommand> decode_device_command(std::string_view json);
DecodeResult<ServerEvent> decode_server_event(std::string_view json);
DecodeResult<Envelope> decode_envelope(std::string_view json);

DecodeResult<std::string> encode_hello(const Hello& hello);

// A replay cursor is deliberately separate from JSON decoding.  Device
// commands are deduplicated by commandId; server events must advance the
// environment-scoped adapter sequence exactly once.
class ReplayGuard {
 public:
  [[nodiscard]] bool accept(std::uint64_t adapter_sequence);
  [[nodiscard]] std::uint64_t last_sequence() const { return last_sequence_; }

 private:
  bool initialized_ = false;
  std::uint64_t last_sequence_ = 0;
};

}  // namespace t3::companion
