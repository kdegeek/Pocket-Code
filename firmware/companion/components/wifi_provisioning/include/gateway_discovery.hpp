#pragma once

#include "device_identity.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace t3::companion {

struct GatewayCandidate {
  std::string name;
  std::string url;
};

// Host tests inject scan results; a platform adapter can replace them with a
// mDNS/LAN scan without changing the provisioning state machine.
class GatewayDiscovery {
 public:
  [[nodiscard]] std::vector<GatewayCandidate> scan() const { return scan_results_; }
  void set_scan_results(std::vector<GatewayCandidate> results) {
    scan_results_ = std::move(results);
  }
  [[nodiscard]] std::optional<GatewayCandidate> discover() const;
  [[nodiscard]] bool select_scan(std::size_t index);
  [[nodiscard]] bool select_manual(std::string_view url);
  [[nodiscard]] bool set_manual(std::string_view url) { return select_manual(url); }
  [[nodiscard]] std::optional<GatewayCandidate> selected() const { return selected_; }
  void clear_selection() { selected_.reset(); }

  [[nodiscard]] static bool valid_url(std::string_view url);

 private:
  std::vector<GatewayCandidate> scan_results_;
  std::optional<GatewayCandidate> selected_;
};

}  // namespace t3::companion

namespace t3::companion::wifi_provisioning {
using ::t3::companion::GatewayCandidate;
using ::t3::companion::GatewayDiscovery;
}  // namespace t3::companion::wifi_provisioning
