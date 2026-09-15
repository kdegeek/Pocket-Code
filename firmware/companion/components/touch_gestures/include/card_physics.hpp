#pragma once

#include "touch_reader.hpp"

namespace t3::companion {

struct CardOffset {
  float x = 0.0F;
  float y = 0.0F;
};

struct CardPhysicsConfig {
  float max_offset = 180.0F;
  float spring_stiffness = 260.0F;
  float spring_damping = 32.0F;
  float settle_distance = 0.5F;
  float settle_velocity = 0.5F;
};

// Card motion is presentation-only.  GestureRecognizer owns action thresholds;
// this class only tracks the visual card offset and its reversible spring.
class CardPhysics {
 public:
  explicit CardPhysics(CardPhysicsConfig config = {});

  void begin_drag(TouchPoint point);
  void drag_to(TouchPoint point);
  void release();
  void cancel();
  void tick(float elapsed_ms);

  [[nodiscard]] const CardOffset& offset() const { return offset_; }
  [[nodiscard]] const CardPhysicsConfig& config() const { return config_; }
  [[nodiscard]] bool dragging() const { return dragging_; }
  [[nodiscard]] bool settling() const { return settling_; }
  [[nodiscard]] bool settled() const;
  [[nodiscard]] float progress() const;

 private:
  CardPhysicsConfig config_;
  CardOffset offset_;
  CardOffset velocity_;
  TouchPoint drag_start_;
  bool dragging_ = false;
  bool settling_ = false;
};

}  // namespace t3::companion
