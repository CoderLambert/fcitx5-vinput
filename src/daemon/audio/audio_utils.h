#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace vinput::audio {

// Peak-normalize samples in-place. Only amplifies (never attenuates).
// If the peak amplitude is below `low_threshold`, scales so peak == `target_peak`.
void PeakNormalize(std::vector<float>& samples, float target_peak = 1.0f,
                   float low_threshold = 0.1f);

// Apply a fixed input gain in-place. Samples are clamped to [-1, 1].
void ApplyGain(std::vector<float>& samples, float gain);

// Apply a fixed input gain to int16 samples in-place. Samples are clamped to [-32768, 32767].
void ApplyGainI16(std::vector<int16_t>& samples, float gain);

// Gain==1 returns the borrowed input with no allocation. Otherwise, the
// returned span refers to the caller-owned scratch buffer and is valid only
// until the scratch buffer is modified.
std::span<const int16_t> PrepareGainAdjustedInput(std::span<const int16_t> pcm, float gain,
                                                  std::vector<int16_t>* scratch);

} // namespace vinput::audio
