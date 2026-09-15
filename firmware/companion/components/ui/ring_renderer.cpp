#include "ring_renderer.hpp"

#include <algorithm>
#include <cmath>

namespace t3::ui {
namespace {

constexpr Color kTrack{0x1B, 0x24, 0x30, 0xFF};
constexpr Color kCodex{0x58, 0xA6, 0xFF, 0xFF};
constexpr Color kClaude{0xE8, 0x89, 0x62, 0xFF};
constexpr Color kXai{0xF4, 0xF6, 0xF8, 0xFF};

ProviderRingValues empty_values() {
  return ProviderRingValues{};
}

ProviderRingValues values_for(const UsageSnapshot& usage, Provider provider) {
  ProviderRingValues result = empty_values();
  for (std::size_t provider_index = 0; provider_index < usage.providers.count;
       ++provider_index) {
    const UsageProvider& source = usage.providers[provider_index];
    if (source.provider != provider) {
      continue;
    }
    const bool usable = source.status == UsageStatus::Available;
    for (std::size_t window_index = 0; window_index < source.windows.count; ++window_index) {
      const RateWindow& window = source.windows[window_index];
      UsageValue* target = window.period == RateWindow::Period::Weekly
                               ? &result.weekly
                               : &result.five_hour;
      if (target->available || target->stale) {
        continue;
      }
      if (!std::isfinite(window.used_percent)) {
        target->stale = source.status == UsageStatus::Stale;
        continue;
      }
      target->percent = std::clamp(window.used_percent, 0.0, 100.0);
      target->stale = source.status == UsageStatus::Stale;
      target->available = usable;
    }
    if (!usable && source.status == UsageStatus::Stale) {
      result.weekly.stale = true;
      result.five_hour.stale = true;
      result.weekly.available = false;
      result.five_hour.available = false;
    }
    break;
  }
  return result;
}

}  // namespace

std::string UsageValue::display_text() const {
  if (!available || stale) {
    return "--";
  }
  const auto rounded = static_cast<int>(std::lround(std::clamp(percent, 0.0, 100.0)));
  return std::to_string(rounded) + "%";
}

std::array<RingGeometry, 3> ring_geometry() {
  return {
      RingGeometry{Provider::Codex, kOuterRingRadius, kRingStroke, kCodex, kTrack},
      RingGeometry{Provider::Claude, kMiddleRingRadius, kRingStroke, kClaude, kTrack},
      RingGeometry{Provider::Xai, kInnerRingRadius, kRingStroke, kXai, kTrack},
  };
}

RingValues make_ring_values(const UsageSnapshot& usage) {
  RingValues result;
  result.codex = values_for(usage, Provider::Codex);
  result.claude = values_for(usage, Provider::Claude);
  result.xai = values_for(usage, Provider::Xai);
  return result;
}

FlushRect round_flush_rect(int x_start, int y_start, int x_end, int y_end) {
  const int bounded_x_start = std::clamp(x_start, 0, static_cast<int>(kDisplayWidth) - 1);
  const int bounded_y_start = std::clamp(y_start, 0, static_cast<int>(kDisplayHeight) - 1);
  const int bounded_x_end = std::clamp(x_end, 0, static_cast<int>(kDisplayWidth) - 1);
  const int bounded_y_end = std::clamp(y_end, 0, static_cast<int>(kDisplayHeight) - 1);
  const int even_x_start = bounded_x_start & ~1;
  const int even_y_start = bounded_y_start & ~1;
  const int odd_x_end = std::min(bounded_x_end | 1, static_cast<int>(kDisplayWidth) - 1);
  const int odd_y_end = std::min(bounded_y_end | 1, static_cast<int>(kDisplayHeight) - 1);
  return FlushRect{static_cast<std::uint16_t>(even_x_start),
                   static_cast<std::uint16_t>(even_y_start),
                   static_cast<std::uint16_t>(odd_x_end),
                   static_cast<std::uint16_t>(odd_y_end)};
}

}  // namespace t3::ui
