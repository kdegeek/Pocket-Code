#include "button_input.hpp"
#include "axp_pkey.hpp"
#include "double_click.hpp"
#include "ptt_button.hpp"
#include "sleep_lock.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace t3::companion;

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

bool has_event(const std::vector<ButtonEvent>& events, ButtonEventKind kind) {
  for (const auto& event : events) {
    if (event.kind == kind) return true;
  }
  return false;
}

bool has_effect(const std::vector<LockEffect>& effects, LockEffectKind kind) {
  for (const auto& effect : effects) {
    if (effect.kind == kind) return true;
  }
  return false;
}

bool has_effect(const std::vector<PttEffect>& effects, PttEffectKind kind) {
  for (const auto& effect : effects) {
    if (effect.kind == kind) return true;
  }
  return false;
}

void test_debounce_and_single_timeout() {
  ButtonInput input;
  expect(input.feed(ButtonId::BottomRight, true, 0).empty(),
         "active-low BOOT raw high is an unpressed baseline");
  expect(input.feed(ButtonId::BottomRight, false, 10).empty(),
         "a raw transition starts debounce without emitting");
  expect(input.feed(ButtonId::BottomRight, true, 15).empty(),
         "a bounce back before the stable interval is ignored");
  expect(input.feed(ButtonId::BottomRight, false, 20).empty(),
         "the next raw press candidate still waits for debounce");
  const auto pressed = input.tick(50);
  expect(has_event(pressed, ButtonEventKind::Pressed),
         "BOOT emits one stable press after 30 ms debounce");
  (void)input.feed(ButtonId::BottomRight, true, 80);
  const auto released = input.tick(110);
  expect(has_event(released, ButtonEventKind::Released),
         "BOOT emits one stable release after debounce");
  expect(input.tick(409).empty(), "single click remains pending before timeout");
  const auto single = input.tick(410);
  expect(has_event(single, ButtonEventKind::SingleClick),
         "single click emits only after the 300 ms timeout");
}

void test_double_click_and_long_hold() {
  ButtonInput input;
  (void)input.feed(ButtonId::BottomRight, true, 0);
  (void)input.feed(ButtonId::BottomRight, false, 30);
  (void)input.tick(60);
  (void)input.feed(ButtonId::BottomRight, true, 70);
  (void)input.tick(100);
  (void)input.feed(ButtonId::BottomRight, false, 110);
  const auto second_press = input.tick(140);
  (void)input.feed(ButtonId::BottomRight, true, 160);
  expect(has_event(second_press, ButtonEventKind::DoubleClick),
         "two clicks inside the timeout emit one double-click");
  const auto second_release = input.tick(500);
  expect(!has_event(second_release, ButtonEventKind::SingleClick),
         "the second release does not create a stray single-click event");
  const auto after_double = input.tick(800);
  expect(!has_event(after_double, ButtonEventKind::SingleClick),
         "a double-click leaves no delayed single-click timeout");

  (void)input.feed(ButtonId::UpperRight, true, 100);
  (void)input.tick(130);
  const auto hold = input.tick(640);
  expect(has_event(hold, ButtonEventKind::LongHoldStart),
         "upper-right hold emits a long-hold start");
  (void)input.feed(ButtonId::UpperRight, false, 650);
  const auto release = input.tick(680);
  expect(has_event(release, ButtonEventKind::LongHoldEnd),
         "upper-right release closes the long hold");
}

void test_simultaneous_button_channels() {
  ButtonInput input;
  (void)input.feed(ButtonId::BottomRight, true, 0);
  (void)input.feed(ButtonId::UpperRight, true, 0);
  (void)input.feed(ButtonId::BottomRight, false, 30);
  (void)input.feed(ButtonId::UpperRight, false, 30);
  const auto simultaneous_press = input.tick(60);
  expect(has_event(simultaneous_press, ButtonEventKind::Pressed),
         "independent debouncers preserve simultaneous button transitions");
  (void)input.feed(ButtonId::BottomRight, true, 70);
  (void)input.feed(ButtonId::UpperRight, true, 70);
  const auto simultaneous_release = input.tick(100);
  expect(has_event(simultaneous_release, ButtonEventKind::Released),
         "simultaneous releases are kept on their independent channels");
}

void test_axp_pkey_status_is_decoded_as_edges() {
  const auto pressed = decode_axp_pkey_status(kAxpPkeyPositiveBit);
  expect(pressed.pressed && !pressed.released,
         "AXP positive INTSTS2 bit becomes one PKEY press edge");
  const auto released = decode_axp_pkey_status(kAxpPkeyNegativeBit);
  expect(!released.pressed && released.released,
         "AXP negative INTSTS2 bit becomes one PKEY release edge");
  const auto both = decode_axp_pkey_status(kAxpPkeyStatusMask);
  expect(both.pressed && both.released,
         "coalesced AXP status preserves both explicit PKEY edges");
  const auto unrelated = decode_axp_pkey_status(0x04);
  expect(!unrelated.pressed && !unrelated.released,
         "non-PKEY AXP status bits do not synthesize input edges");
}

void test_lock_sleep_wake_and_prompt_suppression() {
  SleepLockController lock;
  ButtonEvent double_click{ButtonId::BottomRight, ButtonEventKind::DoubleClick, 1'000};
  auto asleep = lock.on_button(double_click);
  expect(asleep.state.locked && !asleep.state.display_on && !asleep.state.touch_enabled &&
             !asleep.state.voice_enabled,
         "bottom-right double-click locks and turns the display off");
  expect(asleep.state.transport_retained && !asleep.state.hard_power_off,
         "sleep retains Wi-Fi/session and never requests hard power-off");
  expect(has_effect(asleep.effects, LockEffectKind::DisplayOff),
         "locking emits a display-off effect");
  expect(has_effect(lock.on_touch(1'010).effects, LockEffectKind::TouchRejected) &&
             has_effect(lock.on_voice(1'011).effects, LockEffectKind::VoiceRejected),
         "locked touch and voice attempts are rejected");

  const auto prompt = lock.on_prompt_arrival(1'100);
  expect(prompt.state.locked && !prompt.state.display_on &&
             has_effect(prompt.effects, LockEffectKind::PromptSuppressed),
         "a prompt arriving while locked does not wake or unlock the device");
  const auto awake = lock.on_button(double_click);
  expect(!awake.state.locked && awake.state.display_on && awake.state.touch_enabled &&
             awake.state.voice_enabled,
         "bottom-right double-click wakes and unlocks");
  expect(has_effect(awake.effects, LockEffectKind::DisplayOn),
         "waking emits a display-on effect");

  (void)lock.on_button(double_click);
  const auto reboot = lock.on_reboot(2'000);
  expect(!reboot.state.locked && reboot.state.display_on && reboot.state.touch_enabled &&
             reboot.state.voice_enabled,
         "reboot initializes into unlocked ambient input");
}

void test_ptt_lifecycle_and_rejection() {
  PttButtonController ptt;
  ptt.set_context(PttContext{false, true, true, false});
  const ButtonEvent press{ButtonId::UpperRight, ButtonEventKind::Pressed, 10};
  const auto started = ptt.on_button(press);
  expect(has_effect(started, PttEffectKind::VoiceOverlayOpened) &&
             has_effect(started, PttEffectKind::CaptureStarted) && ptt.capturing(),
         "upper-right press starts capture and opens the voice overlay");
  const auto hold = ptt.on_button(
      ButtonEvent{ButtonId::UpperRight, ButtonEventKind::LongHoldStart, 500});
  expect(hold.empty(), "long-hold start does not start capture twice");
  const auto stopped = ptt.on_button(
      ButtonEvent{ButtonId::UpperRight, ButtonEventKind::Released, 900});
  expect(has_effect(stopped, PttEffectKind::CaptureStopped) &&
             has_effect(stopped, PttEffectKind::RequestTranscription) && !ptt.capturing(),
         "upper-right release stops capture and requests server transcription");

  ptt.set_context(PttContext{true, true, true, false});
  const auto locked = ptt.on_button(press);
  expect(has_effect(locked, PttEffectKind::RejectedLocked) && !ptt.capturing(),
         "locked voice input is rejected");
  ptt.set_context(PttContext{false, false, true, false});
  const auto offline = ptt.on_button(press);
  expect(has_effect(offline, PttEffectKind::RejectedUnavailable) && !ptt.capturing(),
         "voice input without a live session is rejected");

  ptt.set_context(PttContext{false, true, true, false});
  (void)ptt.on_button(press);
  const auto cancelled = ptt.on_disconnect(1'200);
  expect(has_effect(cancelled, PttEffectKind::CaptureCancelled) && !ptt.capturing(),
         "disconnect cancellation stops capture without submitting audio");
}

}  // namespace

int main() {
  test_debounce_and_single_timeout();
  test_double_click_and_long_hold();
  test_simultaneous_button_channels();
  test_axp_pkey_status_is_decoded_as_edges();
  test_lock_sleep_wake_and_prompt_suppression();
  test_ptt_lifecycle_and_rejection();
  if (failures != 0) {
    std::cerr << failures << " buttons/power test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "buttons/power tests passed\n";
  return EXIT_SUCCESS;
}
