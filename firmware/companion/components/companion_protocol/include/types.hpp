#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace t3::companion {

// These limits mirror the reviewed T3 contract.  Identifier and timestamp
// fields are additionally bounded here because the wire decoder must never
// allocate storage proportional to an untrusted JSON string.
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxJsonBytes = 1U << 20;
inline constexpr std::size_t kMaxJsonDepth = 16;
inline constexpr std::size_t kMaxIdBytes = 256;
inline constexpr std::size_t kMaxShortStringBytes = 256;
inline constexpr std::size_t kMaxLongStringBytes = 8'192;
inline constexpr std::size_t kMaxQuestionBytes = 2'048;
inline constexpr std::size_t kMaxAnswerKeyBytes = 128;
inline constexpr std::size_t kMaxDateTimeBytes = 128;
inline constexpr std::size_t kMaxPendingCommands = 128;
inline constexpr std::size_t kMaxPendingRequests = 64;
inline constexpr std::size_t kMaxWorkItems = 64;
inline constexpr std::size_t kMaxQuestions = 16;
inline constexpr std::size_t kMaxQuestionOptions = 32;
inline constexpr std::size_t kMaxAnswers = 16;
inline constexpr std::size_t kMaxAnswerSelections = 32;
inline constexpr std::size_t kMaxUsageProviders = 3;
inline constexpr std::size_t kMaxUsageWindows = 2;

template <std::size_t MaxBytes>
struct BoundedString {
  // Ownership is bounded at the public seam: assignment rejects input over
  // MaxBytes before allocating, so this is not an unbounded JSON tree.
  std::string data;

  [[nodiscard]] bool assign(std::string_view input, bool require_non_empty = true) {
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t' ||
                              input.front() == '\n' || input.front() == '\r')) {
      input.remove_prefix(1);
    }
    while (!input.empty() && (input.back() == ' ' || input.back() == '\t' ||
                              input.back() == '\n' || input.back() == '\r')) {
      input.remove_suffix(1);
    }
    if (input.size() > MaxBytes || (require_non_empty && input.empty())) {
      return false;
    }
    data.assign(input.data(), input.size());
    return true;
  }

  [[nodiscard]] std::string_view view() const { return data; }
  [[nodiscard]] bool empty() const { return data.empty(); }

  friend bool operator==(const BoundedString& left, const BoundedString& right) {
    return left.view() == right.view();
  }
  friend bool operator!=(const BoundedString& left, const BoundedString& right) {
    return !(left == right);
  }
};

using Id = BoundedString<kMaxIdBytes>;
using ShortString = BoundedString<kMaxShortStringBytes>;
using LongString = BoundedString<kMaxLongStringBytes>;
using QuestionString = BoundedString<kMaxQuestionBytes>;
using AnswerKey = BoundedString<kMaxAnswerKeyBytes>;
using DateTime = BoundedString<kMaxDateTimeBytes>;

template <typename T, std::size_t MaxItems>
struct BoundedArray {
  // The vector is a bounded owner: push() refuses the (compile-time) contract
  // maximum before any further allocation.  This avoids reserving the full
  // cross-product of nested protocol maxima in every snapshot value.
  std::vector<T> items;
  std::size_t count = 0;

  [[nodiscard]] bool push(const T& value) {
    if (count >= MaxItems) {
      return false;
    }
    items.push_back(value);
    ++count;
    return true;
  }
  [[nodiscard]] const T& operator[](std::size_t index) const { return items[index]; }
  [[nodiscard]] T& operator[](std::size_t index) { return items[index]; }
  [[nodiscard]] auto begin() const { return items.begin(); }
  [[nodiscard]] auto end() const { return items.begin() + static_cast<std::ptrdiff_t>(count); }
};

enum class Provider { Codex, Claude, Xai };
enum class WorkItemState { NeedsInput, Failed, Blocked, Working, Completed, Idle };
enum class PendingRequestKind { Approval, UserInput };
enum class ApprovalDecision { Accept, Decline, Cancel };
enum class CommandStatus { Received, Committed, Applied, Rejected };
enum class UsageStatus { Available, Stale, Unavailable, Error };
enum class ConnectivityStatus { Connected, Reconnecting, Offline };
enum class AudioCodec { PcmS16le, Opus };
enum class ProvisioningStatus { Started, Completed, Cancelled, Error };

struct VersionRange {
  std::uint32_t min = kProtocolVersion;
  std::uint32_t max = kProtocolVersion;
};

struct AudioCapability {
  AudioCodec codec = AudioCodec::PcmS16le;
  std::uint32_t sample_rate_hz = 16'000;
  std::uint32_t channels = 1;
  std::uint32_t max_chunk_bytes = 4'096;
};

struct Capabilities {
  std::optional<AudioCapability> audio;
  std::uint32_t max_message_bytes = 65'536;
  std::uint32_t max_pending_commands = 32;
  std::uint32_t max_pending_requests = 16;
};

struct FirmwareInfo {
  ShortString version;
  std::optional<ShortString> build;
  std::optional<ShortString> hardware;
};

struct Hello {
  std::uint8_t protocol_version = kProtocolVersion;
  Id device_id;
  VersionRange supported_versions;
  std::uint64_t last_accepted_adapter_sequence = 0;
  BoundedArray<Id, kMaxPendingCommands> pending_command_ids;
  FirmwareInfo firmware;
  Capabilities capabilities;

  std::size_t pending_command_count = 0;
};

struct CommandStatusRecord {
  Id command_id;
  CommandStatus status = CommandStatus::Received;
  DateTime updated_at;
  std::optional<std::uint64_t> adapter_sequence;
  std::optional<ShortString> error;
};

struct CommandStatuses {
  BoundedArray<CommandStatusRecord, kMaxPendingCommands> records;
  [[nodiscard]] std::size_t count() const { return records.count; }
};

struct HelloAck {
  std::uint8_t protocol_version = kProtocolVersion;
  std::uint8_t negotiated_version = kProtocolVersion;
  Id session_id;
  CommandStatuses pending_commands;
  bool resume_replay = false;
  std::uint64_t resume_from_adapter_sequence = 0;
};

struct WorkItem {
  Id environment_id;
  Id project_id;
  Id thread_id;
  std::optional<Id> turn_id;
  ShortString project_name;
  ShortString thread_name;
  std::optional<Provider> provider;
  std::optional<ShortString> model;
  WorkItemState state = WorkItemState::Idle;
  std::optional<ShortString> activity;
  DateTime updated_at;
  std::optional<LongString> completion_summary;
};

struct QuestionOption {
  ShortString label;
  QuestionString description;
};

struct UserInputQuestion {
  ShortString id;
  ShortString header;
  QuestionString question;
  BoundedArray<QuestionOption, kMaxQuestionOptions> options;
  bool multi_select = false;
};

struct AnswerValue {
  enum class Kind { String, Boolean, Number, Strings };
  Kind kind = Kind::String;
  LongString string_value;
  bool boolean_value = false;
  double number_value = 0;
  BoundedArray<LongString, kMaxAnswerSelections> string_values;
};

struct AnswerEntry {
  AnswerKey key;
  AnswerValue value;
};

struct AnswerRecord {
  BoundedArray<AnswerEntry, kMaxAnswers> entries;
};

struct PendingRequest {
  Id environment_id;
  Id project_id;
  Id thread_id;
  Id expected_turn_id;
  Id request_id;
  DateTime created_at;
  Provider provider = Provider::Codex;
  PendingRequestKind kind = PendingRequestKind::Approval;
  ShortString request_kind;
  std::optional<ShortString> detail;
  BoundedArray<ApprovalDecision, 3> available_decisions;
  BoundedArray<UserInputQuestion, kMaxQuestions> questions;
  std::size_t question_count = 0;
};

struct RateWindow {
  enum class Period { FiveHour, Weekly };
  Period period = Period::FiveHour;
  double used_percent = 0;
  DateTime reset_at;
};

struct UsageProvider {
  Provider provider = Provider::Codex;
  UsageStatus status = UsageStatus::Available;
  BoundedArray<RateWindow, kMaxUsageWindows> windows;
  DateTime observed_at;
  std::optional<ShortString> error;
};

struct UsageSnapshot {
  DateTime observed_at;
  BoundedArray<UsageProvider, kMaxUsageProviders> providers;
  std::size_t provider_count = 0;
};

struct Connectivity {
  ConnectivityStatus status = ConnectivityStatus::Offline;
  std::optional<DateTime> last_connected_at;
  std::optional<DateTime> last_seen_at;
};

struct Freshness {
  DateTime observed_at;
  bool is_stale = false;
};

struct Snapshot {
  std::uint8_t protocol_version = kProtocolVersion;
  std::uint64_t adapter_sequence = 0;
  BoundedArray<WorkItem, kMaxWorkItems> work_items;
  std::optional<Id> automatic_focus_thread_id;
  BoundedArray<PendingRequest, kMaxPendingRequests> pending_requests;
  UsageSnapshot usage;
  Connectivity connectivity;
  Freshness freshness;
  std::size_t work_item_count = 0;
  std::size_t pending_request_count = 0;
};

struct TargetContext {
  Id environment_id;
  Id project_id;
  Id thread_id;
  Id expected_turn_id;
  std::optional<Id> request_id;
};

enum class DeviceCommandType {
  AnswerPrompt,
  CancelTurn,
  SttStart,
  SttStop,
  SubmitTranscript,
  DiscardTranscript,
  ProvisioningStatus,
};

enum class DeviceCommandPayload { None, Decision, Answers, Provisioning };

struct DeviceCommand {
  DeviceCommandType type = DeviceCommandType::CancelTurn;
  std::uint8_t protocol_version = kProtocolVersion;
  Id command_id;
  Id device_id;
  TargetContext context;
  DeviceCommandPayload payload_kind = DeviceCommandPayload::None;
  ApprovalDecision decision = ApprovalDecision::Accept;
  AnswerRecord answers;
  Id stt_stream_id;
  Id transcript_id;
  LongString transcript;
  Id provisioning_request_id;
  ProvisioningStatus provisioning_status = ProvisioningStatus::Started;
  std::optional<ShortString> error;
};

struct ServerEvent {
  ShortString type;
  std::uint8_t protocol_version = kProtocolVersion;
  Id session_id;
  std::uint64_t adapter_sequence = 0;
  Id event_id;
  std::optional<Id> device_id;
  // The gateway sends a retention-replacement snapshot as a server-event
  // envelope whose payload is the complete CompanionSnapshot. Keep that
  // bounded payload so the runtime can atomically replace its cached model.
  std::optional<Snapshot> snapshot;
};

struct Envelope {
  enum class Type {
    Hello,
    HelloAck,
    Snapshot,
    ServerEvent,
    DeviceCommand,
  } type = Type::Hello;
  std::variant<Hello, HelloAck, Snapshot, ServerEvent, DeviceCommand> payload;
};

}  // namespace t3::companion
