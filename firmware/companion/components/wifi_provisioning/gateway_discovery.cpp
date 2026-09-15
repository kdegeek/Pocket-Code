#include "gateway_discovery.hpp"

#include <algorithm>
#include <cctype>

namespace t3::companion {

bool GatewayDiscovery::valid_url(std::string_view url) {
  if (url.empty() || url.size() > kMaxGatewayUrlBytes) {
    return false;
  }
  if (!(url.starts_with("http://") || url.starts_with("https://"))) {
    return false;
  }
  const auto host_start = url.find("://") + 3U;
  if (host_start >= url.size()) {
    return false;
  }
  return std::all_of(url.begin() + static_cast<std::ptrdiff_t>(host_start), url.end(),
                     [](unsigned char byte) {
                       return !std::isspace(byte) && byte >= 0x21U && byte != 0x7fU;
                     });
}

std::optional<GatewayCandidate> GatewayDiscovery::discover() const {
  for (const auto& candidate : scan_results_) {
    if (valid_url(candidate.url)) {
      return candidate;
    }
  }
  return std::nullopt;
}

bool GatewayDiscovery::select_scan(std::size_t index) {
  if (index >= scan_results_.size() || !valid_url(scan_results_[index].url)) {
    return false;
  }
  selected_ = scan_results_[index];
  return true;
}

bool GatewayDiscovery::select_manual(std::string_view url) {
  if (!valid_url(url)) {
    return false;
  }
  selected_ = GatewayCandidate{"manual", std::string(url)};
  return true;
}

}  // namespace t3::companion
