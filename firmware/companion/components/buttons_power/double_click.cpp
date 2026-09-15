#include "double_click.hpp"

namespace t3::companion {

DoubleClickRecognizer::DoubleClickRecognizer(ClickConfig config) : config_(config) {}

void DoubleClickRecognizer::reset() {
  pressed_ = false;
  long_hold_started_ = false;
  click_pending_ = false;
  suppress_release_click_ = false;
  pressed_since_ms_ = 0;
  first_click_released_ms_ = 0;
}

std::uint64_t DoubleClickRecognizer::elapsed_since(std::uint64_t start_ms,
                                                    std::uint64_t now_ms) const {
  return now_ms >= start_ms ? now_ms - start_ms : 0;
}

void DoubleClickRecognizer::emit_long_hold_if_due(
    std::uint64_t now_ms, std::vector<ClickEvent>& events) {
  if (!pressed_ || long_hold_started_ ||
      elapsed_since(pressed_since_ms_, now_ms) < config_.long_hold_ms) {
    return;
  }
  long_hold_started_ = true;
  events.push_back(ClickEvent{ButtonEventKind::LongHoldStart, now_ms});
}

std::vector<ClickEvent> DoubleClickRecognizer::feed(bool pressed,
                                                      std::uint64_t now_ms) {
  std::vector<ClickEvent> events;
  if (pressed == pressed_) {
    // A repeated stable level still provides an opportunity to observe a
    // long hold without requiring a separate timer callback.
    if (pressed) emit_long_hold_if_due(now_ms, events);
    return events;
  }

  if (pressed) {
    pressed_ = true;
    pressed_since_ms_ = now_ms;
    long_hold_started_ = false;
    events.push_back(ClickEvent{ButtonEventKind::Pressed, now_ms});

    if (click_pending_) {
      if (elapsed_since(first_click_released_ms_, now_ms) <= config_.click_timeout_ms) {
        click_pending_ = false;
        suppress_release_click_ = true;
        events.push_back(ClickEvent{ButtonEventKind::DoubleClick, now_ms});
      } else {
        click_pending_ = false;
        events.push_back(ClickEvent{ButtonEventKind::SingleClick,
                                    first_click_released_ms_});
      }
    }
    return events;
  }

  // A release that crosses the hold boundary still reports a complete hold
  // even when the task did not run a tick at the exact boundary.
  emit_long_hold_if_due(now_ms, events);
  pressed_ = false;
  events.push_back(ClickEvent{ButtonEventKind::Released, now_ms});
  if (long_hold_started_) {
    events.push_back(ClickEvent{ButtonEventKind::LongHoldEnd, now_ms});
    long_hold_started_ = false;
    click_pending_ = false;
    suppress_release_click_ = false;
  } else if (suppress_release_click_) {
    // The second release belongs to the already-emitted double-click and
    // must not start a new single-click timeout.
    suppress_release_click_ = false;
    click_pending_ = false;
  } else {
    click_pending_ = true;
    first_click_released_ms_ = now_ms;
  }
  return events;
}

std::vector<ClickEvent> DoubleClickRecognizer::tick(std::uint64_t now_ms) {
  std::vector<ClickEvent> events;
  if (pressed_) {
    emit_long_hold_if_due(now_ms, events);
    return events;
  }
  if (click_pending_ &&
      elapsed_since(first_click_released_ms_, now_ms) >= config_.click_timeout_ms) {
    click_pending_ = false;
    events.push_back(ClickEvent{ButtonEventKind::SingleClick, now_ms});
  }
  return events;
}

std::vector<ClickEvent> DoubleClickRecognizer::cancel(std::uint64_t now_ms) {
  std::vector<ClickEvent> events;
  if (pressed_ || click_pending_) {
    events.push_back(ClickEvent{ButtonEventKind::Cancelled, now_ms});
  }
  reset();
  return events;
}

}  // namespace t3::companion
