#include "softap_portal.hpp"

#include <cstdio>

namespace t3::companion {

bool SoftApPortal::begin(bool station_connected) {
  if (state_ == PortalState::Advertising || state_ == PortalState::SetupJoinRequested ||
      state_ == PortalState::SetupJoined) {
    return false;
  }
  char name[20]{};
  const int written = std::snprintf(name, sizeof(name), "Pocket-Code-%04X", device_suffix_);
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(name)) {
    state_ = PortalState::Error;
    status_ = ProvisioningStatus::Error;
    return false;
  }
  ap_ssid_ = name;
  state_ = PortalState::Advertising;
  status_ = ProvisioningStatus::Started;
  station_preserved_ = station_connected;
  appended_ = false;
  return true;
}

bool SoftApPortal::request_setup_join() {
  if (state_ != PortalState::Advertising) {
    return false;
  }
  state_ = PortalState::SetupJoinRequested;
  return true;
}

bool SoftApPortal::join_setup_ap() {
  if (state_ != PortalState::SetupJoinRequested) {
    return false;
  }
  state_ = PortalState::SetupJoined;
  // This is the sole intentional handoff point. A caller that never invokes
  // this method can keep using the current station while the AP is advertised.
  station_preserved_ = false;
  return true;
}

bool SoftApPortal::append_network(std::string_view ssid, std::string_view password) {
  if (state_ != PortalState::SetupJoined) {
    return false;
  }
  const auto result = networks_.add_or_update(ssid, password);
  if (!result.ok) {
    state_ = PortalState::Error;
    status_ = ProvisioningStatus::Error;
    return false;
  }
  appended_ = true;
  return true;
}

bool SoftApPortal::complete() {
  if (state_ != PortalState::SetupJoined || !appended_) {
    return false;
  }
  state_ = PortalState::Completed;
  status_ = ProvisioningStatus::Completed;
  return true;
}

bool SoftApPortal::cancel() {
  if (state_ == PortalState::Completed || state_ == PortalState::Cancelled) {
    return false;
  }
  state_ = PortalState::Cancelled;
  status_ = ProvisioningStatus::Cancelled;
  return true;
}

bool SoftApPortal::fail() {
  if (state_ == PortalState::Completed || state_ == PortalState::Cancelled) {
    return false;
  }
  state_ = PortalState::Error;
  status_ = ProvisioningStatus::Error;
  return true;
}

}  // namespace t3::companion
