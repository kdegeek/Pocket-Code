#include "prompt_cards.hpp"

#include <algorithm>

namespace t3::ui {

PromptCardModel make_prompt_card(const InteractionState& state) {
  PromptCardModel result;
  if (!state.request.has_value()) {
    return result;
  }
  const auto& request = *state.request;
  result.pending = state.mode == InteractionMode::AwaitingAck;
  if (request.kind == PendingRequestKind::Approval) {
    result.kind = result.pending ? PromptCardKind::AwaitingAck : PromptCardKind::Approval;
    result.kicker = "APPROVAL REQUEST";
    result.title = request.detail.has_value() ? std::string(request.detail->view())
                                               : "Decision requested";
    result.answer = "Run request";
    result.gesture_hint = "← deny · approve → · cancel ↓";
    return result;
  }

  result.kind = result.pending ? PromptCardKind::AwaitingAck : PromptCardKind::Choice;
  result.kicker = "CHOOSE ONE";
  if (state.question_index >= request.questions.count) {
    result.title = "Answer requested";
    result.answer = "Submit selected";
    result.gesture_hint = "answer ↑ · cancel ↓";
    return result;
  }
  const auto& question = request.questions[state.question_index];
  result.title = std::string(question.question.view());
  result.index = state.card_index;
  result.count = question.options.count + (question.multi_select ? 1U : 0U);
  if (question.options.count == 0U) {
    result.answer = "Hold push-to-talk";
    result.gesture_hint = "voice input · cancel ↓";
    return result;
  }
  if (state.card_index < question.options.count) {
    result.answer = std::string(question.options[state.card_index].label.view());
  } else {
    result.answer = "Submit selected";
  }
  result.gesture_hint = question.multi_select ? "← browse → · toggle ↑ · cancel ↓"
                                              : "← browse → · answer ↑ · cancel ↓";
  return result;
}

}  // namespace t3::ui
