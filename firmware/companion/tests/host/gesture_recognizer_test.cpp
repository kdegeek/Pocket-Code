#include "gesture_recognizer.hpp"
#include "touch_reader.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using namespace t3::companion;

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

TouchSample sample(TouchPhase phase, int x, int y, std::uint64_t timestamp_ms) {
  TouchSample result;
  result.phase = phase;
  result.point = TouchPoint{x, y, 1};
  result.timestamp_ms = timestamp_ms;
  return result;
}

std::optional<SemanticAction> swipe(GestureRecognizer& recognizer, int x0, int y0, int x1,
                                    int y1, std::uint64_t duration_ms) {
  (void)recognizer.feed(sample(TouchPhase::Down, x0, y0, 0));
  (void)recognizer.feed(sample(TouchPhase::Move, x1, y1, duration_ms / 2U));
  return recognizer.feed(sample(TouchPhase::Up, x1, y1, duration_ms));
}

void test_orientation_normalization() {
  TouchReader reader;
  const auto upper_left = reader.normalize(RawTouchPoint{0, 0, 12});
  const auto lower_right = reader.normalize(RawTouchPoint{465, 465, 12});
  expect(upper_left.x == 465 && upper_left.y == 465,
         "verified mirror X/Y maps raw upper-left to logical lower-right");
  expect(lower_right.x == 0 && lower_right.y == 0,
         "verified mirror X/Y maps raw lower-right to logical upper-left");
}

void test_noise_and_short_drag_are_ignored() {
  GestureRecognizer recognizer;
  recognizer.set_context(GestureContext{InteractionMode::Approval, false, false, false});
  expect(!swipe(recognizer, 120, 220, 126, 224, 120).has_value(),
         "tap noise emits no action");
  expect(!swipe(recognizer, 120, 220, 148, 223, 240).has_value(),
         "short horizontal drag below threshold emits no action");
}

void test_contextual_swipes_emit_once() {
  GestureRecognizer recognizer;
  recognizer.set_context(GestureContext{InteractionMode::Approval, false, false, false});
  auto action = swipe(recognizer, 120, 220, 360, 226, 180);
  expect(action == SemanticAction::Approve, "approval right swipe approves");
  expect(!recognizer.feed(sample(TouchPhase::Up, 361, 226, 200)).has_value(),
         "one contact cannot emit a second action");

  recognizer.set_context(GestureContext{InteractionMode::Approval, false, false, false});
  expect(swipe(recognizer, 340, 220, 90, 226, 180) == SemanticAction::Deny,
         "approval left swipe denies");
  expect(!swipe(recognizer, 220, 300, 224, 70, 180).has_value(),
         "approval up swipe is not an action");

  recognizer.set_context(GestureContext{InteractionMode::UserInput, false, true, false});
  expect(swipe(recognizer, 100, 220, 350, 226, 180) == SemanticAction::BrowseNext,
         "bounded choice right swipe browses next");
  expect(swipe(recognizer, 220, 300, 224, 80, 180) == SemanticAction::Select,
         "multi-select option up swipe toggles the centered option");

  recognizer.set_context(GestureContext{InteractionMode::UserInput, false, true, true});
  expect(swipe(recognizer, 220, 300, 224, 80, 180) == SemanticAction::Submit,
         "multi-select submit-card up swipe submits");

  recognizer.set_context(GestureContext{InteractionMode::Transcript, false, false, false});
  expect(swipe(recognizer, 100, 220, 350, 226, 180) == SemanticAction::Submit,
         "transcript right swipe submits for confirmation");
  expect(swipe(recognizer, 350, 220, 100, 226, 180) == SemanticAction::Discard,
         "transcript left swipe discards without forwarding");
}

void test_cancel_diagonal_edges_and_lock() {
  GestureRecognizer recognizer;
  recognizer.set_context(GestureContext{InteractionMode::Approval, false, false, false});
  expect(swipe(recognizer, 0, 230, 250, 235, 180) == SemanticAction::Approve,
         "swipe beginning at left edge remains usable");
  expect(swipe(recognizer, 460, 230, 215, 235, 180) == SemanticAction::Deny,
         "swipe beginning at right edge remains usable");
  expect(!swipe(recognizer, 220, 220, 350, 350, 180).has_value(),
         "diagonal ambiguity is rejected");
  expect(!swipe(recognizer, 220, 220, 350, 224, 1500).has_value(),
         "overlong contact is not treated as a swipe");
  expect(swipe(recognizer, 220, 50, 224, 300, 180) == SemanticAction::Cancel,
         "approval down swipe cancels the active turn");

  recognizer.set_context(GestureContext{InteractionMode::Approval, true, false, false});
  expect(!swipe(recognizer, 100, 220, 350, 224, 180).has_value(),
         "locked state rejects touch actions");
  (void)recognizer.feed(sample(TouchPhase::Down, 100, 220, 300));
  expect(!recognizer.feed(sample(TouchPhase::Cancel, 100, 220, 320)).has_value(),
         "cancelled contact emits no action");
}

void test_mapping_into_interaction_fsm() {
  expect(to_interaction_gesture(SemanticAction::Approve) == Gesture::SwipeRight,
         "approve action maps to FSM right swipe");
  expect(to_interaction_gesture(SemanticAction::Deny) == Gesture::SwipeLeft,
         "deny action maps to FSM left swipe");
  expect(to_interaction_gesture(SemanticAction::Cancel) == Gesture::SwipeDown,
         "cancel action maps to FSM down swipe");
  expect(to_interaction_gesture(SemanticAction::BrowsePrevious) == Gesture::BrowsePrevious,
         "browse previous action maps to FSM previous gesture");
  expect(!to_interaction_gesture(SemanticAction::None).has_value(),
         "no semantic action does not enter the FSM");
  const auto event = to_interaction_event(SemanticAction::Approve, InteractionMode::Approval, 42);
  expect(event.has_value() && event->kind == InteractionEventKind::Gesture &&
             event->gesture_value == Gesture::SwipeRight && event->now_ms == 42,
         "semantic action produces an FSM gesture event without sending a command");
}

}  // namespace

int main() {
  test_orientation_normalization();
  test_noise_and_short_drag_are_ignored();
  test_contextual_swipes_emit_once();
  test_cancel_diagonal_edges_and_lock();
  test_mapping_into_interaction_fsm();
  if (failures != 0) {
    std::cerr << failures << " gesture test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "gesture recognizer tests passed\n";
  return EXIT_SUCCESS;
}
