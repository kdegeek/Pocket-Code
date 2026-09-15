#include "transcript_card.hpp"

namespace t3::ui {

using t3::companion::InteractionMode;

TranscriptCardModel make_transcript_card(const InteractionState& state) {
  TranscriptCardModel result;
  if (!state.transcript.has_value()) {
    return result;
  }
  result.visible = state.mode == InteractionMode::Transcript ||
                   state.mode == InteractionMode::AwaitingAck;
  result.transcript = std::string(state.transcript->view());
  if (state.mode == InteractionMode::AwaitingAck) {
    result.kicker = "SENDING TRANSCRIPT";
  }
  return result;
}

}  // namespace t3::ui
