#pragma once

#include <string>
#include <string_view>

namespace t3::companion {

struct HttpResponse {
  int status = 0;
  std::string body;
};

/**
 * Transport-neutral HTTP seam.  The ESP-IDF adapter owns sockets, response
 * bounds, and certificate handling; the protocol/state core only sees this
 * small request/response boundary so host fakes can exercise failure paths.
 */
class HttpClient {
 public:
  virtual ~HttpClient() = default;
  [[nodiscard]] virtual HttpResponse get(std::string_view url,
                                          std::string_view bearer_token) = 0;
};

/** Certificate/peer identity verification seam for remote HTTPS gateways. */
class TlsIdentityVerifier {
 public:
  virtual ~TlsIdentityVerifier() = default;
  [[nodiscard]] virtual bool verify(std::string_view host,
                                    std::string_view peer_identity) = 0;
};

}  // namespace t3::companion
