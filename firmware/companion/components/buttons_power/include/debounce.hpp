#pragma once

#include <cstdint>
#include <optional>

namespace t3::companion {

struct DebounceConfig {
  std::uint64_t stable_ms = 30;
};

struct DebouncedEvent {
  bool pressed = false;
  std::uint64_t timestamp_ms = 0;
};

/**
 * Converts an electrically noisy level into one stable pressed/released edge.
 * Time is supplied by the caller so this seam remains deterministic on host
 * and on the ESP-IDF board task.
 */
class Debouncer {
 public:
  explicit Debouncer(DebounceConfig config = {});

  void reset(bool stable_pressed = false);
  [[nodiscard]] std::optional<DebouncedEvent> feed(bool raw_pressed,
                                                    std::uint64_t now_ms);
  [[nodiscard]] std::optional<DebouncedEvent> tick(std::uint64_t now_ms);

  [[nodiscard]] bool stable_pressed() const { return stable_pressed_; }
  [[nodiscard]] bool candidate_pressed() const { return candidate_pressed_; }

 private:
  [[nodiscard]] std::uint64_t elapsed(std::uint64_t now_ms) const;

  DebounceConfig config_;
  bool initialized_ = false;
  bool stable_pressed_ = false;
  bool candidate_pressed_ = false;
  std::uint64_t candidate_since_ms_ = 0;
};

}  // namespace t3::companion
