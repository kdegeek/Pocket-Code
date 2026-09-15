#include "sleep_lock.hpp"

namespace t3::companion {

LockTransition SleepLockController::snapshot() const { return LockTransition{state_, {}}; }

void SleepLockController::enter_lock(std::uint64_t now_ms,
                                     std::vector<LockEffect>& effects) {
  state_.locked = true;
  state_.display_on = false;
  state_.touch_enabled = false;
  state_.voice_enabled = false;
  state_.transport_retained = true;
  state_.hard_power_off = false;
  effects.push_back(LockEffect{LockEffectKind::Locked, now_ms});
  effects.push_back(LockEffect{LockEffectKind::DisplayOff, now_ms});
}

void SleepLockController::leave_lock(std::uint64_t now_ms,
                                     std::vector<LockEffect>& effects) {
  state_.locked = false;
  state_.display_on = true;
  state_.touch_enabled = true;
  state_.voice_enabled = true;
  state_.transport_retained = true;
  state_.hard_power_off = false;
  effects.push_back(LockEffect{LockEffectKind::Unlocked, now_ms});
  effects.push_back(LockEffect{LockEffectKind::DisplayOn, now_ms});
}

LockTransition SleepLockController::on_button(const ButtonEvent& event) {
  LockTransition transition = snapshot();
  if (event.button != ButtonId::BottomRight ||
      event.kind != ButtonEventKind::DoubleClick) {
    return transition;
  }
  if (state_.locked) {
    leave_lock(event.timestamp_ms, transition.effects);
  } else {
    enter_lock(event.timestamp_ms, transition.effects);
  }
  transition.state = state_;
  return transition;
}

LockTransition SleepLockController::on_prompt_arrival(std::uint64_t now_ms) {
  LockTransition transition = snapshot();
  if (state_.locked) {
    transition.effects.push_back(LockEffect{LockEffectKind::PromptSuppressed, now_ms});
  }
  return transition;
}

LockTransition SleepLockController::on_touch(std::uint64_t now_ms) {
  LockTransition transition = snapshot();
  if (state_.locked) {
    transition.effects.push_back(LockEffect{LockEffectKind::TouchRejected, now_ms});
  }
  return transition;
}

LockTransition SleepLockController::on_voice(std::uint64_t now_ms) {
  LockTransition transition = snapshot();
  if (state_.locked) {
    transition.effects.push_back(LockEffect{LockEffectKind::VoiceRejected, now_ms});
  }
  return transition;
}

LockTransition SleepLockController::on_reboot(std::uint64_t now_ms) {
  LockTransition transition = snapshot();
  const bool needed_display_restore = !state_.display_on;
  const bool needed_unlock = state_.locked;
  state_ = LockState{};
  state_.transport_retained = true;
  state_.hard_power_off = false;
  if (needed_unlock) transition.effects.push_back(LockEffect{LockEffectKind::Unlocked, now_ms});
  if (needed_display_restore) {
    transition.effects.push_back(LockEffect{LockEffectKind::DisplayOn, now_ms});
  }
  transition.state = state_;
  return transition;
}

}  // namespace t3::companion
