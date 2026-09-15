#include "reconnect.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace t3::companion {

Backoff::Backoff(BackoffPolicy policy) : policy_(policy) {
  if (policy_.initial_delay_ms == 0) policy_.initial_delay_ms = 1;
  if (policy_.max_delay_ms < policy_.initial_delay_ms) {
    policy_.max_delay_ms = policy_.initial_delay_ms;
  }
  if (!std::isfinite(policy_.multiplier) || policy_.multiplier < 1.0) {
    policy_.multiplier = 1.0;
  }
  policy_.jitter_percent = std::min<std::uint32_t>(policy_.jitter_percent, 100);
}

BackoffDelay Backoff::next(std::uint32_t jitter_sample) {
  jitter_sample = std::min<std::uint32_t>(jitter_sample, 100);
  const auto current_attempt = attempt_++;
  long double raw = static_cast<long double>(policy_.initial_delay_ms);
  for (std::uint32_t index = 0; index < current_attempt; ++index) {
    raw *= static_cast<long double>(policy_.multiplier);
    if (raw >= static_cast<long double>(policy_.max_delay_ms)) {
      raw = static_cast<long double>(policy_.max_delay_ms);
      break;
    }
  }
  const auto capped = std::min<long double>(raw, policy_.max_delay_ms);
  const long double spread = static_cast<long double>(policy_.jitter_percent) / 100.0L;
  const long double centered =
      (static_cast<long double>(jitter_sample) - 50.0L) / 50.0L;
  const long double factor = 1.0L + spread * centered;
  const long double jittered = std::max<long double>(1.0L, capped * factor);
  const auto delay = static_cast<std::uint64_t>(std::llround(jittered));
  return {std::min<std::uint64_t>(delay, policy_.max_delay_ms), current_attempt + 1};
}

void Backoff::reset() { attempt_ = 0; }

}  // namespace t3::companion
