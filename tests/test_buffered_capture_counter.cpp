#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "daemon/audio/buffered_capture_counter.h"

namespace {
void Require(bool condition, const char* what) {
  if (!condition) {
    std::cerr << "Buffered recording regression: " << what << '\n';
    std::abort();
  }
}

void TestStreamingCallbackCountsWithoutBufferStorage() {
  constexpr std::array<int16_t, 4> chunk{123, -23, 40, -40};
  std::size_t count = 0;
  const auto* chunk_address = chunk.data();
  for (int i = 0; i < 10000; ++i) {
    vinput::audio::CountBufferedCaptureSamples(&count, chunk);
  }
  Require(count == 40000, "callback sample count unchanged");
  Require(chunk.data() == chunk_address, "callback sample data remains borrowed");
  vinput::audio::CountBufferedCaptureSamples(&count, {});
  Require(count == 40000, "empty callback does not change count");
  count = 0; // New session reset.
  vinput::audio::CountBufferedCaptureSamples(&count, chunk);
  Require(count == 4, "new session begins from zero");
}

} // namespace

int main() {
  TestStreamingCallbackCountsWithoutBufferStorage();
  std::cout << "Buffered recording accounting passed\n";
}
