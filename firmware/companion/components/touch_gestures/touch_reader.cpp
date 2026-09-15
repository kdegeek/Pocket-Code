#include "touch_reader.hpp"

#include <algorithm>

namespace t3::companion {
namespace {

int valid_extent(int extent, int fallback) {
  return extent > 0 ? extent : fallback;
}

int clamp_coordinate(int value, int extent) {
  return std::clamp(value, 0, valid_extent(extent, 1) - 1);
}

}  // namespace

TouchReader::TouchReader(TouchOrientation orientation) : orientation_(orientation) {
  orientation_.width = valid_extent(orientation_.width, kTouchDisplayWidth);
  orientation_.height = valid_extent(orientation_.height, kTouchDisplayHeight);
}

TouchPoint TouchReader::normalize(RawTouchPoint point) const {
  return normalize_touch_point(point, orientation_);
}

TouchSample TouchReader::normalize(const RawTouchSample& sample) const {
  TouchSample result;
  result.phase = sample.phase;
  result.point = normalize(sample.point);
  result.timestamp_ms = sample.timestamp_ms;
  return result;
}

TouchPoint normalize_touch_point(RawTouchPoint point, TouchOrientation orientation) {
  orientation.width = valid_extent(orientation.width, kTouchDisplayWidth);
  orientation.height = valid_extent(orientation.height, kTouchDisplayHeight);

  int x = clamp_coordinate(point.x, orientation.width);
  int y = clamp_coordinate(point.y, orientation.height);
  if (orientation.mirror_x) {
    x = orientation.width - 1 - x;
  }
  if (orientation.mirror_y) {
    y = orientation.height - 1 - y;
  }
  if (orientation.swap_xy) {
    std::swap(x, y);
  }
  return TouchPoint{x, y, point.pressure};
}

}  // namespace t3::companion
