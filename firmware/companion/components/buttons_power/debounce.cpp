#include "debounce.hpp"

#include <algorithm>

namespace t3::companion {

Debouncer::Debouncer(DebounceConfig config) : config_(config) {
  // A zero interval is useful for deterministic tests but still must not
  // underflow when elapsed time is calculated.
  config_.stable_ms = config.stable_ms;
}

void Debouncer::reset(bool stable_pressed) {
  initialized_ = false;
  stable_pressed_ = stable_pressed;
  candidate_pressed_ = stable_pressed;
  candidate_since_ms_ = 0;
}

std::uint64_t Debouncer::elapsed(std::uint64_t now_ms) const {
  return now_ms >= candidate_since_ms_ ? now_ms - candidate_since_ms_ : 0;
}

std::optional<DebouncedEvent> Debouncer::feed(bool raw_pressed,
                                               std::uint64_t now_ms) {
  if (!initialized_) {
    initialized_ = true;
    candidate_pressed_ = raw_pressed;
    candidate_since_ms_ = now_ms;
    return tick(now_ms);
  }
  if (raw_pressed != candidate_pressed_) {
    candidate_pressed_ = raw_pressed;
    candidate_since_ms_ = now_ms;
    return std::nullopt;
  }
  return tick(now_ms);
}

std::optional<DebouncedEvent> Debouncer::tick(std::uint64_t now_ms) {
  if (!initialized_ || candidate_pressed_ == stable_pressed_ ||
      elapsed(now_ms) < config_.stable_ms) {
    return std::nullopt;
  }
  stable_pressed_ = candidate_pressed_;
  return DebouncedEvent{stable_pressed_, now_ms};
}

}  // namespace t3::companion
