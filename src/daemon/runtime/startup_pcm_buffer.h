#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "daemon/audio/audio_utils.h"

namespace vinput::daemon::runtime {

// Access is serialized by DaemonRuntimeController::state_mutex_.
// Preserves the mic samples arriving during ASR session initialization.
class StartupPcmBuffer {
public:
  void Append(std::span<const int16_t> pcm) {
    samples_.insert(samples_.end(), pcm.begin(), pcm.end());
  }

  std::vector<int16_t> TakeWithGain(float gain) {
    std::vector<int16_t> result;
    result.swap(samples_);
    vinput::audio::ApplyGainI16(result, gain);
    return result;
  }

  void Clear() { samples_.clear(); }

private:
  std::vector<int16_t> samples_;
};

} // namespace vinput::daemon::runtime
