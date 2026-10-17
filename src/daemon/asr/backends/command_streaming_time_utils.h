#pragma once

#include <chrono>

namespace vinput::daemon::asr {

// Keep the clamp in the clock's duration domain; an int conversion before
// clamping can turn a large positive wait into an infinite poll timeout.
inline int RemainingPollMs(std::chrono::steady_clock::time_point deadline, int max_timeout_ms,
                           std::chrono::steady_clock::time_point now) {
  using Clock = std::chrono::steady_clock;
  if (max_timeout_ms <= 0) {
    return 0;
  }
  if (deadline == Clock::time_point::max()) {
    return max_timeout_ms;
  }
  if (deadline <= now) {
    return 0;
  }

  const auto remaining = deadline - now;
  if (remaining >= std::chrono::milliseconds(max_timeout_ms)) {
    return max_timeout_ms;
  }
  // At this point the remaining time fits within the nonnegative int bound.
  return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
}

} // namespace vinput::daemon::asr
