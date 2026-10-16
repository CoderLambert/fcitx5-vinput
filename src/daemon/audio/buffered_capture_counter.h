#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace vinput::audio {

// Preserve the real-time sample count without retaining a second full copy.
inline void CountBufferedCaptureSamples(std::size_t* total, std::span<const int16_t> samples) {
  *total += samples.size();
}

} // namespace vinput::audio
