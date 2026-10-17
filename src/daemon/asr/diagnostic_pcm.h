#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace vinput::daemon::asr {

// Preserve the existing float-to-int16 diagnostic conversion while allowing
// std::vector's amortized growth, rather than reserving exactly one chunk at a time.
inline void AppendDiagnosticPcm(std::vector<int16_t>& destination, std::span<const float> samples) {
  for (const float sample : samples) {
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    destination.push_back(static_cast<int16_t>(clamped * 32767.0f));
  }
}

} // namespace vinput::daemon::asr
