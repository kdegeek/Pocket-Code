#include "roaming.hpp"

namespace t3::companion {

std::optional<NetworkCredential> RoamingController::next_attempt() const {
  const auto ordered = store_.fallback_sequence();
  if (cursor_ >= ordered.size()) {
    return std::nullopt;
  }
  return ordered[cursor_];
}

bool RoamingController::report_failure() {
  const auto ordered = store_.fallback_sequence();
  if (cursor_ >= ordered.size()) {
    return false;
  }
  ++cursor_;
  return true;
}

bool RoamingController::report_success(std::string_view ssid) {
  if (!store_.find(ssid).has_value() || !store_.mark_successful(ssid)) {
    return false;
  }
  cursor_ = 0;
  return true;
}

}  // namespace t3::companion
