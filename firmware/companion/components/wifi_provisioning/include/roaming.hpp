#pragma once

#include "network_store.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace t3::companion {

class RoamingController {
 public:
  explicit RoamingController(NetworkStore& store) : store_(store) {}

  [[nodiscard]] std::optional<NetworkCredential> next_attempt() const;
  [[nodiscard]] bool report_failure();
  [[nodiscard]] bool report_success(std::string_view ssid);
  void reset() { cursor_ = 0; }
  [[nodiscard]] std::size_t cursor() const { return cursor_; }

 private:
  NetworkStore& store_;
  std::size_t cursor_ = 0;
};

}  // namespace t3::companion

namespace t3::companion::wifi_provisioning {
using ::t3::companion::RoamingController;
}  // namespace t3::companion::wifi_provisioning
