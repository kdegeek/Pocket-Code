#include "card_physics.hpp"

#include <algorithm>
#include <cmath>

namespace t3::companion {
namespace {

float clamp_offset(float value, float limit) {
  return std::clamp(value, -std::abs(limit), std::abs(limit));
}

float magnitude(CardOffset value) { return std::sqrt(value.x * value.x + value.y * value.y); }

}  // namespace

CardPhysics::CardPhysics(CardPhysicsConfig config) : config_(config) {
  config_.max_offset = std::max(std::abs(config_.max_offset), 1.0F);
  config_.spring_stiffness = std::max(config_.spring_stiffness, 1.0F);
  config_.spring_damping = std::max(config_.spring_damping, 1.0F);
  config_.settle_distance = std::max(config_.settle_distance, 0.01F);
  config_.settle_velocity = std::max(config_.settle_velocity, 0.01F);
}

void CardPhysics::begin_drag(TouchPoint point) {
  drag_start_ = point;
  dragging_ = true;
  settling_ = false;
  velocity_ = CardOffset{};
}

void CardPhysics::drag_to(TouchPoint point) {
  if (!dragging_) {
    return;
  }
  offset_.x = clamp_offset(static_cast<float>(point.x - drag_start_.x), config_.max_offset);
  offset_.y = clamp_offset(static_cast<float>(point.y - drag_start_.y), config_.max_offset);
}

void CardPhysics::release() {
  if (!dragging_) {
    return;
  }
  dragging_ = false;
  settling_ = !settled();
}

void CardPhysics::cancel() {
  dragging_ = false;
  settling_ = false;
  offset_ = CardOffset{};
  velocity_ = CardOffset{};
}

void CardPhysics::tick(float elapsed_ms) {
  if (dragging_ || !settling_) {
    return;
  }
  const float seconds = std::clamp(elapsed_ms, 0.0F, 100.0F) / 1000.0F;
  if (seconds <= 0.0F) {
    return;
  }
  const float acceleration_x = -config_.spring_stiffness * offset_.x -
                               config_.spring_damping * velocity_.x;
  const float acceleration_y = -config_.spring_stiffness * offset_.y -
                               config_.spring_damping * velocity_.y;
  velocity_.x += acceleration_x * seconds;
  velocity_.y += acceleration_y * seconds;
  offset_.x += velocity_.x * seconds;
  offset_.y += velocity_.y * seconds;
  offset_.x = clamp_offset(offset_.x, config_.max_offset);
  offset_.y = clamp_offset(offset_.y, config_.max_offset);
  if (settled()) {
    offset_ = CardOffset{};
    velocity_ = CardOffset{};
    settling_ = false;
  }
}

bool CardPhysics::settled() const {
  return magnitude(offset_) <= config_.settle_distance &&
         magnitude(velocity_) <= config_.settle_velocity;
}

float CardPhysics::progress() const {
  return std::clamp(magnitude(offset_) / config_.max_offset, 0.0F, 1.0F);
}

}  // namespace t3::companion
