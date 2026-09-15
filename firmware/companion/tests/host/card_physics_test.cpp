#include "card_physics.hpp"

#include <cmath>
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

void test_drag_is_independent_from_action_thresholds() {
  CardPhysics physics;
  physics.begin_drag(TouchPoint{120, 220, 1});
  physics.drag_to(TouchPoint{360, 220, 1});
  expect(physics.dragging(), "card physics tracks an active drag");
  expect(physics.offset().x > 0.0F, "card follows horizontal drag direction");
  expect(physics.progress() > 0.0F && physics.progress() <= 1.0F,
         "card progress is bounded independently of recognizer thresholds");
  physics.drag_to(TouchPoint{1000, 220, 1});
  expect(std::fabs(physics.offset().x) <= physics.config().max_offset + 0.01F,
         "card offset is clamped to physical travel");
}

void test_release_springs_to_center() {
  CardPhysics physics;
  physics.begin_drag(TouchPoint{220, 220, 1});
  physics.drag_to(TouchPoint{340, 220, 1});
  physics.release();
  expect(physics.settling(), "release starts a spring settle");
  for (int index = 0; index < 120; ++index) {
    physics.tick(16.0F);
  }
  expect(physics.settled(), "released card eventually settles");
  expect(std::fabs(physics.offset().x) < 0.5F && std::fabs(physics.offset().y) < 0.5F,
         "released card returns to center");
}

void test_cancel_resets_without_animation() {
  CardPhysics physics;
  physics.begin_drag(TouchPoint{220, 220, 1});
  physics.drag_to(TouchPoint{220, 80, 1});
  physics.cancel();
  expect(!physics.dragging() && physics.settled(), "cancel clears the drag immediately");
  expect(std::fabs(physics.offset().x) < 0.01F && std::fabs(physics.offset().y) < 0.01F,
         "cancel restores the centered card");
}

}  // namespace

int main() {
  test_drag_is_independent_from_action_thresholds();
  test_release_springs_to_center();
  test_cancel_resets_without_animation();
  if (failures != 0) {
    std::cerr << failures << " card physics test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "card physics tests passed\n";
  return EXIT_SUCCESS;
}
