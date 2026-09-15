#include "envelope.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

namespace t3::companion {
namespace {

enum class FieldResult { Handled, Unknown, Error };

class JsonReader {
 public:
  explicit JsonReader(std::string_view input) : input_(input) {}

  [[nodiscard]] bool ok() const { return error_.code == DecodeError::None; }
  [[nodiscard]] const DecodeFailure& error() const { return error_; }
  [[nodiscard]] std::size_t position() const { return position_; }

  bool semantic(DecodeError code, const char* message) { return fail(code, message); }

  void skip_whitespace() {
    while (position_ < input_.size()) {
      const char value = input_[position_];
      if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
        break;
      }
      ++position_;
    }
  }

  [[nodiscard]] char peek() {
    skip_whitespace();
    return position_ < input_.size() ? input_[position_] : '\0';
  }

  bool expect(char expected) {
    skip_whitespace();
    if (position_ >= input_.size() || input_[position_] != expected) {
      return fail(DecodeError::MalformedJson, "expected JSON delimiter");
    }
    ++position_;
    return true;
  }

  bool complete() {
    skip_whitespace();
    if (position_ != input_.size()) {
      return fail(DecodeError::MalformedJson, "trailing JSON data");
    }
    return true;
  }

  template <std::size_t MaxBytes>
  bool string(BoundedString<MaxBytes>& output, bool require_non_empty = true) {
    skip_whitespace();
    if (position_ >= input_.size() || input_[position_] != '"') {
      return fail(DecodeError::MalformedJson, "expected JSON string");
    }
    ++position_;
    std::array<char, MaxBytes + 1> decoded{};
    std::size_t decoded_size = 0;
    while (position_ < input_.size()) {
      const unsigned char value = static_cast<unsigned char>(input_[position_++]);
      if (value == '"') {
        std::string_view view(decoded.data(), decoded_size);
        if (!output.assign(view, require_non_empty)) {
          return fail(DecodeError::LimitExceeded, "JSON string exceeds its bound");
        }
        return true;
      }
      if (value < 0x20) {
        return fail(DecodeError::MalformedJson, "control byte in JSON string");
      }
      if (value != '\\') {
        if (decoded_size >= MaxBytes) {
          return fail(DecodeError::LimitExceeded, "JSON string exceeds its bound");
        }
        decoded[decoded_size++] = static_cast<char>(value);
        continue;
      }
      if (position_ >= input_.size()) {
        return fail(DecodeError::Truncated, "truncated JSON escape");
      }
      const char escape = input_[position_++];
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          if (!append_char(decoded, decoded_size, MaxBytes, escape)) {
            return false;
          }
          break;
        case 'b':
          if (!append_char(decoded, decoded_size, MaxBytes, '\b')) {
            return false;
          }
          break;
        case 'f':
          if (!append_char(decoded, decoded_size, MaxBytes, '\f')) {
            return false;
          }
          break;
        case 'n':
          if (!append_char(decoded, decoded_size, MaxBytes, '\n')) {
            return false;
          }
          break;
        case 'r':
          if (!append_char(decoded, decoded_size, MaxBytes, '\r')) {
            return false;
          }
          break;
        case 't':
          if (!append_char(decoded, decoded_size, MaxBytes, '\t')) {
            return false;
          }
          break;
        case 'u': {
          std::uint32_t code_point = 0;
          if (!unicode_escape(code_point)) {
            return false;
          }
          if (code_point <= 0x7f) {
            if (!append_char(decoded, decoded_size, MaxBytes,
                             static_cast<char>(code_point))) {
              return false;
            }
          } else if (code_point <= 0x7ff) {
            if (!append_char(decoded, decoded_size, MaxBytes,
                             static_cast<char>(0xc0 | (code_point >> 6))) ||
                !append_char(decoded, decoded_size, MaxBytes,
                             static_cast<char>(0x80 | (code_point & 0x3f)))) {
              return false;
            }
          } else {
            if (!append_char(decoded, decoded_size, MaxBytes,
                             static_cast<char>(0xe0 | (code_point >> 12))) ||
                !append_char(decoded, decoded_size, MaxBytes,
                             static_cast<char>(0x80 | ((code_point >> 6) & 0x3f))) ||
                !append_char(decoded, decoded_size, MaxBytes,
                             static_cast<char>(0x80 | (code_point & 0x3f)))) {
              return false;
            }
          }
          break;
        }
        default:
          return fail(DecodeError::MalformedJson, "invalid JSON escape");
      }
    }
    return fail(DecodeError::Truncated, "unterminated JSON string");
  }

  bool uint64(std::uint64_t& output) {
    std::string_view token;
    if (!number_token(token, false)) {
      return false;
    }
    const auto result = std::from_chars(token.data(), token.data() + token.size(), output, 10);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
      return fail(DecodeError::InvalidValue, "expected non-negative integer");
    }
    return true;
  }

  bool number(double& output) {
    std::string_view token;
    if (!number_token(token, true)) {
      return false;
    }
    if (token.size() >= 64) {
      return fail(DecodeError::LimitExceeded, "number token exceeds its bound");
    }
    std::array<char, 64> buffer{};
    std::copy(token.begin(), token.end(), buffer.begin());
    char* end = nullptr;
    output = std::strtod(buffer.data(), &end);
    if (end != buffer.data() + token.size() || !std::isfinite(output)) {
      return fail(DecodeError::InvalidValue, "invalid JSON number");
    }
    return true;
  }

  bool boolean(bool& output) {
    skip_whitespace();
    if (input_.substr(position_, 4) == "true") {
      position_ += 4;
      output = true;
      return true;
    }
    if (input_.substr(position_, 5) == "false") {
      position_ += 5;
      output = false;
      return true;
    }
    return fail(DecodeError::InvalidValue, "expected boolean");
  }

  bool null_value() {
    skip_whitespace();
    if (input_.substr(position_, 4) != "null") {
      return fail(DecodeError::InvalidValue, "expected null");
    }
    position_ += 4;
    return true;
  }

  template <typename Callback>
  bool object(Callback&& callback) {
    if (!expect('{')) {
      return false;
    }
    skip_whitespace();
    if (peek() == '}') {
      ++position_;
      return true;
    }
    std::size_t field_count = 0;
    while (position_ < input_.size()) {
      if (++field_count > 256) {
        return fail(DecodeError::LimitExceeded, "object has too many fields");
      }
      BoundedString<kMaxShortStringBytes> key;
      if (!string(key)) {
        return false;
      }
      if (!expect(':')) {
        return false;
      }
      const FieldResult result = callback(key.view());
      if (result == FieldResult::Unknown && !skip_value(0)) {
        return false;
      }
      if (result == FieldResult::Error || !ok()) {
        return false;
      }
      skip_whitespace();
      if (position_ < input_.size() && input_[position_] == '}') {
        ++position_;
        return true;
      }
      if (!expect(',')) {
        return false;
      }
    }
    return fail(DecodeError::Truncated, "unterminated JSON object");
  }

  template <typename Callback>
  bool array(Callback&& callback, std::size_t max_items) {
    if (!expect('[')) {
      return false;
    }
    skip_whitespace();
    if (peek() == ']') {
      ++position_;
      return true;
    }
    std::size_t item_count = 0;
    while (position_ < input_.size()) {
      if (item_count >= max_items) {
        return fail(DecodeError::LimitExceeded, "array exceeds its bound");
      }
      if (!callback(item_count++)) {
        return false;
      }
      skip_whitespace();
      if (position_ < input_.size() && input_[position_] == ']') {
        ++position_;
        return true;
      }
      if (!expect(',')) {
        return false;
      }
    }
    return fail(DecodeError::Truncated, "unterminated JSON array");
  }

  bool skip_value(std::size_t depth) {
    if (depth > kMaxJsonDepth) {
      return fail(DecodeError::LimitExceeded, "JSON nesting exceeds its bound");
    }
    const char value = peek();
    if (value == '"') {
      BoundedString<kMaxLongStringBytes> ignored;
      return string(ignored, false);
    }
    if (value == '{') {
      return object([&](std::string_view) {
        return skip_value(depth + 1) ? FieldResult::Handled : FieldResult::Error;
      });
    }
    if (value == '[') {
      return array([&](std::size_t) {
        return skip_value(depth + 1);
      }, 256);
    }
    if (value == 't' || value == 'f') {
      bool ignored = false;
      return boolean(ignored);
    }
    if (value == 'n') {
      return null_value();
    }
    double ignored = 0;
    return number(ignored);
  }

 private:
  template <std::size_t ArraySize>
  bool append_char(std::array<char, ArraySize>& output, std::size_t& size,
                   std::size_t max_bytes, char value) {
    if (size >= max_bytes) {
      return fail(DecodeError::LimitExceeded, "JSON string exceeds its bound");
    }
    output[size++] = value;
    return true;
  }

  bool unicode_escape(std::uint32_t& code_point) {
    if (position_ + 4 > input_.size()) {
      return fail(DecodeError::Truncated, "truncated unicode escape");
    }
    code_point = 0;
    for (std::size_t index = 0; index < 4; ++index) {
      const char value = input_[position_++];
      code_point <<= 4;
      if (value >= '0' && value <= '9') {
        code_point |= static_cast<std::uint32_t>(value - '0');
      } else if (value >= 'a' && value <= 'f') {
        code_point |= static_cast<std::uint32_t>(value - 'a' + 10);
      } else if (value >= 'A' && value <= 'F') {
        code_point |= static_cast<std::uint32_t>(value - 'A' + 10);
      } else {
        return fail(DecodeError::MalformedJson, "invalid unicode escape");
      }
    }
    if (code_point >= 0xd800 && code_point <= 0xdfff) {
      return fail(DecodeError::InvalidValue, "surrogate unicode escape is unsupported");
    }
    return true;
  }

  bool number_token(std::string_view& token, bool allow_fraction) {
    skip_whitespace();
    const std::size_t start = position_;
    if (position_ < input_.size() && input_[position_] == '-') {
      ++position_;
    }
    if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
      return fail(DecodeError::InvalidValue, "expected JSON number");
    }
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
        return fail(DecodeError::MalformedJson, "leading zero in JSON number");
      }
    } else {
      while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
        ++position_;
      }
    }
    if (allow_fraction && position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      const std::size_t fraction_start = position_;
      while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
        ++position_;
      }
      if (fraction_start == position_) {
        return fail(DecodeError::MalformedJson, "fraction has no digits");
      }
    }
    if (allow_fraction && position_ < input_.size() &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      const std::size_t exponent_start = position_;
      while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
        ++position_;
      }
      if (exponent_start == position_) {
        return fail(DecodeError::MalformedJson, "exponent has no digits");
      }
    }
    token = input_.substr(start, position_ - start);
    return true;
  }

  bool fail(DecodeError code, const char* message) {
    if (error_.code == DecodeError::None) {
      error_ = DecodeFailure{code, position_, message};
    }
    return false;
  }

  std::string_view input_;
  std::size_t position_ = 0;
  DecodeFailure error_{};
};

template <std::size_t MaxBytes>
bool set_string(JsonReader& reader, BoundedString<MaxBytes>& target, bool required = true) {
  return reader.string(target, required);
}

bool field_seen(std::uint64_t& seen, std::size_t bit, JsonReader& reader) {
  if (bit >= 64) {
    return true;
  }
  const std::uint64_t mask = 1ULL << bit;
  if ((seen & mask) != 0) {
    reader.semantic(DecodeError::Duplicate, "duplicate protocol field");
    return false;
  }
  seen |= mask;
  return true;
}

template <typename T>
DecodeResult<T> finish(JsonReader& reader, T value) {
  DecodeResult<T> result;
  result.value = std::move(value);
  if (!reader.ok()) {
    result.error = reader.error();
    return result;
  }
  if (!reader.complete()) {
    result.error = reader.error();
    return result;
  }
  return result;
}

template <typename T>
DecodeResult<T> failure(const DecodeFailure& error) {
  DecodeResult<T> result;
  result.error = error;
  return result;
}

// The protocol-version helper above needs to report a semantic error rather
// than a parser error.  This small wrapper parses the scalar in callers and
// maps it after the object is complete.
bool read_version(JsonReader& reader, std::uint8_t& version, bool& unsupported) {
  std::uint64_t value = 0;
  if (!reader.uint64(value)) {
    return false;
  }
  if (value != kProtocolVersion) {
    unsupported = true;
    version = static_cast<std::uint8_t>(value & 0xffU);
    return true;
  }
  version = kProtocolVersion;
  return true;
}

bool read_provider(JsonReader& reader, Provider& output) {
  ShortString value;
  if (!reader.string(value)) {
    return false;
  }
  if (value.view() == "codex") {
    output = Provider::Codex;
  } else if (value.view() == "claude") {
    output = Provider::Claude;
  } else if (value.view() == "xai") {
    output = Provider::Xai;
  } else {
    return false;
  }
  return true;
}

bool read_status(JsonReader& reader, CommandStatus& output) {
  ShortString value;
  if (!reader.string(value)) {
    return false;
  }
  if (value.view() == "received") {
    output = CommandStatus::Received;
  } else if (value.view() == "committed") {
    output = CommandStatus::Committed;
  } else if (value.view() == "applied") {
    output = CommandStatus::Applied;
  } else if (value.view() == "rejected") {
    output = CommandStatus::Rejected;
  } else {
    return false;
  }
  return true;
}

bool read_usage_status(JsonReader& reader, UsageStatus& output) {
  ShortString value;
  if (!reader.string(value)) {
    return false;
  }
  if (value.view() == "available") {
    output = UsageStatus::Available;
  } else if (value.view() == "stale") {
    output = UsageStatus::Stale;
  } else if (value.view() == "unavailable") {
    output = UsageStatus::Unavailable;
  } else if (value.view() == "error") {
    output = UsageStatus::Error;
  } else {
    return false;
  }
  return true;
}

bool read_provisioning_status(JsonReader& reader, ProvisioningStatus& output) {
  ShortString value;
  if (!reader.string(value)) {
    return false;
  }
  if (value.view() == "started") {
    output = ProvisioningStatus::Started;
  } else if (value.view() == "completed") {
    output = ProvisioningStatus::Completed;
  } else if (value.view() == "cancelled") {
    output = ProvisioningStatus::Cancelled;
  } else if (value.view() == "error") {
    output = ProvisioningStatus::Error;
  } else {
    return false;
  }
  return true;
}

bool read_decision(JsonReader& reader, ApprovalDecision& output) {
  ShortString value;
  if (!reader.string(value)) {
    return false;
  }
  if (value.view() == "accept") {
    output = ApprovalDecision::Accept;
  } else if (value.view() == "decline") {
    output = ApprovalDecision::Decline;
  } else if (value.view() == "cancel") {
    output = ApprovalDecision::Cancel;
  } else {
    return false;
  }
  return true;
}

bool read_work_state(JsonReader& reader, WorkItemState& output) {
  ShortString value;
  if (!reader.string(value)) {
    return false;
  }
  if (value.view() == "needs_input") {
    output = WorkItemState::NeedsInput;
  } else if (value.view() == "failed") {
    output = WorkItemState::Failed;
  } else if (value.view() == "blocked") {
    output = WorkItemState::Blocked;
  } else if (value.view() == "working") {
    output = WorkItemState::Working;
  } else if (value.view() == "completed") {
    output = WorkItemState::Completed;
  } else if (value.view() == "idle") {
    output = WorkItemState::Idle;
  } else {
    return false;
  }
  return true;
}

bool read_iso_date(JsonReader& reader, DateTime& output) {
  return reader.string(output);
}

bool parse_audio_capability(JsonReader& reader, AudioCapability& output) {
  std::uint64_t seen = 0;
  bool valid = reader.object([&](std::string_view key) {
    if (key == "codec") {
      if (!field_seen(seen, 0, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      if (value.view() == "pcm_s16le") output.codec = AudioCodec::PcmS16le;
      else if (value.view() == "opus") output.codec = AudioCodec::Opus;
      else return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "sampleRateHz") {
      if (!field_seen(seen, 1, reader)) return FieldResult::Error;
      std::uint64_t value = 0;
      if (!reader.uint64(value) || value < 8'000 || value > 48'000) return FieldResult::Error;
      output.sample_rate_hz = static_cast<std::uint32_t>(value);
      return FieldResult::Handled;
    }
    if (key == "channels") {
      if (!field_seen(seen, 2, reader)) return FieldResult::Error;
      std::uint64_t value = 0;
      if (!reader.uint64(value) || value < 1 || value > 2) return FieldResult::Error;
      output.channels = static_cast<std::uint32_t>(value);
      return FieldResult::Handled;
    }
    if (key == "maxChunkBytes") {
      if (!field_seen(seen, 3, reader)) return FieldResult::Error;
      std::uint64_t value = 0;
      if (!reader.uint64(value) || value < 1 || value > 65'536) return FieldResult::Error;
      output.max_chunk_bytes = static_cast<std::uint32_t>(value);
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 0x0fU) == 0x0fU;
}

bool parse_capabilities(JsonReader& reader, Capabilities& output) {
  std::uint64_t seen = 0;
  bool valid = reader.object([&](std::string_view key) {
    if (key == "audio") {
      if (!field_seen(seen, 0, reader)) return FieldResult::Error;
      AudioCapability capability;
      if (!parse_audio_capability(reader, capability)) return FieldResult::Error;
      output.audio = capability;
      return FieldResult::Handled;
    }
    if (key == "maxMessageBytes") {
      if (!field_seen(seen, 1, reader)) return FieldResult::Error;
      std::uint64_t value = 0;
      if (!reader.uint64(value) || value < 1 || value > 1'048'576) return FieldResult::Error;
      output.max_message_bytes = static_cast<std::uint32_t>(value);
      return FieldResult::Handled;
    }
    if (key == "maxPendingCommands") {
      if (!field_seen(seen, 2, reader)) return FieldResult::Error;
      std::uint64_t value = 0;
      if (!reader.uint64(value) || value < 1 || value > kMaxPendingCommands) return FieldResult::Error;
      output.max_pending_commands = static_cast<std::uint32_t>(value);
      return FieldResult::Handled;
    }
    if (key == "maxPendingRequests") {
      if (!field_seen(seen, 3, reader)) return FieldResult::Error;
      std::uint64_t value = 0;
      if (!reader.uint64(value) || value < 1 || value > kMaxPendingRequests) return FieldResult::Error;
      output.max_pending_requests = static_cast<std::uint32_t>(value);
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 0x0eU) == 0x0eU;
}

bool parse_firmware(JsonReader& reader, FirmwareInfo& output) {
  std::uint64_t seen = 0;
  bool valid = reader.object([&](std::string_view key) {
    if (key == "version") {
      if (!field_seen(seen, 0, reader) || !set_string(reader, output.version)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "build") {
      if (!field_seen(seen, 1, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      output.build = value;
      return FieldResult::Handled;
    }
    if (key == "hardware") {
      if (!field_seen(seen, 2, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      output.hardware = value;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 1U) != 0;
}

bool parse_hello(JsonReader& reader, Hello& output, bool& unsupported) {
  std::uint64_t seen = 0;
  bool valid = reader.object([&](std::string_view key) {
    if (key == "type") {
      if (!field_seen(seen, 0, reader)) return FieldResult::Error;
      ShortString type;
      if (!reader.string(type) || type.view() != "hello") return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "protocolVersion") {
      if (!field_seen(seen, 1, reader) || !read_version(reader, output.protocol_version, unsupported)) {
        return FieldResult::Error;
      }
      return FieldResult::Handled;
    }
    if (key == "deviceId") {
      if (!field_seen(seen, 2, reader) || !set_string(reader, output.device_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "supportedVersions") {
      if (!field_seen(seen, 3, reader)) return FieldResult::Error;
      std::uint64_t nested_seen = 0;
      if (!reader.object([&](std::string_view nested_key) {
            if (nested_key == "min") {
              if (!field_seen(nested_seen, 0, reader)) return FieldResult::Error;
              std::uint64_t value = 0;
              if (!reader.uint64(value) || value < 1) return FieldResult::Error;
              output.supported_versions.min = static_cast<std::uint32_t>(value);
              return FieldResult::Handled;
            }
            if (nested_key == "max") {
              if (!field_seen(nested_seen, 1, reader)) return FieldResult::Error;
              std::uint64_t value = 0;
              if (!reader.uint64(value) || value < 1) return FieldResult::Error;
              output.supported_versions.max = static_cast<std::uint32_t>(value);
              return FieldResult::Handled;
            }
            return FieldResult::Unknown;
          }) || (nested_seen & 3U) != 3U ||
          output.supported_versions.min > output.supported_versions.max) {
        return FieldResult::Error;
      }
      return FieldResult::Handled;
    }
    if (key == "lastAcceptedAdapterSequence") {
      if (!field_seen(seen, 4, reader) || !reader.uint64(output.last_accepted_adapter_sequence)) {
        return FieldResult::Error;
      }
      return FieldResult::Handled;
    }
    if (key == "pendingCommandIds") {
      if (!field_seen(seen, 5, reader)) return FieldResult::Error;
      if (!reader.array([&](std::size_t) {
            Id command_id;
            if (!set_string(reader, command_id)) return false;
            for (std::size_t index = 0; index < output.pending_command_ids.count; ++index) {
              if (output.pending_command_ids[index] == command_id) {
                reader.semantic(DecodeError::Duplicate, "duplicate pending command id");
                return false;
              }
            }
            return output.pending_command_ids.push(command_id);
          }, kMaxPendingCommands)) {
        return FieldResult::Error;
      }
      output.pending_command_count = output.pending_command_ids.count;
      return FieldResult::Handled;
    }
    if (key == "firmware") {
      if (!field_seen(seen, 6, reader) || !parse_firmware(reader, output.firmware)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "capabilities") {
      if (!field_seen(seen, 7, reader) || !parse_capabilities(reader, output.capabilities)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 0xffU) == 0xffU;
}

bool parse_question_option(JsonReader& reader, QuestionOption& output) {
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "label") {
      if (!field_seen(seen, 0, reader) || !set_string(reader, output.label)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "description") {
      if (!field_seen(seen, 1, reader) || !set_string(reader, output.description)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 3U) == 3U;
}

bool parse_question(JsonReader& reader, UserInputQuestion& output) {
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "id") {
      if (!field_seen(seen, 0, reader) || !set_string(reader, output.id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "header") {
      if (!field_seen(seen, 1, reader) || !set_string(reader, output.header)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "question") {
      if (!field_seen(seen, 2, reader) || !set_string(reader, output.question)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "options") {
      if (!field_seen(seen, 3, reader)) return FieldResult::Error;
      if (!reader.array([&](std::size_t) {
            QuestionOption option;
            return parse_question_option(reader, option) && output.options.push(option);
          }, kMaxQuestionOptions)) {
        return FieldResult::Error;
      }
      return FieldResult::Handled;
    }
    if (key == "multiSelect") {
      if (!field_seen(seen, 4, reader) || !reader.boolean(output.multi_select)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 0x0fU) == 0x0fU;
}

bool parse_pending_request(JsonReader& reader, PendingRequest& output) {
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "environmentId") {
      if (!field_seen(seen, 0, reader) || !set_string(reader, output.environment_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "projectId") {
      if (!field_seen(seen, 1, reader) || !set_string(reader, output.project_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "threadId") {
      if (!field_seen(seen, 2, reader) || !set_string(reader, output.thread_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "expectedTurnId") {
      if (!field_seen(seen, 3, reader) || !set_string(reader, output.expected_turn_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "requestId") {
      if (!field_seen(seen, 4, reader) || !set_string(reader, output.request_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "createdAt") {
      if (!field_seen(seen, 5, reader) || !read_iso_date(reader, output.created_at)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "provider") {
      if (!field_seen(seen, 6, reader) || !read_provider(reader, output.provider)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "kind") {
      if (!field_seen(seen, 7, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      if (value.view() == "approval") output.kind = PendingRequestKind::Approval;
      else if (value.view() == "user_input") output.kind = PendingRequestKind::UserInput;
      else return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "requestKind") {
      if (!field_seen(seen, 8, reader) || !set_string(reader, output.request_kind)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "detail") {
      if (!field_seen(seen, 9, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      output.detail = value;
      return FieldResult::Handled;
    }
    if (key == "availableDecisions") {
      if (!field_seen(seen, 10, reader)) return FieldResult::Error;
      if (!reader.array([&](std::size_t) {
            ApprovalDecision decision;
            return read_decision(reader, decision) && output.available_decisions.push(decision);
          }, 3)) {
        return FieldResult::Error;
      }
      return FieldResult::Handled;
    }
    if (key == "questions") {
      if (!field_seen(seen, 11, reader)) return FieldResult::Error;
      if (!reader.array([&](std::size_t) {
            UserInputQuestion question;
            return parse_question(reader, question) && output.questions.push(question);
          }, kMaxQuestions)) {
        return FieldResult::Error;
      }
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  if (!valid || (seen & 0xffU) != 0xffU) return false;
  if (output.kind == PendingRequestKind::Approval) {
    if ((seen & (1ULL << 10)) != 0 && (seen & (1ULL << 11)) == 0) return true;
    reader.semantic(DecodeError::InvalidValue, "approval request fields are inconsistent");
    return false;
  }
  output.question_count = output.questions.count;
  if ((seen & (1ULL << 11)) != 0 && (seen & (1ULL << 8)) == 0 &&
      (seen & (1ULL << 9)) == 0 && (seen & (1ULL << 10)) == 0) return true;
  reader.semantic(DecodeError::InvalidValue, "user-input request fields are inconsistent");
  return false;
}

bool parse_rate_window(JsonReader& reader, RateWindow& output) {
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "period") {
      if (!field_seen(seen, 0, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      if (value.view() == "five_hour") output.period = RateWindow::Period::FiveHour;
      else if (value.view() == "weekly") output.period = RateWindow::Period::Weekly;
      else return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "usedPercent") {
      if (!field_seen(seen, 1, reader) || !reader.number(output.used_percent) ||
          output.used_percent < 0 || output.used_percent > 100) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "resetAt") {
      if (!field_seen(seen, 2, reader) || !read_iso_date(reader, output.reset_at)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 7U) == 7U;
}

bool parse_usage_provider(JsonReader& reader, UsageProvider& output) {
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "provider") {
      if (!field_seen(seen, 0, reader) || !read_provider(reader, output.provider)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "status") {
      if (!field_seen(seen, 1, reader) || !read_usage_status(reader, output.status)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "windows") {
      if (!field_seen(seen, 2, reader)) return FieldResult::Error;
      if (!reader.array([&](std::size_t) {
            RateWindow window;
            return parse_rate_window(reader, window) && output.windows.push(window);
          }, kMaxUsageWindows)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "observedAt") {
      if (!field_seen(seen, 3, reader) || !read_iso_date(reader, output.observed_at)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "error") {
      if (!field_seen(seen, 4, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      output.error = value;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 0x0fU) == 0x0fU;
}

bool parse_usage(JsonReader& reader, UsageSnapshot& output) {
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "observedAt") {
      if (!field_seen(seen, 0, reader) || !read_iso_date(reader, output.observed_at)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "providers") {
      if (!field_seen(seen, 1, reader)) return FieldResult::Error;
      if (!reader.array([&](std::size_t) {
            UsageProvider provider;
            return parse_usage_provider(reader, provider) && output.providers.push(provider);
          }, kMaxUsageProviders)) return FieldResult::Error;
      output.provider_count = output.providers.count;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 3U) == 3U;
}

bool parse_connectivity(JsonReader& reader, Connectivity& output) {
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "status") {
      if (!field_seen(seen, 0, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      if (value.view() == "connected") output.status = ConnectivityStatus::Connected;
      else if (value.view() == "reconnecting") output.status = ConnectivityStatus::Reconnecting;
      else if (value.view() == "offline") output.status = ConnectivityStatus::Offline;
      else return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "lastConnectedAt") {
      if (!field_seen(seen, 1, reader)) return FieldResult::Error;
      DateTime value;
      if (!read_iso_date(reader, value)) return FieldResult::Error;
      output.last_connected_at = value;
      return FieldResult::Handled;
    }
    if (key == "lastSeenAt") {
      if (!field_seen(seen, 2, reader)) return FieldResult::Error;
      DateTime value;
      if (!read_iso_date(reader, value)) return FieldResult::Error;
      output.last_seen_at = value;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 1U) != 0;
}

bool parse_freshness(JsonReader& reader, Freshness& output) {
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "observedAt") {
      if (!field_seen(seen, 0, reader) || !read_iso_date(reader, output.observed_at)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "isStale") {
      if (!field_seen(seen, 1, reader) || !reader.boolean(output.is_stale)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 3U) == 3U;
}

bool parse_work_item(JsonReader& reader, WorkItem& output) {
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "environmentId") {
      if (!field_seen(seen, 0, reader) || !set_string(reader, output.environment_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "projectId") {
      if (!field_seen(seen, 1, reader) || !set_string(reader, output.project_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "threadId") {
      if (!field_seen(seen, 2, reader) || !set_string(reader, output.thread_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "turnId") {
      if (!field_seen(seen, 3, reader)) return FieldResult::Error;
      Id value;
      if (!set_string(reader, value)) return FieldResult::Error;
      output.turn_id = value;
      return FieldResult::Handled;
    }
    if (key == "projectName") {
      if (!field_seen(seen, 4, reader) || !set_string(reader, output.project_name)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "threadName") {
      if (!field_seen(seen, 5, reader) || !set_string(reader, output.thread_name)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "provider") {
      if (!field_seen(seen, 6, reader)) return FieldResult::Error;
      Provider value;
      if (!read_provider(reader, value)) return FieldResult::Error;
      output.provider = value;
      return FieldResult::Handled;
    }
    if (key == "model") {
      if (!field_seen(seen, 7, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      output.model = value;
      return FieldResult::Handled;
    }
    if (key == "state") {
      if (!field_seen(seen, 8, reader) || !read_work_state(reader, output.state)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "activity") {
      if (!field_seen(seen, 9, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      output.activity = value;
      return FieldResult::Handled;
    }
    if (key == "updatedAt") {
      if (!field_seen(seen, 10, reader) || !read_iso_date(reader, output.updated_at)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "completionSummary") {
      if (!field_seen(seen, 11, reader)) return FieldResult::Error;
      LongString value;
      if (!reader.string(value)) return FieldResult::Error;
      output.completion_summary = value;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & (1ULL << 0x0a)) == (1ULL << 0x0a) &&
         (seen & (1ULL << 0x08)) != 0 && (seen & 0x37U) == 0x37U;
}

bool parse_snapshot(JsonReader& reader, Snapshot& output, bool& unsupported) {
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "type") {
      if (!field_seen(seen, 0, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value) || value.view() != "snapshot") return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "protocolVersion") {
      if (!field_seen(seen, 1, reader) || !read_version(reader, output.protocol_version, unsupported)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "adapterSequence") {
      if (!field_seen(seen, 2, reader) || !reader.uint64(output.adapter_sequence)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "workItems") {
      if (!field_seen(seen, 3, reader)) return FieldResult::Error;
      if (!reader.array([&](std::size_t) {
            WorkItem item;
            return parse_work_item(reader, item) && output.work_items.push(item);
          }, kMaxWorkItems)) return FieldResult::Error;
      output.work_item_count = output.work_items.count;
      return FieldResult::Handled;
    }
    if (key == "automaticFocusThreadId") {
      if (!field_seen(seen, 4, reader)) return FieldResult::Error;
      // No automatic focus is represented as JSON null by the live gateway;
      // keep the bounded value empty while accepting that valid optional form.
      if (reader.peek() == 'n') {
        return reader.null_value() ? FieldResult::Handled : FieldResult::Error;
      }
      Id value;
      if (!set_string(reader, value)) return FieldResult::Error;
      output.automatic_focus_thread_id = value;
      return FieldResult::Handled;
    }
    if (key == "pendingRequests") {
      if (!field_seen(seen, 5, reader)) return FieldResult::Error;
      if (!reader.array([&](std::size_t) {
            PendingRequest request;
            return parse_pending_request(reader, request) && output.pending_requests.push(request);
          }, kMaxPendingRequests)) return FieldResult::Error;
      output.pending_request_count = output.pending_requests.count;
      return FieldResult::Handled;
    }
    if (key == "usage") {
      if (!field_seen(seen, 6, reader) || !parse_usage(reader, output.usage)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "connectivity") {
      if (!field_seen(seen, 7, reader) || !parse_connectivity(reader, output.connectivity)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "freshness") {
      if (!field_seen(seen, 8, reader) || !parse_freshness(reader, output.freshness)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  // automaticFocusThreadId is optional in the contract; all other snapshot
  // fields are required for an atomic replacement.
  return valid && (seen & 0x1efU) == 0x1efU;
}

bool parse_answer_value(JsonReader& reader, AnswerValue& output) {
  const char value = reader.peek();
  if (value == '"') {
    output.kind = AnswerValue::Kind::String;
    return reader.string(output.string_value);
  }
  if (value == 't' || value == 'f') {
    output.kind = AnswerValue::Kind::Boolean;
    return reader.boolean(output.boolean_value);
  }
  if (value == '[') {
    output.kind = AnswerValue::Kind::Strings;
    return reader.array([&](std::size_t) {
      LongString item;
      return reader.string(item) && output.string_values.push(item);
    }, kMaxAnswerSelections);
  }
  output.kind = AnswerValue::Kind::Number;
  return reader.number(output.number_value);
}

bool parse_answers(JsonReader& reader, AnswerRecord& output) {
  return reader.object([&](std::string_view key) {
    // The callback receives a view backed by the reader's bounded key; copy it
    // before parsing the value so no untrusted key is retained.
    AnswerEntry entry;
    if (!entry.key.assign(key)) return FieldResult::Error;
    for (std::size_t index = 0; index < output.entries.count; ++index) {
      if (output.entries[index].key == entry.key) return FieldResult::Error;
    }
    if (!parse_answer_value(reader, entry.value) || !output.entries.push(entry)) return FieldResult::Error;
    return FieldResult::Handled;
  });
}

bool command_type_from_string(std::string_view type, DeviceCommandType& output) {
  if (type == "answer_prompt") output = DeviceCommandType::AnswerPrompt;
  else if (type == "cancel_turn") output = DeviceCommandType::CancelTurn;
  else if (type == "stt_start") output = DeviceCommandType::SttStart;
  else if (type == "stt_stop") output = DeviceCommandType::SttStop;
  else if (type == "submit_transcript") output = DeviceCommandType::SubmitTranscript;
  else if (type == "discard_transcript") output = DeviceCommandType::DiscardTranscript;
  else if (type == "provisioning_status") output = DeviceCommandType::ProvisioningStatus;
  else return false;
  return true;
}

bool parse_device_command(JsonReader& reader, DeviceCommand& output, bool& unsupported) {
  std::uint64_t seen = 0;
  bool valid = reader.object([&](std::string_view key) {
    if (key == "type") {
      if (!field_seen(seen, 0, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      if (!command_type_from_string(value.view(), output.type)) {
        reader.semantic(DecodeError::UnknownType, "unknown device command type");
        return FieldResult::Error;
      }
      return FieldResult::Handled;
    }
    if (key == "protocolVersion") {
      if (!field_seen(seen, 1, reader) || !read_version(reader, output.protocol_version, unsupported)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "commandId") {
      if (!field_seen(seen, 2, reader) || !set_string(reader, output.command_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "deviceId") {
      if (!field_seen(seen, 3, reader) || !set_string(reader, output.device_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "environmentId") {
      if (!field_seen(seen, 4, reader) || !set_string(reader, output.context.environment_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "projectId") {
      if (!field_seen(seen, 5, reader) || !set_string(reader, output.context.project_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "threadId") {
      if (!field_seen(seen, 6, reader) || !set_string(reader, output.context.thread_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "expectedTurnId") {
      if (!field_seen(seen, 7, reader) || !set_string(reader, output.context.expected_turn_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "requestId") {
      if (!field_seen(seen, 8, reader)) return FieldResult::Error;
      Id value;
      if (!set_string(reader, value)) return FieldResult::Error;
      output.context.request_id = value;
      return FieldResult::Handled;
    }
    if (key == "decision") {
      if (!field_seen(seen, 9, reader) || !read_decision(reader, output.decision)) return FieldResult::Error;
      output.payload_kind = DeviceCommandPayload::Decision;
      return FieldResult::Handled;
    }
    if (key == "answers") {
      if (!field_seen(seen, 10, reader) || !parse_answers(reader, output.answers)) return FieldResult::Error;
      output.payload_kind = DeviceCommandPayload::Answers;
      return FieldResult::Handled;
    }
    if (key == "sttStreamId") {
      if (!field_seen(seen, 11, reader) || !set_string(reader, output.stt_stream_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "transcriptId") {
      if (!field_seen(seen, 12, reader) || !set_string(reader, output.transcript_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "transcript") {
      if (!field_seen(seen, 13, reader) || !set_string(reader, output.transcript)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "provisioningRequestId") {
      if (!field_seen(seen, 14, reader) || !set_string(reader, output.provisioning_request_id)) return FieldResult::Error;
      output.payload_kind = DeviceCommandPayload::Provisioning;
      return FieldResult::Handled;
    }
    if (key == "status") {
      if (!field_seen(seen, 15, reader) || !read_provisioning_status(reader, output.provisioning_status)) return FieldResult::Error;
      output.payload_kind = DeviceCommandPayload::Provisioning;
      return FieldResult::Handled;
    }
    if (key == "error") {
      if (!field_seen(seen, 16, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      output.error = value;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  if (!valid || (seen & 0x0fU) != 0x0fU) return false;
  if (output.type == DeviceCommandType::ProvisioningStatus) {
    if ((seen & (1ULL << 14)) != 0 && (seen & (1ULL << 15)) != 0 &&
        (seen & (1ULL << 4)) == 0 && (seen & (1ULL << 7)) == 0) return true;
    reader.semantic(DecodeError::InvalidValue, "provisioning command fields are inconsistent");
    return false;
  }
  if ((seen & 0xf0U) != 0xf0U) {
    reader.semantic(DecodeError::MissingField, "command target context is incomplete");
    return false;
  }
  switch (output.type) {
    case DeviceCommandType::AnswerPrompt:
      if ((seen & (1ULL << 8)) != 0 && ((seen & (1ULL << 9)) != 0) != ((seen & (1ULL << 10)) != 0)) return true;
      reader.semantic(DecodeError::InvalidValue, "answer_prompt requires exactly one payload");
      return false;
    case DeviceCommandType::SttStart:
    case DeviceCommandType::SttStop:
      if ((seen & (1ULL << 11)) != 0) return true;
      reader.semantic(DecodeError::MissingField, "stt command requires stream id");
      return false;
    case DeviceCommandType::SubmitTranscript:
      if ((seen & ((1ULL << 11) | (1ULL << 12) | (1ULL << 13))) ==
          ((1ULL << 11) | (1ULL << 12) | (1ULL << 13))) return true;
      reader.semantic(DecodeError::MissingField, "submit_transcript fields are incomplete");
      return false;
    case DeviceCommandType::DiscardTranscript:
      if ((seen & ((1ULL << 11) | (1ULL << 12))) == ((1ULL << 11) | (1ULL << 12))) return true;
      reader.semantic(DecodeError::MissingField, "discard_transcript fields are incomplete");
      return false;
    case DeviceCommandType::CancelTurn:
      return true;
    case DeviceCommandType::ProvisioningStatus:
      return false;
  }
  return false;
}

bool parse_command_status(JsonReader& reader, CommandStatusRecord& output) {
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "commandId") {
      if (!field_seen(seen, 0, reader) || !set_string(reader, output.command_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "status") {
      if (!field_seen(seen, 1, reader) || !read_status(reader, output.status)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "updatedAt") {
      if (!field_seen(seen, 2, reader) || !read_iso_date(reader, output.updated_at)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "adapterSequence") {
      if (!field_seen(seen, 3, reader)) return FieldResult::Error;
      std::uint64_t value = 0;
      if (!reader.uint64(value)) return FieldResult::Error;
      output.adapter_sequence = value;
      return FieldResult::Handled;
    }
    if (key == "error") {
      if (!field_seen(seen, 4, reader)) return FieldResult::Error;
      ShortString value;
      if (!reader.string(value)) return FieldResult::Error;
      output.error = value;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 7U) == 7U;
}

bool parse_server_event(JsonReader& reader, ServerEvent& output, bool& unsupported) {
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "type") {
      if (!field_seen(seen, 0, reader) || !reader.string(output.type)) return FieldResult::Error;
      constexpr std::array<std::string_view, 13> known = {
          "snapshot", "work_item_upsert", "work_item_remove", "focus_changed", "prompt_opened",
          "prompt_closed", "usage_updated", "provisioning_requested", "command_progress",
          "command_ack", "command_error", "stt_ready", "stt_final"};
      if (std::find(known.begin(), known.end(), output.type.view()) == known.end() &&
          output.type.view() != "stt_error") {
        reader.semantic(DecodeError::UnknownType, "unknown server event type");
        return FieldResult::Error;
      }
      return FieldResult::Handled;
    }
    if (key == "protocolVersion") {
      if (!field_seen(seen, 1, reader) || !read_version(reader, output.protocol_version, unsupported)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "sessionId") {
      if (!field_seen(seen, 2, reader) || !set_string(reader, output.session_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "adapterSequence") {
      if (!field_seen(seen, 3, reader) || !reader.uint64(output.adapter_sequence)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "eventId") {
      if (!field_seen(seen, 4, reader) || !set_string(reader, output.event_id)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "deviceId") {
      if (!field_seen(seen, 5, reader)) return FieldResult::Error;
      Id value;
      if (!set_string(reader, value)) return FieldResult::Error;
      output.device_id = value;
      return FieldResult::Handled;
    }
    if (key == "payload") {
      if (!field_seen(seen, 6, reader)) return FieldResult::Error;
      if (output.type.view() == "snapshot") {
        Snapshot snapshot;
        if (!parse_snapshot(reader, snapshot, unsupported)) return FieldResult::Error;
        output.snapshot = std::move(snapshot);
      } else if (!reader.skip_value(0)) {
        return FieldResult::Error;
      }
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  return valid && (seen & 0x57U) == 0x57U;
}

bool extract_type_and_version(std::string_view json, ShortString& type, std::uint64_t& version,
                              DecodeFailure& failure_out) {
  JsonReader reader(json);
  std::uint64_t seen = 0;
  const bool valid = reader.object([&](std::string_view key) {
    if (key == "type") {
      if (!field_seen(seen, 0, reader) || !reader.string(type)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    if (key == "protocolVersion") {
      if (!field_seen(seen, 1, reader) || !reader.uint64(version)) return FieldResult::Error;
      return FieldResult::Handled;
    }
    return FieldResult::Unknown;
  });
  if (!valid || !reader.complete()) {
    failure_out = reader.error();
    return false;
  }
  if ((seen & 3U) != 3U) {
    failure_out = {DecodeError::MissingField, reader.position(), "type and protocolVersion are required"};
    return false;
  }
  return true;
}

template <typename T, typename Parser>
DecodeResult<T> decode_object(std::string_view json, Parser&& parser) {
  if (json.size() > kMaxJsonBytes) {
    return failure<T>({DecodeError::LimitExceeded, kMaxJsonBytes, "JSON frame exceeds maximum bytes"});
  }
  JsonReader reader(json);
  T value;
  bool unsupported = false;
  if (!parser(reader, value, unsupported)) {
    if (unsupported) return failure<T>({DecodeError::UnsupportedVersion, reader.position(), "unsupported protocol version"});
    if (!reader.ok()) return failure<T>(reader.error());
    return failure<T>({DecodeError::MissingField, reader.position(), "required protocol field missing"});
  }
  auto result = finish(reader, std::move(value));
  if (unsupported) result.error = {DecodeError::UnsupportedVersion, reader.position(), "unsupported protocol version"};
  return result;
}

}  // namespace

DecodeResult<Hello> decode_hello(std::string_view json) {
  return decode_object<Hello>(json, [](JsonReader& reader, Hello& value, bool& unsupported) {
    return parse_hello(reader, value, unsupported);
  });
}

DecodeResult<Snapshot> decode_snapshot(std::string_view json) {
  return decode_object<Snapshot>(json, [](JsonReader& reader, Snapshot& value, bool& unsupported) {
    return parse_snapshot(reader, value, unsupported);
  });
}

DecodeResult<BoundedArray<PendingRequest, kMaxPendingRequests>>
decode_pending_requests(std::string_view json) {
  if (json.size() > kMaxJsonBytes) {
    return failure<BoundedArray<PendingRequest, kMaxPendingRequests>>(
        {DecodeError::LimitExceeded, kMaxJsonBytes, "JSON frame exceeds maximum bytes"});
  }
  JsonReader reader(json);
  BoundedArray<PendingRequest, kMaxPendingRequests> value;
  if (!reader.array([&](std::size_t) {
        PendingRequest request;
        return parse_pending_request(reader, request) && value.push(request);
      }, kMaxPendingRequests)) {
    return failure<BoundedArray<PendingRequest, kMaxPendingRequests>>(reader.error());
  }
  return finish(reader, std::move(value));
}

DecodeResult<CommandStatuses> decode_command_statuses(std::string_view json) {
  if (json.size() > kMaxJsonBytes) {
    return failure<CommandStatuses>({DecodeError::LimitExceeded, kMaxJsonBytes, "JSON frame exceeds maximum bytes"});
  }
  JsonReader reader(json);
  CommandStatuses value;
  if (!reader.array([&](std::size_t) {
        CommandStatusRecord record;
        if (!parse_command_status(reader, record)) return false;
        for (std::size_t index = 0; index < value.records.count; ++index) {
          if (value.records[index].command_id == record.command_id) {
            reader.semantic(DecodeError::Duplicate, "duplicate command status");
            return false;
          }
        }
        return value.records.push(record);
      }, kMaxPendingCommands)) {
    DecodeFailure error = reader.error();
    return failure<CommandStatuses>(error);
  }
  return finish(reader, std::move(value));
}

DecodeResult<DeviceCommand> decode_device_command(std::string_view json) {
  return decode_object<DeviceCommand>(json, [](JsonReader& reader, DeviceCommand& value, bool& unsupported) {
    return parse_device_command(reader, value, unsupported);
  });
}

DecodeResult<ServerEvent> decode_server_event(std::string_view json) {
  return decode_object<ServerEvent>(json, [](JsonReader& reader, ServerEvent& value, bool& unsupported) {
    return parse_server_event(reader, value, unsupported);
  });
}

DecodeResult<HelloAck> decode_hello_ack(std::string_view json) {
  return decode_object<HelloAck>(json, [](JsonReader& reader, HelloAck& value, bool& unsupported) {
    std::uint64_t seen = 0;
    bool valid = reader.object([&](std::string_view key) {
      if (key == "type") {
        if (!field_seen(seen, 0, reader)) return FieldResult::Error;
        ShortString type;
        if (!reader.string(type) || type.view() != "hello_ack") return FieldResult::Error;
        return FieldResult::Handled;
      }
      if (key == "protocolVersion") {
        if (!field_seen(seen, 1, reader) || !read_version(reader, value.protocol_version, unsupported)) return FieldResult::Error;
        return FieldResult::Handled;
      }
      if (key == "negotiatedVersion") {
        if (!field_seen(seen, 2, reader) || !read_version(reader, value.negotiated_version, unsupported)) return FieldResult::Error;
        return FieldResult::Handled;
      }
      if (key == "sessionId") {
        if (!field_seen(seen, 3, reader) || !set_string(reader, value.session_id)) return FieldResult::Error;
        return FieldResult::Handled;
      }
      if (key == "pendingCommands") {
        if (!field_seen(seen, 4, reader)) return FieldResult::Error;
        if (!reader.array([&](std::size_t) {
              CommandStatusRecord record;
              if (!parse_command_status(reader, record)) return false;
              for (std::size_t index = 0; index < value.pending_commands.records.count; ++index) {
                if (value.pending_commands.records[index].command_id == record.command_id) {
                  reader.semantic(DecodeError::Duplicate, "duplicate pending command status");
                  return false;
                }
              }
              return value.pending_commands.records.push(record);
            }, kMaxPendingCommands)) return FieldResult::Error;
        return FieldResult::Handled;
      }
      if (key == "resume") {
        if (!field_seen(seen, 5, reader)) return FieldResult::Error;
        std::uint64_t resume_seen = 0;
        if (!reader.object([&](std::string_view resume_key) {
              if (resume_key == "mode") {
                if (!field_seen(resume_seen, 0, reader)) return FieldResult::Error;
                ShortString mode;
                if (!reader.string(mode)) return FieldResult::Error;
                if (mode.view() == "replay") value.resume_replay = true;
                else if (mode.view() == "snapshot") value.resume_replay = false;
                else return FieldResult::Error;
                return FieldResult::Handled;
              }
              if (resume_key == "fromAdapterSequence") {
                if (!field_seen(resume_seen, 1, reader) || !reader.uint64(value.resume_from_adapter_sequence)) return FieldResult::Error;
                return FieldResult::Handled;
              }
              return FieldResult::Unknown;
            }) || (resume_seen & 1U) == 0 || (value.resume_replay && (resume_seen & 2U) == 0) ||
            (!value.resume_replay && (resume_seen & 2U) != 0)) return FieldResult::Error;
        return FieldResult::Handled;
      }
      return FieldResult::Unknown;
    });
    return valid && (seen & 0x3fU) == 0x3fU;
  });
}

DecodeResult<Envelope> decode_envelope(std::string_view json) {
  if (json.size() > kMaxJsonBytes) {
    return failure<Envelope>({DecodeError::LimitExceeded, kMaxJsonBytes, "JSON frame exceeds maximum bytes"});
  }
  ShortString type;
  std::uint64_t version = 0;
  DecodeFailure extraction_error;
  if (!extract_type_and_version(json, type, version, extraction_error)) {
    return failure<Envelope>(extraction_error);
  }
  if (version != kProtocolVersion) {
    return failure<Envelope>({DecodeError::UnsupportedVersion, 0, "unsupported protocol version"});
  }
  Envelope envelope;
  if (type.view() == "hello") {
    const auto value = decode_hello(json);
    if (!value.ok()) return failure<Envelope>(value.error);
    envelope.type = Envelope::Type::Hello;
    envelope.payload = value.value;
  } else if (type.view() == "hello_ack") {
    const auto value = decode_hello_ack(json);
    if (!value.ok()) return failure<Envelope>(value.error);
    envelope.type = Envelope::Type::HelloAck;
    envelope.payload = value.value;
  } else if (type.view() == "snapshot") {
    const auto value = decode_snapshot(json);
    if (value.ok()) {
      envelope.type = Envelope::Type::Snapshot;
      envelope.payload = value.value;
    } else {
      // Live WebSocket replay wraps a replacement snapshot in the server
      // event envelope (`payload`), while the dedicated HTTP route returns
      // the snapshot object directly. Accept both wire forms without
      // weakening the strict bounded decoders.
      const auto event = decode_server_event(json);
      if (!event.ok()) return failure<Envelope>(value.error);
      envelope.type = Envelope::Type::ServerEvent;
      envelope.payload = event.value;
    }
  } else if (type.view() == "cancel_turn" || type.view() == "answer_prompt" ||
             type.view() == "stt_start" || type.view() == "stt_stop" ||
             type.view() == "submit_transcript" || type.view() == "discard_transcript" ||
             type.view() == "provisioning_status") {
    const auto value = decode_device_command(json);
    if (!value.ok()) return failure<Envelope>(value.error);
    envelope.type = Envelope::Type::DeviceCommand;
    envelope.payload = value.value;
  } else {
    constexpr std::array<std::string_view, 14> server_types = {
        "work_item_upsert", "work_item_remove", "focus_changed", "prompt_opened", "prompt_closed",
        "usage_updated", "provisioning_requested", "command_progress", "command_ack", "command_error",
        "stt_ready", "stt_final", "stt_error", "server_event"};
    if (std::find(server_types.begin(), server_types.end(), type.view()) == server_types.end()) {
      return failure<Envelope>({DecodeError::UnknownType, 0, "unknown companion message type"});
    }
    const auto value = decode_server_event(json);
    if (!value.ok()) return failure<Envelope>(value.error);
    envelope.type = Envelope::Type::ServerEvent;
    envelope.payload = value.value;
  }
  DecodeResult<Envelope> result;
  result.value = std::move(envelope);
  return result;
}

namespace {

void append_json_string(std::string& output, std::string_view value) {
  output.push_back('"');
  for (const char character : value) {
    switch (character) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: output.push_back(character); break;
    }
  }
  output.push_back('"');
}

void append_key(std::string& output, std::string_view key, bool& first) {
  if (!first) output.push_back(',');
  first = false;
  append_json_string(output, key);
  output.push_back(':');
}

}  // namespace

DecodeResult<std::string> encode_hello(const Hello& hello) {
  DecodeResult<std::string> result;
  std::string output;
  output.reserve(1024);
  output += "{";
  bool first = true;
  append_key(output, "type", first); append_json_string(output, "hello");
  append_key(output, "protocolVersion", first); output += "1";
  append_key(output, "deviceId", first); append_json_string(output, hello.device_id.view());
  append_key(output, "supportedVersions", first);
  output += "{\"min\":" + std::to_string(hello.supported_versions.min) +
            ",\"max\":" + std::to_string(hello.supported_versions.max) + "}";
  append_key(output, "lastAcceptedAdapterSequence", first);
  output += std::to_string(hello.last_accepted_adapter_sequence);
  append_key(output, "pendingCommandIds", first); output.push_back('[');
  for (std::size_t index = 0; index < hello.pending_command_ids.count; ++index) {
    if (index != 0) output.push_back(',');
    append_json_string(output, hello.pending_command_ids[index].view());
  }
  output.push_back(']');
  append_key(output, "firmware", first); output += "{";
  bool firmware_first = true;
  append_key(output, "version", firmware_first); append_json_string(output, hello.firmware.version.view());
  if (hello.firmware.build.has_value()) {
    append_key(output, "build", firmware_first); append_json_string(output, hello.firmware.build->view());
  }
  if (hello.firmware.hardware.has_value()) {
    append_key(output, "hardware", firmware_first); append_json_string(output, hello.firmware.hardware->view());
  }
  output.push_back('}');
  append_key(output, "capabilities", first); output += "{";
  bool capability_first = true;
  if (hello.capabilities.audio.has_value()) {
    append_key(output, "audio", capability_first); output += "{";
    bool audio_first = true;
    append_key(output, "codec", audio_first);
    append_json_string(output, hello.capabilities.audio->codec == AudioCodec::Opus ? "opus" : "pcm_s16le");
    append_key(output, "sampleRateHz", audio_first); output += std::to_string(hello.capabilities.audio->sample_rate_hz);
    append_key(output, "channels", audio_first); output += std::to_string(hello.capabilities.audio->channels);
    append_key(output, "maxChunkBytes", audio_first); output += std::to_string(hello.capabilities.audio->max_chunk_bytes);
    output.push_back('}');
  }
  append_key(output, "maxMessageBytes", capability_first); output += std::to_string(hello.capabilities.max_message_bytes);
  append_key(output, "maxPendingCommands", capability_first); output += std::to_string(hello.capabilities.max_pending_commands);
  append_key(output, "maxPendingRequests", capability_first); output += std::to_string(hello.capabilities.max_pending_requests);
  output.push_back('}');
  output.push_back('}');
  if (output.size() > kMaxJsonBytes) {
    result.error = {DecodeError::LimitExceeded, kMaxJsonBytes, "encoded frame exceeds maximum bytes"};
    return result;
  }
  result.value = std::move(output);
  return result;
}

bool ReplayGuard::accept(std::uint64_t adapter_sequence) {
  if (initialized_ && adapter_sequence <= last_sequence_) {
    return false;
  }
  initialized_ = true;
  last_sequence_ = adapter_sequence;
  return true;
}

}  // namespace t3::companion
