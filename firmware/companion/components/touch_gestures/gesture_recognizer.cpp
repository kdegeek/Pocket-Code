#include "gesture_recognizer.hpp"

#include <algorithm>
#include <cstdlib>

namespace t3::companion {
namespace {

int absolute(int value) { return value < 0 ? -value : value; }

bool dominant(int primary, int perpendicular, const GestureRecognizerConfig& config) {
  return primary * config.dominant_axis_denominator >=
         perpendicular * config.dominant_axis_numerator;
}

std::optional<SemanticAction> horizontal_action(const GestureContext& context, bool right) {
  switch (context.mode) {
    case InteractionMode::Ambient:
      return right ? SemanticAction::BrowseNext : SemanticAction::BrowsePrevious;
    case InteractionMode::Approval:
      return right ? SemanticAction::Approve : SemanticAction::Deny;
    case InteractionMode::UserInput:
      return right ? SemanticAction::BrowseNext : SemanticAction::BrowsePrevious;
    case InteractionMode::Transcript:
      return right ? SemanticAction::Submit : SemanticAction::Discard;
    case InteractionMode::AwaitingAck:
    case InteractionMode::AppliedGlow:
    case InteractionMode::ErrorGlow:
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<SemanticAction> vertical_action(const GestureContext& context, bool up) {
  if (up) {
    if (context.mode != InteractionMode::UserInput) {
      return std::nullopt;
    }
    if (context.multi_select && !context.submit_card) {
      return SemanticAction::Select;
    }
    return SemanticAction::Submit;
  }
  switch (context.mode) {
    case InteractionMode::Approval:
    case InteractionMode::UserInput:
    case InteractionMode::Transcript:
      return SemanticAction::Cancel;
    case InteractionMode::Ambient:
    case InteractionMode::AwaitingAck:
    case InteractionMode::AppliedGlow:
    case InteractionMode::ErrorGlow:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace

GestureRecognizer::GestureRecognizer(GestureRecognizerConfig config,
                                     TouchOrientation orientation)
    : config_(config), reader_(orientation) {
  config_.minimum_distance = std::max(config_.minimum_distance, 1);
  config_.maximum_duration_ms = std::max<std::uint64_t>(config_.maximum_duration_ms, 1);
  config_.dominant_axis_numerator = std::max(config_.dominant_axis_numerator, 1);
  config_.dominant_axis_denominator = std::max(config_.dominant_axis_denominator, 1);
}

void GestureRecognizer::set_context(GestureContext context) { context_ = context; }

std::optional<SemanticAction> GestureRecognizer::feed(const RawTouchSample& sample) {
  return feed(reader_.normalize(sample));
}

std::optional<SemanticAction> GestureRecognizer::feed(const TouchSample& sample) {
  switch (sample.phase) {
    case TouchPhase::Down:
      tracking_ = !context_.locked;
      contact_context_ = context_;
      start_point_ = sample.point;
      current_point_ = sample.point;
      start_timestamp_ms_ = sample.timestamp_ms;
      return std::nullopt;
    case TouchPhase::Move:
      if (tracking_) {
        current_point_ = sample.point;
      }
      return std::nullopt;
    case TouchPhase::Cancel:
      reset();
      return std::nullopt;
    case TouchPhase::Up:
      if (!tracking_) {
        reset();
        return std::nullopt;
      }
      return finish(sample);
  }
  return std::nullopt;
}

std::optional<SemanticAction> GestureRecognizer::finish(const TouchSample& sample) {
  current_point_ = sample.point;
  const std::uint64_t duration = sample.timestamp_ms >= start_timestamp_ms_
                                     ? sample.timestamp_ms - start_timestamp_ms_
                                     : config_.maximum_duration_ms + 1U;
  const int dx = current_point_.x - start_point_.x;
  const int dy = current_point_.y - start_point_.y;
  const int absolute_x = absolute(dx);
  const int absolute_y = absolute(dy);
  const int minimum_distance = config_.minimum_distance;
  std::optional<SemanticAction> action;

  if (!contact_context_.locked && duration <= config_.maximum_duration_ms &&
      (absolute_x * absolute_x + absolute_y * absolute_y) >=
          minimum_distance * minimum_distance) {
    if (dominant(absolute_x, absolute_y, config_)) {
      action = horizontal_action(contact_context_, dx > 0);
    } else if (dominant(absolute_y, absolute_x, config_)) {
      action = vertical_action(contact_context_, dy < 0);
    }
  }
  reset();
  return action;
}

void GestureRecognizer::reset() {
  tracking_ = false;
  start_point_ = TouchPoint{};
  current_point_ = TouchPoint{};
  start_timestamp_ms_ = 0;
  contact_context_ = context_;
}

std::optional<Gesture> to_interaction_gesture(SemanticAction action) {
  switch (action) {
    case SemanticAction::BrowsePrevious:
      return Gesture::BrowsePrevious;
    case SemanticAction::BrowseNext:
      return Gesture::BrowseNext;
    case SemanticAction::Approve:
      return Gesture::SwipeRight;
    case SemanticAction::Deny:
    case SemanticAction::Discard:
      return Gesture::SwipeLeft;
    case SemanticAction::Cancel:
      return Gesture::SwipeDown;
    case SemanticAction::Select:
    case SemanticAction::Submit:
      return Gesture::SwipeUp;
    case SemanticAction::None:
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<Gesture> to_interaction_gesture(SemanticAction action, InteractionMode mode) {
  if (action == SemanticAction::Submit && mode == InteractionMode::Transcript) {
    return Gesture::SwipeRight;
  }
  return to_interaction_gesture(action);
}

std::optional<InteractionEvent> to_interaction_event(SemanticAction action,
                                                      InteractionMode mode,
                                                      std::uint64_t now_ms) {
  const auto gesture = to_interaction_gesture(action, mode);
  if (!gesture.has_value()) {
    return std::nullopt;
  }
  return InteractionEvent::gesture(*gesture, now_ms);
}

}  // namespace t3::companion
