#pragma once

#include "ambient_view_fwd.hpp"

#include <string>

namespace t3::ui {

enum class OverlayTone { None, Neutral, Info, Success, Error };

struct OverlayModel {
  OverlayTone tone = OverlayTone::None;
  std::string title;
  std::string detail;
  bool visible = false;
};

[[nodiscard]] OverlayModel overlay_for(ViewState state);

}  // namespace t3::ui
