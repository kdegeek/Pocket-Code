#pragma once

#include "interaction_fsm.hpp"

#include <cstddef>
#include <string>

namespace t3::ui {

using t3::companion::InteractionMode;
using t3::companion::InteractionState;
using t3::companion::PendingRequestKind;

enum class PromptCardKind { None, Approval, Choice, AwaitingAck };

struct PromptCardModel {
  PromptCardKind kind = PromptCardKind::None;
  std::string kicker;
  std::string title;
  std::string answer;
  std::string detail;
  std::string gesture_hint;
  std::size_t index = 0;
  std::size_t count = 0;
  bool pending = false;
};

[[nodiscard]] PromptCardModel make_prompt_card(const InteractionState& state);

}  // namespace t3::ui
