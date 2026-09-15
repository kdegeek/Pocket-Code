#pragma once

#include <cstdint>

namespace t3::companion {

struct BackoffPolicy {
  std::uint64_t initial_delay_ms = 250;
  std::uint64_t max_delay_ms = 30'000;
  double multiplier = 2.0;
  std::uint32_t jitter_percent = 20;
};

struct BackoffDelay {
  std::uint64_t delay_ms = 0;
  std::uint32_t attempt = 0;
};

/** Deterministic, injectable exponential reconnect delay with bounded jitter. */
class Backoff {
 public:
  explicit Backoff(BackoffPolicy policy = {});

  /**
   * Return the next delay. `jitter_sample` is a 0..100 sample supplied by a
   * platform RNG; 50 means no jitter and is the deterministic host default.
   */
  [[nodiscard]] BackoffDelay next(std::uint32_t jitter_sample = 50);
  void reset();
  [[nodiscard]] std::uint32_t attempt() const { return attempt_; }
  [[nodiscard]] const BackoffPolicy& policy() const { return policy_; }

 private:
  BackoffPolicy policy_;
  std::uint32_t attempt_ = 0;
};

}  // namespace t3::companion
