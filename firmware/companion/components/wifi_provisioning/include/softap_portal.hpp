#pragma once

#include "network_store.hpp"
#include "types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace t3::companion {

enum class PortalState { Idle, Advertising, SetupJoinRequested, SetupJoined, Completed, Cancelled, Error };

// Pure portal lifecycle state. Starting an AP never tears down the current
// station; only join_setup_ap() is an intentional station handoff.
class SoftApPortal {
 public:
  SoftApPortal(NetworkStore& networks, std::uint16_t device_suffix)
      : networks_(networks), device_suffix_(device_suffix) {}

  [[nodiscard]] bool begin(bool station_connected);
  [[nodiscard]] bool start(bool station_connected) { return begin(station_connected); }
  [[nodiscard]] bool request_setup_join();
  [[nodiscard]] bool join_setup_ap();
  [[nodiscard]] bool append_network(std::string_view ssid, std::string_view password);
  [[nodiscard]] bool complete();
  [[nodiscard]] bool cancel();
  [[nodiscard]] bool fail();

  [[nodiscard]] PortalState state() const { return state_; }
  [[nodiscard]] ProvisioningStatus status() const { return status_; }
  [[nodiscard]] ProvisioningStatus provisioning_status() const { return status_; }
  [[nodiscard]] const std::string& ap_ssid() const { return ap_ssid_; }
  [[nodiscard]] bool station_preserved() const { return station_preserved_; }
  [[nodiscard]] bool appended() const { return appended_; }

 private:
  NetworkStore& networks_;
  std::uint16_t device_suffix_;
  PortalState state_ = PortalState::Idle;
  ProvisioningStatus status_ = ProvisioningStatus::Cancelled;
  std::string ap_ssid_;
  bool station_preserved_ = false;
  bool appended_ = false;
};

}  // namespace t3::companion

namespace t3::companion::wifi_provisioning {
using ::t3::companion::PortalState;
using ::t3::companion::SoftApPortal;
}  // namespace t3::companion::wifi_provisioning
