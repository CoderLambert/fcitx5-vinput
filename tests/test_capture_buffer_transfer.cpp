#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "daemon/audio/captured_pcm_transfer.h"

namespace {
void Require(bool condition, const char* reason) {
  if (!condition) {
    std::cerr << "Captured PCM transfer regression: " << reason << '\n';
    std::abort();
  }
}

void TestTransferRetainsContentWithoutCopy() {
  std::vector<int16_t> capture{1000, -1000, 0, 32767, -32768};
  const auto* original = capture.data();
  auto handed_off = vinput::audio::TakeCapturedPcm(capture);
  Require(capture.empty(), "capture must be empty after transfer");
  Require(handed_off.data() == original, "no full-recording allocation or copy");
  Require(handed_off == std::vector<int16_t>({1000, -1000, 0, 32767, -32768}),
          "the PCM sequence must be identical");
}

void TestEmptyAndNextRecording() {
  std::vector<int16_t> capture;
  auto empty = vinput::audio::TakeCapturedPcm(capture);
  Require(empty.empty() && capture.empty(), "empty recording is supported");
  for (int iteration = 0; iteration < 5; ++iteration) {
    const std::array<int16_t, 3> next{
        static_cast<int16_t>(iteration), static_cast<int16_t>(iteration + 1), -32768};
    capture.insert(capture.end(), next.begin(), next.end());
    auto recording = vinput::audio::TakeCapturedPcm(capture);
    Require(capture.empty(), "next recording buffer stays independent");
    Require(recording == std::vector<int16_t>(next.begin(), next.end()),
            "previous recording never leaks into the next one");
  }
}

} // namespace

int main() {
  TestTransferRetainsContentWithoutCopy();
  TestEmptyAndNextRecording();
  std::cout << "Captured PCM transfer tests passed\n";
}
