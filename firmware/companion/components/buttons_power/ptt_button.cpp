#include "ptt_button.hpp"

namespace t3::companion {

std::vector<PttEffect> PttButtonController::reject(std::uint64_t now_ms) const {
  const PttEffectKind kind = context_.locked ? PttEffectKind::RejectedLocked
                                             : PttEffectKind::RejectedUnavailable;
  return {PttEffect{kind, now_ms}};
}

std::vector<PttEffect> PttButtonController::start(std::uint64_t now_ms) {
  if (capturing_) return {};
  capturing_ = true;
  overlay_visible_ = true;
  return {PttEffect{PttEffectKind::VoiceOverlayOpened, now_ms},
          PttEffect{PttEffectKind::CaptureStarted, now_ms}};
}

std::vector<PttEffect> PttButtonController::stop(std::uint64_t now_ms) {
  if (!capturing_) return {};
  capturing_ = false;
  overlay_visible_ = false;
  return {PttEffect{PttEffectKind::CaptureStopped, now_ms},
          PttEffect{PttEffectKind::RequestTranscription, now_ms}};
}

std::vector<PttEffect> PttButtonController::on_button(const ButtonEvent& event) {
  if (event.button != ButtonId::UpperRight) return {};
  switch (event.kind) {
    case ButtonEventKind::Pressed:
    case ButtonEventKind::LongHoldStart:
      if (capturing_) return {};
      if (context_.locked || !context_.voice_enabled || !context_.live_session) {
        return reject(event.timestamp_ms);
      }
      return start(event.timestamp_ms);
    case ButtonEventKind::Released:
    case ButtonEventKind::LongHoldEnd:
      return stop(event.timestamp_ms);
    case ButtonEventKind::Cancelled:
      return cancel(event.timestamp_ms);
    case ButtonEventKind::SingleClick:
    case ButtonEventKind::DoubleClick:
      return {};
  }
  return {};
}

std::vector<PttEffect> PttButtonController::cancel(std::uint64_t now_ms) {
  if (!capturing_ && !overlay_visible_) return {};
  capturing_ = false;
  overlay_visible_ = false;
  return {PttEffect{PttEffectKind::CaptureCancelled, now_ms}};
}

std::vector<PttEffect> PttButtonController::on_disconnect(std::uint64_t now_ms) {
  context_.live_session = false;
  return cancel(now_ms);
}

std::vector<PttEffect> PttButtonController::on_lock_changed(bool locked,
                                                              std::uint64_t now_ms) {
  context_.locked = locked;
  if (locked) return cancel(now_ms);
  return {};
}

}  // namespace t3::companion
