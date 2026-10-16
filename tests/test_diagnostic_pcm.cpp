#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

#include "daemon/asr/diagnostic_pcm.h"

namespace {

void Check(bool condition, const char* description) {
  if (!condition) {
    std::cerr << "Diagnostic PCM test failed: " << description << '\n';
    std::abort();
  }
}

void TestConversionAndEmptyInput() {
  const std::array<float, 7> input = {-2.0f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 2.0f};
  const std::array<int16_t, 7> expected = {-32767, -32767, -16383, 0, 16383, 32767, 32767};
  std::vector<int16_t> output;
  vinput::daemon::asr::AppendDiagnosticPcm(output, input);
  Check(output.size() == expected.size(), "conversion preserves sample count");
  for (std::size_t i = 0; i < input.size(); ++i) {
    Check(output[i] == expected[i], "conversion preserves existing rounding and clipping");
  }
  vinput::daemon::asr::AppendDiagnosticPcm(output, {});
  Check(output.size() == expected.size(), "empty input leaves the buffer unchanged");
}

void TestRepeatedChunks(int audio_seconds) {
  constexpr std::size_t kSampleRate = 16000;
  constexpr std::size_t kChunkSamples = 800;
  std::array<float, kChunkSamples> chunk{};
  for (std::size_t i = 0; i < chunk.size(); ++i) {
    chunk[i] = static_cast<float>(static_cast<int>(i % 201) - 100) / 100.0f;
  }

  std::vector<int16_t> output;
  std::size_t growth_events = 0;
  const auto chunk_count = static_cast<std::size_t>(audio_seconds) * kSampleRate / kChunkSamples;
  for (std::size_t n = 0; n < chunk_count; ++n) {
    const std::size_t previous_capacity = output.capacity();
    vinput::daemon::asr::AppendDiagnosticPcm(output, chunk);
    growth_events += output.capacity() != previous_capacity ? 1 : 0;
  }
  Check(output.size() == chunk_count * kChunkSamples, "no samples dropped in repeated chunks");
  for (std::size_t i = 0; i < output.size(); ++i) {
    const auto sample = chunk[i % kChunkSamples];
    const auto expected = static_cast<int16_t>(sample * 32767.0f);
    Check(output[i] == expected, "repeated chunks preserve every sample");
  }
  Check(growth_events < chunk_count / 4, "capacity does not grow on nearly every chunk");
  std::cout << audio_seconds << " seconds equivalent PCM: " << growth_events
            << " growth events over " << chunk_count << " chunks\n";
}

void TestLargeChunkAndReset() {
  std::vector<int16_t> output;
  const std::vector<float> large_chunk(16000, 0.25f);
  vinput::daemon::asr::AppendDiagnosticPcm(output, large_chunk);
  Check(output.size() == large_chunk.size(), "large chunk preserves length");
  Check(output.front() == 8191 && output.back() == 8191, "large chunk preserves conversion");
  output.clear(); // The session's cancel/destructor lifecycle may release its vector.
  vinput::daemon::asr::AppendDiagnosticPcm(output, std::span<const float>(large_chunk).first(1));
  Check(output.size() == 1 && output.front() == 8191, "new session data is not stale");
}

} // namespace

int main() {
  TestConversionAndEmptyInput();
  TestLargeChunkAndReset();
  TestRepeatedChunks(10);
  TestRepeatedChunks(60);
  TestRepeatedChunks(300);
  std::cout << "Diagnostic PCM tests passed\n";
}
