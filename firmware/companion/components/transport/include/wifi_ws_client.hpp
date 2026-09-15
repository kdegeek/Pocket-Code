#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace t3::companion {

/**
 * Transport-neutral WebSocket seam.  Only the dedicated companion route is
 * ever passed to this interface; the normal T3 effect `/ws` is not a valid
 * endpoint in the route builder.
 */
class WebSocketClient {
 public:
  virtual ~WebSocketClient() = default;
  [[nodiscard]] virtual bool open(std::string_view url,
                                  std::string_view bearer_token) = 0;
  [[nodiscard]] virtual bool send_text(std::string_view frame) = 0;
  // Binary frames are opt-in at the injected adapter.  The default returns
  // false so the inert firmware composition cannot accidentally stream audio.
  [[nodiscard]] virtual bool send_binary(std::span<const std::uint8_t> frame) {
    (void)frame;
    return false;
  }
  virtual void close() = 0;
};

// Host-safe seam used by AudioStreamSession: no socket is opened and no
// capture is enabled here; a live ESP-IDF adapter must explicitly override
// WebSocketClient::send_binary.
[[nodiscard]] bool send_companion_audio_frame(
    WebSocketClient& client, std::span<const std::uint8_t> frame);

}  // namespace t3::companion
