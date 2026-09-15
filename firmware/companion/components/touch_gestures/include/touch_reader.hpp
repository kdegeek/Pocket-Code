#pragma once

#include <cstdint>

namespace t3::companion {

inline constexpr int kTouchDisplayWidth = 466;
inline constexpr int kTouchDisplayHeight = 466;

enum class TouchPhase {
  Down,
  Move,
  Up,
  Cancel,
};

struct RawTouchPoint {
  int x = 0;
  int y = 0;
  std::uint16_t pressure = 0;
};

struct TouchPoint {
  int x = 0;
  int y = 0;
  std::uint16_t pressure = 0;
};

struct RawTouchSample {
  TouchPhase phase = TouchPhase::Cancel;
  RawTouchPoint point;
  std::uint64_t timestamp_ms = 0;
};

struct TouchSample {
  TouchPhase phase = TouchPhase::Cancel;
  TouchPoint point;
  std::uint64_t timestamp_ms = 0;
};

// These defaults are the Task 2 verified CST9217 calibration: logical upright
// 466x466 coordinates, mirror X/Y, and no axis swap.
struct TouchOrientation {
  int width = kTouchDisplayWidth;
  int height = kTouchDisplayHeight;
  bool mirror_x = true;
  bool mirror_y = true;
  bool swap_xy = false;
};

class TouchReader {
 public:
  explicit TouchReader(TouchOrientation orientation = {});

  [[nodiscard]] TouchPoint normalize(RawTouchPoint point) const;
  [[nodiscard]] TouchSample normalize(const RawTouchSample& sample) const;
  [[nodiscard]] const TouchOrientation& orientation() const { return orientation_; }

 private:
  TouchOrientation orientation_;
};

[[nodiscard]] TouchPoint normalize_touch_point(RawTouchPoint point,
                                               TouchOrientation orientation = {});

}  // namespace t3::companion
