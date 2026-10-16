#pragma once

#include <cstdint>
#include <vector>

namespace vinput::audio {

// Transfer the recording buffer without copying its samples. The original
// vector becomes empty and ready for the next BeginRecording cycle.
inline std::vector<int16_t> TakeCapturedPcm(std::vector<int16_t>& recording) {
  std::vector<int16_t> result;
  result.swap(recording);
  return result;
}

} // namespace vinput::audio
