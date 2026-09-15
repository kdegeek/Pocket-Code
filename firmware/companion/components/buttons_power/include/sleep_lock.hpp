#pragma once

#include "button_input.hpp"

#include <cstdint>
#include <vector>

namespace t3::companion {

enum class LockEffectKind : std::uint8_t {
  Locked,
  Unlocked,
  DisplayOff,
  DisplayOn,
  PromptSuppressed,
  TouchRejected,
  VoiceRejected,
};

struct LockState {
  bool locked = false;
  bool display_on = true;
  bool touch_enabled = true;
  bool voice_enabled = true;
  // Version 1 sleep is display-off only.  Wi-Fi/session retention is a
  // positive invariant, and hard power-off is never delegated to AXP PKEY.
  bool transport_retained = true;
  bool hard_power_off = false;
};

struct LockEffect {
  LockEffectKind kind = LockEffectKind::Unlocked;
  std::uint64_t timestamp_ms = 0;
};

struct LockTransition {
  LockState state;
  std::vector<LockEffect> effects;
};

class SleepLockController {
 public:
  SleepLockController() = default;

  [[nodiscard]] LockTransition on_button(const ButtonEvent& event);
  [[nodiscard]] LockTransition on_bottom_button(const ButtonEvent& event) {
    return on_button(event);
  }
  [[nodiscard]] LockTransition on_prompt_arrival(std::uint64_t now_ms);
  [[nodiscard]] LockTransition on_touch(std::uint64_t now_ms);
  [[nodiscard]] LockTransition on_voice(std::uint64_t now_ms);
  [[nodiscard]] LockTransition on_reboot(std::uint64_t now_ms = 0);

  [[nodiscard]] const LockState& state() const { return state_; }
  [[nodiscard]] bool locked() const { return state_.locked; }
  [[nodiscard]] bool display_on() const { return state_.display_on; }
  [[nodiscard]] bool accepts_touch() const { return state_.touch_enabled; }
  [[nodiscard]] bool accepts_voice() const { return state_.voice_enabled; }

 private:
  [[nodiscard]] LockTransition snapshot() const;
  void enter_lock(std::uint64_t now_ms, std::vector<LockEffect>& effects);
  void leave_lock(std::uint64_t now_ms, std::vector<LockEffect>& effects);

  LockState state_;
};

}  // namespace t3::companion
