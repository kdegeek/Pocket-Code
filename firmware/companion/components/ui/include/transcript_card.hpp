#pragma once

#include "interaction_fsm.hpp"

#include <string>

namespace t3::ui {

using t3::companion::InteractionState;

struct TranscriptCardModel {
  bool visible = false;
  std::string kicker = "TRANSCRIPT";
  std::string transcript;
  std::string gesture_hint = "← discard · submit → · cancel ↓";
};

[[nodiscard]] TranscriptCardModel make_transcript_card(const InteractionState& state);

}  // namespace t3::ui
