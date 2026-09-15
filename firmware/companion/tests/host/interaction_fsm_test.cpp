#include "interaction_fsm.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace t3::companion;

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

Id id(const char* value) {
  Id result;
  (void)result.assign(value);
  return result;
}

PendingRequest approval() {
  PendingRequest request;
  request.environment_id = id("env");
  request.project_id = id("project");
  request.thread_id = id("thread");
  request.expected_turn_id = id("turn");
  request.request_id = id("request");
  request.kind = PendingRequestKind::Approval;
  (void)request.available_decisions.push(ApprovalDecision::Accept);
  (void)request.available_decisions.push(ApprovalDecision::Decline);
  (void)request.available_decisions.push(ApprovalDecision::Cancel);
  return request;
}

QuestionOption option(const char* label) {
  QuestionOption result;
  (void)result.label.assign(label);
  (void)result.description.assign("description");
  return result;
}

PendingRequest multi_question() {
  PendingRequest request;
  request.environment_id = id("env");
  request.project_id = id("project");
  request.thread_id = id("thread");
  request.expected_turn_id = id("turn");
  request.request_id = id("request");
  request.kind = PendingRequestKind::UserInput;

  UserInputQuestion first;
  (void)first.id.assign("first");
  (void)first.options.push(option("read"));
  (void)first.options.push(option("write"));
  (void)request.questions.push(first);

  UserInputQuestion second;
  (void)second.id.assign("second");
  second.multi_select = true;
  (void)second.options.push(option("all"));
  (void)second.options.push(option("changed"));
  (void)request.questions.push(second);
  return request;
}

void test_approval_swipes_and_acknowledgement() {
  InteractionState state = make_interaction_state(true);
  auto opened = reduce_interaction(state, InteractionEvent::open(approval(), 10));
  state = opened.state;
  expect(state.mode == InteractionMode::Approval, "approval request opens takeover mode");

  auto accepted = reduce_interaction(state, InteractionEvent::gesture(Gesture::SwipeRight, 11));
  expect(accepted.effects.size() == 2, "approval emits persist and send effects once");
  expect(accepted.effects[1].kind == EffectKind::SendCommand, "approval sends command declaratively");
  expect(accepted.effects[1].command.has_value(), "approval effect contains command");
  expect(accepted.effects[1].command->decision == ApprovalDecision::Accept,
         "right swipe maps to accept");
  expect(accepted.effects[1].command->context.thread_id.view() == "thread",
         "approval captures immutable target context");
  expect(accepted.state.outbox.size() == 1, "approval creates one outbox command");
  const auto command_id = accepted.effects[1].command->command_id;

  const auto duplicate = reduce_interaction(accepted.state,
                                            InteractionEvent::gesture(Gesture::SwipeRight, 12));
  expect(duplicate.state.outbox.size() == 1, "repeated gesture cannot create a second command");

  const auto progress = reduce_interaction(accepted.state,
                                           InteractionEvent::command_status(command_id,
                                                                           CommandStatus::Committed,
                                                                           13));
  expect(!has_effect(progress.effects, EffectKind::ShowAppliedGlow),
         "committed dispatch does not show success glow");
  const auto applied = reduce_interaction(progress.state,
                                          InteractionEvent::command_status(command_id,
                                                                          CommandStatus::Applied,
                                                                          14));
  expect(has_effect(applied.effects, EffectKind::ShowAppliedGlow),
         "only applied status shows success glow");

  const auto rejected = reduce_interaction(accepted.state,
                                           InteractionEvent::command_status(command_id,
                                                                           CommandStatus::Rejected,
                                                                           15));
  expect(has_effect(rejected.effects, EffectKind::ShowErrorGlow),
         "rejected status shows error glow");
}

void test_bounded_and_multi_select_answers() {
  InteractionState state = make_interaction_state(true);
  state = reduce_interaction(state, InteractionEvent::open(multi_question(), 20)).state;

  state = reduce_interaction(state, InteractionEvent::gesture(Gesture::BrowseNext, 21)).state;
  expect(state.card_index == 1, "horizontal swipe browses bounded choices");
  const auto first_submit = reduce_interaction(state,
                                               InteractionEvent::gesture(Gesture::SwipeUp, 22));
  expect(first_submit.effects.empty(), "non-final single answer stays local");
  expect(first_submit.state.question_index == 1, "multi-question flow advances in source order");

  const auto toggle = reduce_interaction(first_submit.state,
                                         InteractionEvent::gesture(Gesture::SwipeUp, 23));
  expect(toggle.effects.empty(), "multi-select option toggles without sending early");
  expect(toggle.state.selected_option_indices.size() == 1,
         "multi-select remembers the centered option");
  const auto next_card = reduce_interaction(toggle.state,
                                            InteractionEvent::gesture(Gesture::BrowseNext, 24));
  const auto submit_card = reduce_interaction(next_card.state,
                                              InteractionEvent::gesture(Gesture::BrowseNext, 25));
  const auto final_submit = reduce_interaction(submit_card.state,
                                               InteractionEvent::gesture(Gesture::SwipeUp, 26));
  expect(final_submit.effects.size() == 2, "final multi-question answer emits one command");
  expect(final_submit.state.outbox.size() == 1, "complete answer record uses one command");
  expect(final_submit.effects[1].command->payload_kind == DeviceCommandPayload::Answers,
         "bounded answer command carries answer record");
  expect(final_submit.effects[1].command->answers.entries.count == 2,
         "answer record retains every question id");
}

void test_offline_rejection_and_transcript_context() {
  InteractionState offline = make_interaction_state(false);
  const auto rejected = reduce_interaction(offline,
                                           InteractionEvent::open(approval(), 30));
  const auto attempt = reduce_interaction(rejected.state,
                                          InteractionEvent::gesture(Gesture::SwipeLeft, 31));
  expect(has_effect(attempt.effects, EffectKind::OfflineRejected),
         "offline mutating gesture is rejected declaratively");
  expect(attempt.state.outbox.size() == 0, "offline action does not create an outbox command");

  InteractionState online = make_interaction_state(true);
  online = reduce_interaction(online, InteractionEvent::open(approval(), 32)).state;
  const auto proposed = reduce_interaction(online,
                                           InteractionEvent::transcript("follow up", 33));
  expect(proposed.state.mode == InteractionMode::Transcript,
         "transcript enters confirmation mode");
  const auto sent = reduce_interaction(proposed.state,
                                       InteractionEvent::gesture(Gesture::SwipeRight, 34));
  expect(sent.effects.size() == 2, "transcript submit emits one command");
  expect(sent.effects[1].command->type == DeviceCommandType::SubmitTranscript,
         "transcript right swipe submits");
  expect(sent.effects[1].command->context.request_id.has_value(),
         "transcript keeps captured request context");
}

}  // namespace

int main() {
  test_approval_swipes_and_acknowledgement();
  test_bounded_and_multi_select_answers();
  test_offline_rejection_and_transcript_context();
  if (failures != 0) {
    std::cerr << failures << " interaction FSM test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "T3 Companion interaction FSM tests passed\n";
  return EXIT_SUCCESS;
}
