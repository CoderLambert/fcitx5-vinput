#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

#include "daemon/audio/audio_utils.h"

namespace {

void Require(bool condition, const char* name) {
  if (!condition) {
    std::cerr << "Input gain scratch regression: " << name << '\n';
    std::abort();
  }
}

void TestUnityGainBorrowsOriginalInput() {
  const std::array<int16_t, 4> input{1000, -1000, 20000, -20000};
  std::vector<int16_t> scratch;
  const auto view = vinput::audio::PrepareGainAdjustedInput(input, 1.0f, &scratch);
  Require(view.data() == input.data(), "unity gain must use original PCM pointer");
  Require(view.size() == input.size(), "unity gain length");
  Require(scratch.empty(), "unity gain must not allocate scratch PCM");
}

void TestNonUnityGainUsesScratch() {
  const std::array<int16_t, 4> input{1000, -1000, 20000, -20000};
  std::vector<int16_t> scratch;
  const auto view = vinput::audio::PrepareGainAdjustedInput(input, 2.0f, &scratch);
  Require(view.data() == scratch.data(), "non-unity gain must own scratch samples");
  Require(input[0] == 1000, "raw PCM remains unchanged");
  const std::array<int16_t, 4> expected{2000, -2000, 32767, -32768};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    Require(view[i] == expected[i], "gain clipping and sample order unchanged");
  }
}

void TestEmptyAndFractional() {
  const std::vector<int16_t> empty;
  std::vector<int16_t> scratch;
  const auto borrowed = vinput::audio::PrepareGainAdjustedInput(empty, 0.5f, &scratch);
  Require(borrowed.empty() && scratch.empty(), "empty input remains empty");
  const std::array<int16_t, 3> input{1001, -1001, 32767};
  const auto view = vinput::audio::PrepareGainAdjustedInput(input, 0.5f, &scratch);
  Require(view[0] == 500 && view[1] == -500 && view[2] == 16383,
          "fractional gain matches previous truncation semantics");
}

} // namespace

int main() {
  TestUnityGainBorrowsOriginalInput();
  TestNonUnityGainUsesScratch();
  TestEmptyAndFractional();
  std::cout << "Unity input gain scratch tests passed\n";
}
