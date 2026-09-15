#pragma once

#include "button_input.hpp"

#include <cstdint>
#include <vector>

namespace t3::companion {

enum class PttEffectKind : std::uint8_t {
  VoiceOverlayOpened,
  CaptureStarted,
  CaptureStopped,
  RequestTranscription,
  CaptureCancelled,
  RejectedLocked,
  RejectedUnavailable,

  Started = CaptureStarted,
  Stopped = CaptureStopped,
  Cancelled = CaptureCancelled,
};

struct PttContext {
  bool locked = false;
  bool live_session = true;
  bool voice_enabled = true;
  // Kept as context for Task 10's transcript routing.  Free-form prompts may
  // opt in; when false, confirmed speech is a new message to the focused task.
  bool prompt_accepts_free_form = false;
};

struct PttEffect {
  PttEffectKind kind = PttEffectKind::CaptureCancelled;
  std::uint64_t timestamp_ms = 0;
};

class PttButtonController {
 public:
  explicit PttButtonController(PttContext context = {}) : context_(context) {}

  void set_context(PttContext context) { context_ = context; }
  [[nodiscard]] const PttContext& context() const { return context_; }

  [[nodiscard]] std::vector<PttEffect> on_button(const ButtonEvent& event);
  [[nodiscard]] std::vector<PttEffect> feed(const ButtonEvent& event) {
    return on_button(event);
  }
  [[nodiscard]] std::vector<PttEffect> cancel(std::uint64_t now_ms);
  [[nodiscard]] std::vector<PttEffect> on_disconnect(std::uint64_t now_ms);
  [[nodiscard]] std::vector<PttEffect> on_lock_changed(bool locked,
                                                        std::uint64_t now_ms);

  [[nodiscard]] bool capturing() const { return capturing_; }
  [[nodiscard]] bool overlay_visible() const { return overlay_visible_; }

 private:
  [[nodiscard]] std::vector<PttEffect> reject(std::uint64_t now_ms) const;
  [[nodiscard]] std::vector<PttEffect> start(std::uint64_t now_ms);
  [[nodiscard]] std::vector<PttEffect> stop(std::uint64_t now_ms);

  PttContext context_;
  bool capturing_ = false;
  bool overlay_visible_ = false;
};

using PttButton = PttButtonController;

}  // namespace t3::companion
