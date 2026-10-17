#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace vinput::audio {

// Buffered delivery keeps the authoritative PCM in AudioCapture until
// StopAndGetBuffer(); the callback only needs the running sample count.
inline void CountBufferedCaptureSamples(std::size_t* total, std::span<const int16_t> samples) {
  *total += samples.size();
}

} // namespace vinput::audio
