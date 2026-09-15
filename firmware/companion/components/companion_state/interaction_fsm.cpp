#include "interaction_fsm.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace t3::companion {
namespace {

Id make_id(std::string_view prefix, std::uint64_t number) {
  Id result;
  const std::string value = std::string(prefix) + std::to_string(number);
  (void)result.assign(value);
  return result;
}

TargetContext target_for(const PendingRequest& request) {
  TargetContext target;
  target.environment_id = request.environment_id;
  target.project_id = request.project_id;
  target.thread_id = request.thread_id;
  target.expected_turn_id = request.expected_turn_id;
  target.request_id = request.request_id;
  return target;
}

bool decision_available(const PendingRequest& request, ApprovalDecision decision) {
  for (std::size_t index = 0; index < request.available_decisions.count; ++index) {
    if (request.available_decisions[index] == decision) {
      return true;
    }
  }
  return false;
}

bool has_target(const TargetContext& target) {
  return !target.environment_id.empty() && !target.project_id.empty() &&
         !target.thread_id.empty() && !target.expected_turn_id.empty();
}

void append_effects(std::vector<Effect>& destination, const std::vector<Effect>& source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

void offline_effect(std::vector<Effect>& effects) {
  Effect effect;
  effect.kind = EffectKind::OfflineRejected;
  effects.push_back(effect);
}

void reset_answer_cursor(InteractionState& state) {
  state.card_index = 0;
  state.selected_option_indices.clear();
}

void toggle_selected(std::vector<std::size_t>& values, std::size_t value) {
  const auto found = std::find(values.begin(), values.end(), value);
  if (found == values.end()) {
    values.push_back(value);
  } else {
    values.erase(found);
  }
}

bool add_string_answer(InteractionState& state, const UserInputQuestion& question,
                       const QuestionOption* option) {
  AnswerEntry entry;
  if (!entry.key.assign(question.id.view())) {
    return false;
  }
  entry.value.kind = AnswerValue::Kind::String;
  if (option == nullptr || !entry.value.string_value.assign(option->label.view())) {
    return false;
  }
  return state.answers.entries.push(entry);
}

bool add_multi_answer(InteractionState& state, const UserInputQuestion& question,
                      const std::vector<std::size_t>& selected_indices) {
  AnswerEntry entry;
  if (!entry.key.assign(question.id.view())) {
    return false;
  }
  entry.value.kind = AnswerValue::Kind::Strings;
  std::vector<std::size_t> ordered_indices = selected_indices;
  std::sort(ordered_indices.begin(), ordered_indices.end());
  for (const std::size_t index : ordered_indices) {
    if (index >= question.options.count) {
      return false;
    }
    LongString value;
    if (!value.assign(question.options[index].label.view()) ||
        !entry.value.string_values.push(value)) {
      return false;
    }
  }
  return state.answers.entries.push(entry);
}

DeviceCommand base_command(const InteractionState& state, DeviceCommandType type,
                           std::uint64_t& next_command_number) {
  DeviceCommand command;
  command.type = type;
  command.protocol_version = kProtocolVersion;
  command.command_id = make_id("companion-command-", next_command_number++);
  command.device_id = state.device_id;
  command.context = state.target;
  return command;
}

bool enqueue_command(InteractionState& state, DeviceCommand command,
                     std::vector<Effect>& effects) {
  if (!state.live_handshake) {
    offline_effect(effects);
    return false;
  }
  const auto transition = state.outbox.enqueue(command, state.live_handshake);
  append_effects(effects, transition.effects);
  if (!transition.accepted) {
    return false;
  }
  state.active_command_id = command.command_id;
  state.glow_return_mode = state.mode;
  state.mode = InteractionMode::AwaitingAck;
  return true;
}

void advance_question_or_submit(InteractionState& state, const PendingRequest& request,
                                std::vector<Effect>& effects) {
  if (state.question_index + 1U < request.questions.count) {
    ++state.question_index;
    reset_answer_cursor(state);
    return;
  }
  DeviceCommand command = base_command(state, DeviceCommandType::AnswerPrompt,
                                       state.next_command_number);
  command.payload_kind = DeviceCommandPayload::Answers;
  command.answers = state.answers;
  enqueue_command(state, command, effects);
}

void cancel_turn(InteractionState& state, std::vector<Effect>& effects) {
  const TargetContext& target = state.transcript_target_frozen ? state.transcript_target
                                                                : state.target;
  if (!has_target(target)) {
    return;
  }
  DeviceCommand command = base_command(state, DeviceCommandType::CancelTurn,
                                       state.next_command_number);
  command.context = target;
  enqueue_command(state, command, effects);
}

void handle_approval_gesture(InteractionState& state, Gesture gesture,
                             std::vector<Effect>& effects) {
  if (!state.request.has_value()) {
    return;
  }
  std::optional<ApprovalDecision> decision;
  if (gesture == Gesture::SwipeRight) {
    decision = ApprovalDecision::Accept;
  } else if (gesture == Gesture::SwipeLeft) {
    decision = ApprovalDecision::Decline;
  } else if (gesture == Gesture::SwipeDown) {
    decision = ApprovalDecision::Cancel;
  }
  if (!decision.has_value() || !decision_available(*state.request, *decision)) {
    return;
  }
  DeviceCommand command = base_command(state, DeviceCommandType::AnswerPrompt,
                                       state.next_command_number);
  command.payload_kind = DeviceCommandPayload::Decision;
  command.decision = *decision;
  enqueue_command(state, command, effects);
}

void handle_user_input_gesture(InteractionState& state, Gesture gesture,
                               std::vector<Effect>& effects) {
  if (!state.request.has_value() || state.question_index >= state.request->questions.count) {
    return;
  }
  const UserInputQuestion& question = state.request->questions[state.question_index];
  const bool previous = gesture == Gesture::SwipeLeft || gesture == Gesture::BrowsePrevious;
  const bool next = gesture == Gesture::SwipeRight || gesture == Gesture::BrowseNext;
  if (previous || next) {
    const std::size_t card_count = question.options.count + 1U;
    if (card_count > 1U) {
      if (next) {
        state.card_index = (state.card_index + 1U) % card_count;
      } else {
        state.card_index = state.card_index == 0U ? card_count - 1U : state.card_index - 1U;
      }
    }
    return;
  }
  if (gesture == Gesture::SwipeDown) {
    cancel_turn(state, effects);
    return;
  }
  if (gesture != Gesture::SwipeUp) {
    return;
  }

  if (question.options.count == 0U) {
    if (state.live_handshake) {
      Effect effect;
      effect.kind = EffectKind::RequestVoiceInput;
      effects.push_back(effect);
    } else {
      offline_effect(effects);
    }
    return;
  }
  if (question.multi_select) {
    if (state.card_index < question.options.count) {
      toggle_selected(state.selected_option_indices, state.card_index);
      return;
    }
    if (!add_multi_answer(state, question, state.selected_option_indices)) {
      return;
    }
    advance_question_or_submit(state, *state.request, effects);
    return;
  }
  if (state.card_index >= question.options.count ||
      !add_string_answer(state, question, &question.options[state.card_index])) {
    return;
  }
  advance_question_or_submit(state, *state.request, effects);
}

void handle_transcript_gesture(InteractionState& state, Gesture gesture,
                               std::vector<Effect>& effects) {
  if (gesture == Gesture::SwipeLeft) {
    state.transcript.reset();
    state.transcript_target = {};
    state.transcript_stream_id = {};
    state.transcript_id = {};
    state.transcript_target_frozen = false;
    state.mode = InteractionMode::Ambient;
    Effect effect;
    effect.kind = EffectKind::RestoreAmbient;
    effects.push_back(effect);
    return;
  }
  if (gesture == Gesture::SwipeDown) {
    cancel_turn(state, effects);
    state.transcript.reset();
    state.transcript_target = {};
    state.transcript_stream_id = {};
    state.transcript_id = {};
    state.transcript_target_frozen = false;
    state.mode = InteractionMode::Ambient;
    return;
  }
  if (gesture != Gesture::SwipeRight || !state.transcript.has_value()) {
    return;
  }
  DeviceCommand command = base_command(state, DeviceCommandType::SubmitTranscript,
                                       state.next_command_number);
  if (state.transcript_target_frozen) command.context = state.transcript_target;
  command.transcript = *state.transcript;
  command.stt_stream_id = state.transcript_stream_id;
  command.transcript_id = state.transcript_id.empty()
                              ? make_id("transcript-", state.next_command_number)
                              : state.transcript_id;
  enqueue_command(state, command, effects);
}

void on_command_effects(InteractionState& state, const std::vector<Effect>& effects,
                        std::uint64_t now_ms) {
  bool applied = false;
  bool rejected = false;
  for (const auto& effect : effects) {
    applied = applied || effect.kind == EffectKind::ShowAppliedGlow;
    rejected = rejected || effect.kind == EffectKind::ShowErrorGlow;
  }
  if (applied) {
    state.mode = InteractionMode::AppliedGlow;
    state.glow_started_ms = now_ms;
    state.request.reset();
    state.transcript.reset();
    state.transcript_target = {};
    state.transcript_stream_id = {};
    state.transcript_id = {};
    state.transcript_target_frozen = false;
  } else if (rejected) {
    state.mode = InteractionMode::ErrorGlow;
    state.glow_started_ms = now_ms;
  }
}

}  // namespace

InteractionEvent InteractionEvent::open(const PendingRequest& request, std::uint64_t now_ms) {
  InteractionEvent event;
  event.kind = InteractionEventKind::OpenRequest;
  event.now_ms = now_ms;
  event.request = request;
  return event;
}

InteractionEvent InteractionEvent::gesture(Gesture value, std::uint64_t now_ms) {
  InteractionEvent event;
  event.kind = InteractionEventKind::Gesture;
  event.gesture_value = value;
  event.now_ms = now_ms;
  return event;
}

InteractionEvent InteractionEvent::transcript(std::string_view value, std::uint64_t now_ms) {
  InteractionEvent event;
  event.kind = InteractionEventKind::TranscriptProposed;
  event.now_ms = now_ms;
  LongString transcript_value;
  (void)transcript_value.assign(value, false);
  event.transcript_value = transcript_value;
  return event;
}

InteractionEvent InteractionEvent::stt_start(const Id& stream_id, const TargetContext& target,
                                             std::uint64_t now_ms, const Id& transcript_id) {
  InteractionEvent event;
  event.kind = InteractionEventKind::SttStarted;
  event.now_ms = now_ms;
  event.stt_stream_id_value = stream_id;
  event.transcript_target_value = target;
  if (!transcript_id.empty()) event.transcript_id_value = transcript_id;
  return event;
}

InteractionEvent InteractionEvent::command_status(const Id& command_id, CommandStatus status,
                                                  std::uint64_t now_ms) {
  InteractionEvent event;
  event.kind = InteractionEventKind::CommandStatus;
  event.now_ms = now_ms;
  event.command_id = command_id;
  event.status = status;
  return event;
}

InteractionEvent InteractionEvent::connectivity(bool live_handshake, std::uint64_t now_ms) {
  InteractionEvent event;
  event.kind = InteractionEventKind::SetConnectivity;
  event.now_ms = now_ms;
  event.live_handshake = live_handshake;
  return event;
}

InteractionState make_interaction_state(bool live_handshake, std::string_view device_id) {
  InteractionState state;
  state.live_handshake = live_handshake;
  (void)state.device_id.assign(device_id);
  return state;
}

InteractionTransition reduce_interaction(const InteractionState& current,
                                         const InteractionEvent& event) {
  InteractionTransition transition;
  transition.state = current;
  InteractionState& state = transition.state;

  switch (event.kind) {
    case InteractionEventKind::OpenRequest:
      if (!event.request.has_value()) {
        break;
      }
      state.request = event.request;
      state.target = target_for(*event.request);
      state.question_index = 0;
      reset_answer_cursor(state);
      state.answers = AnswerRecord{};
      state.transcript.reset();
      state.active_command_id.reset();
      // A live STT session owns its original target even if an ambient update
      // or a new request arrives while the server is transcribing.
      if (!state.transcript_target_frozen) {
        state.transcript_target = {};
        state.transcript_stream_id = {};
        state.transcript_id = {};
      }
      state.mode = event.request->kind == PendingRequestKind::Approval
                       ? InteractionMode::Approval
                       : InteractionMode::UserInput;
      break;
    case InteractionEventKind::SttStarted:
      if (event.stt_stream_id_value.has_value() && event.transcript_target_value.has_value()) {
        state.transcript_stream_id = *event.stt_stream_id_value;
        state.transcript_target = *event.transcript_target_value;
        state.transcript_target_frozen = has_target(state.transcript_target);
        state.transcript.reset();
        state.transcript_id = event.transcript_id_value.value_or(Id{});
      }
      break;
    case InteractionEventKind::Gesture:
      switch (state.mode) {
        case InteractionMode::Approval:
          handle_approval_gesture(state, event.gesture_value, transition.effects);
          break;
        case InteractionMode::UserInput:
          handle_user_input_gesture(state, event.gesture_value, transition.effects);
          break;
        case InteractionMode::Transcript:
          handle_transcript_gesture(state, event.gesture_value, transition.effects);
          break;
        case InteractionMode::ErrorGlow:
        case InteractionMode::Ambient:
        case InteractionMode::AwaitingAck:
        case InteractionMode::AppliedGlow:
          break;
      }
      break;
    case InteractionEventKind::TranscriptProposed:
      if (event.transcript_value.has_value()) {
        if (!state.transcript_target_frozen && event.transcript_target_value.has_value() &&
            has_target(*event.transcript_target_value)) {
          state.transcript_target = *event.transcript_target_value;
          state.transcript_target_frozen = true;
        }
        if (event.stt_stream_id_value.has_value()) {
          state.transcript_stream_id = *event.stt_stream_id_value;
        }
        if (event.transcript_id_value.has_value()) {
          state.transcript_id = *event.transcript_id_value;
        }
        state.transcript = event.transcript_value;
        state.mode = InteractionMode::Transcript;
      }
      break;
    case InteractionEventKind::CommandStatus: {
      const Id command_id = event.command_id.value_or(state.active_command_id.value_or(Id{}));
      const auto status_transition = state.outbox.apply_status(command_id, event.status);
      append_effects(transition.effects, status_transition.effects);
      on_command_effects(state, status_transition.effects, event.now_ms);
      break;
    }
    case InteractionEventKind::CommandStatuses: {
      const auto status_transition = state.outbox.reconcile(event.statuses);
      append_effects(transition.effects, status_transition.effects);
      on_command_effects(state, status_transition.effects, event.now_ms);
      break;
    }
    case InteractionEventKind::SetConnectivity:
      state.live_handshake = event.live_handshake;
      break;
    case InteractionEventKind::Tick:
      if ((state.mode == InteractionMode::AppliedGlow || state.mode == InteractionMode::ErrorGlow) &&
          event.now_ms >= state.glow_started_ms &&
          event.now_ms - state.glow_started_ms >= kGlowDurationMs) {
        if (state.mode == InteractionMode::AppliedGlow) {
          state.mode = InteractionMode::Ambient;
          Effect effect;
          effect.kind = EffectKind::RestoreAmbient;
          transition.effects.push_back(effect);
        } else {
          state.mode = state.glow_return_mode;
        }
      }
      break;
    case InteractionEventKind::ResetAmbient:
      state.request.reset();
      state.transcript.reset();
      state.mode = InteractionMode::Ambient;
      state.active_command_id.reset();
      state.transcript_target = {};
      state.transcript_stream_id = {};
      state.transcript_id = {};
      state.transcript_target_frozen = false;
      {
        Effect effect;
        effect.kind = EffectKind::RestoreAmbient;
        transition.effects.push_back(effect);
      }
      break;
  }
  return transition;
}

}  // namespace t3::companion
