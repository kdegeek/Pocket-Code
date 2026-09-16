#include "overlays.hpp"

namespace t3::ui {

OverlayModel overlay_for(ViewState state) {
  OverlayModel result;
  switch (state) {
    case ViewState::Ambient:
      break;
    case ViewState::Locked:
      result = {OverlayTone::Neutral, "LOCKED", "Double-click to wake", true};
      break;
    case ViewState::Reconnecting:
      result = {OverlayTone::Info, "RECONNECTING", "Cached state is stale", true};
      break;
    case ViewState::Incompatible:
      result = {OverlayTone::Error, "INCOMPATIBLE", "Update Pocket-Code firmware", true};
      break;
    case ViewState::Prompt:
    case ViewState::BoundedChoice:
    case ViewState::Transcript:
      break;
    case ViewState::AppliedGlow:
      result = {OverlayTone::Success, "APPLIED", "Acknowledged by gateway", true};
      break;
    case ViewState::RejectedGlow:
      result = {OverlayTone::Error, "REJECTED", "Gateway did not apply the action", true};
      break;
    case ViewState::Provisioning:
      result = {OverlayTone::Info, "PROVISIONING", "Add a remembered network", true};
      break;
  }
  return result;
}

}  // namespace t3::ui
