#pragma once

#include "command_outbox.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace t3::companion {

inline constexpr std::uint64_t kGlowDurationMs = 800;

enum class InteractionMode {
  Ambient,
  Approval,
  UserInput,
  Transcript,
  AwaitingAck,
  AppliedGlow,
  ErrorGlow,
};

enum class Gesture {
  SwipeLeft,
  SwipeRight,
  SwipeUp,
  SwipeDown,
  BrowsePrevious,
  BrowseNext,
};

struct InteractionState {
  bool live_handshake = false;
  Id device_id;
  InteractionMode mode = InteractionMode::Ambient;
  std::optional<PendingRequest> request;
  TargetContext target;
  std::size_t question_index = 0;
  std::size_t card_index = 0;
  std::vector<std::size_t> selected_option_indices;
  AnswerRecord answers;
  std::optional<LongString> transcript;
  // STT ownership is frozen at capture start.  A later snapshot/request may
  // change ambient focus, but it cannot retarget a transcript already being
  // confirmed.
  TargetContext transcript_target;
  Id transcript_stream_id;
  Id transcript_id;
  bool transcript_target_frozen = false;
  std::optional<Id> active_command_id;
  std::uint64_t glow_started_ms = 0;
  InteractionMode glow_return_mode = InteractionMode::Ambient;
  std::uint64_t next_command_number = 1;
  CommandOutbox outbox;
};

enum class InteractionEventKind {
  OpenRequest,
  SttStarted,
  Gesture,
  TranscriptProposed,
  CommandStatus,
  CommandStatuses,
  SetConnectivity,
  Tick,
  ResetAmbient,
};

struct InteractionEvent {
  InteractionEventKind kind = InteractionEventKind::Tick;
  Gesture gesture_value = Gesture::SwipeUp;
  std::uint64_t now_ms = 0;
  bool live_handshake = false;
  std::optional<PendingRequest> request;
  std::optional<Id> command_id;
  CommandStatus status = CommandStatus::Received;
  CommandStatuses statuses;
  std::optional<LongString> transcript_value;
  std::optional<TargetContext> transcript_target_value;
  std::optional<Id> stt_stream_id_value;
  std::optional<Id> transcript_id_value;

  [[nodiscard]] static InteractionEvent open(const PendingRequest& request,
                                             std::uint64_t now_ms);
  [[nodiscard]] static InteractionEvent gesture(Gesture gesture,
                                                std::uint64_t now_ms);
  [[nodiscard]] static InteractionEvent transcript(std::string_view transcript,
                                                   std::uint64_t now_ms);
  [[nodiscard]] static InteractionEvent stt_start(const Id& stream_id,
                                                  const TargetContext& target,
                                                  std::uint64_t now_ms,
                                                  const Id& transcript_id = {});
  [[nodiscard]] static InteractionEvent command_status(const Id& command_id,
                                                       CommandStatus status,
                                                       std::uint64_t now_ms);
  [[nodiscard]] static InteractionEvent connectivity(bool live_handshake,
                                                     std::uint64_t now_ms);
};

struct InteractionTransition {
  InteractionState state;
  std::vector<Effect> effects;
};

[[nodiscard]] InteractionState make_interaction_state(bool live_handshake,
                                                       std::string_view device_id = "device");
[[nodiscard]] InteractionTransition reduce_interaction(const InteractionState& current,
                                                        const InteractionEvent& event);

}  // namespace t3::companion
