#pragma once

#include "interaction_fsm.hpp"
#include "touch_reader.hpp"

#include <cstdint>
#include <optional>

namespace t3::companion {

// Semantic actions are deliberately transport-free.  The reducer remains the
// only layer that creates or sends commands.
enum class SemanticAction {
  None,
  BrowsePrevious,
  BrowseNext,
  Approve,
  Deny,
  Cancel,
  Select,
  Submit,
  Discard,
};

using GestureAction = SemanticAction;

struct GestureContext {
  InteractionMode mode = InteractionMode::Ambient;
  bool locked = false;
  bool multi_select = false;
  bool submit_card = false;
};

struct GestureRecognizerConfig {
  int minimum_distance = 48;
  std::uint64_t maximum_duration_ms = 1000;
  // A direction must be at least 1.25x stronger than its perpendicular
  // component.  Keeping this as integer ratios avoids platform float drift.
  int dominant_axis_numerator = 5;
  int dominant_axis_denominator = 4;
};

class GestureRecognizer {
 public:
  explicit GestureRecognizer(GestureRecognizerConfig config = {},
                             TouchOrientation orientation = {});

  void set_context(GestureContext context);
  [[nodiscard]] const GestureContext& context() const { return context_; }
  [[nodiscard]] std::optional<SemanticAction> feed(const TouchSample& sample);
  [[nodiscard]] std::optional<SemanticAction> feed(const RawTouchSample& sample);
  void reset();

 private:
  [[nodiscard]] std::optional<SemanticAction> finish(const TouchSample& sample);

  GestureRecognizerConfig config_;
  TouchReader reader_;
  GestureContext context_;
  GestureContext contact_context_;
  TouchPoint start_point_;
  TouchPoint current_point_;
  std::uint64_t start_timestamp_ms_ = 0;
  bool tracking_ = false;
};

[[nodiscard]] std::optional<Gesture> to_interaction_gesture(SemanticAction action);
[[nodiscard]] std::optional<Gesture> to_interaction_gesture(SemanticAction action,
                                                            InteractionMode mode);
[[nodiscard]] std::optional<InteractionEvent> to_interaction_event(SemanticAction action,
                                                                    InteractionMode mode,
                                                                    std::uint64_t now_ms);

}  // namespace t3::companion
