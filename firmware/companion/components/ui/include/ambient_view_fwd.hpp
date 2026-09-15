#pragma once

namespace t3::ui {

enum class ViewState {
  Ambient,
  Locked,
  Reconnecting,
  Incompatible,
  Prompt,
  BoundedChoice,
  Transcript,
  AppliedGlow,
  RejectedGlow,
  Provisioning,
};

}  // namespace t3::ui
