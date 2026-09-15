#include "wifi_ws_client.hpp"

namespace t3::companion {

// The ESP-IDF websocket implementation is injected by the live composition
// layer.  No socket is opened by this neutral component or by app_main's
// default (disabled) composition.

bool send_companion_audio_frame(WebSocketClient& client,
                                std::span<const std::uint8_t> frame) {
  if (frame.empty()) return false;
  return client.send_binary(frame);
}

}  // namespace t3::companion
