#pragma once

#include <cstdint>
#include <vector>

namespace t3::companion {

enum class ButtonEventKind : std::uint8_t {
  Pressed,
  Released,
  SingleClick,
  DoubleClick,
  LongHoldStart,
  LongHoldEnd,
  Cancelled,

  // Compatibility aliases keep the seam readable for callers that use the
  // shorter physical-input vocabulary.
  Press = Pressed,
  Release = Released,
  LongPressStart = LongHoldStart,
  LongPressEnd = LongHoldEnd,
  Cancel = Cancelled,
};

struct ClickConfig {
  std::uint64_t click_timeout_ms = 300;
  std::uint64_t long_hold_ms = 500;
};

struct ClickEvent {
  ButtonEventKind kind = ButtonEventKind::Released;
  std::uint64_t timestamp_ms = 0;
};

/**
 * Recognizes short clicks, a timeout-delimited double-click, and a long hold.
 * It consumes already-debounced edges; callers must invoke tick() while the
 * button task is idle to release a pending single click and to observe a hold
 * without waiting for release.
 */
class DoubleClickRecognizer {
 public:
  explicit DoubleClickRecognizer(ClickConfig config = {});

  void reset();
  [[nodiscard]] std::vector<ClickEvent> feed(bool pressed,
                                              std::uint64_t now_ms);
  [[nodiscard]] std::vector<ClickEvent> tick(std::uint64_t now_ms);
  [[nodiscard]] std::vector<ClickEvent> cancel(std::uint64_t now_ms);

  [[nodiscard]] bool pressed() const { return pressed_; }
  [[nodiscard]] bool click_pending() const { return click_pending_; }
  [[nodiscard]] bool long_hold_started() const { return long_hold_started_; }

 private:
  [[nodiscard]] std::uint64_t elapsed_since(std::uint64_t start_ms,
                                             std::uint64_t now_ms) const;
  void emit_long_hold_if_due(std::uint64_t now_ms,
                             std::vector<ClickEvent>& events);

  ClickConfig config_;
  bool pressed_ = false;
  bool long_hold_started_ = false;
  bool click_pending_ = false;
  bool suppress_release_click_ = false;
  std::uint64_t pressed_since_ms_ = 0;
  std::uint64_t first_click_released_ms_ = 0;
};

}  // namespace t3::companion
