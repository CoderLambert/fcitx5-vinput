#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "daemon/audio/buffered_capture_counter.h"

namespace {
void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Buffered capture counter regression: " << message << '\n';
    std::abort();
  }
}
} // namespace

int main() {
  constexpr std::array<int16_t, 4> chunk{123, -23, 40, -40};
  std::size_t count = 0;
  for (int i = 0; i < 10000; ++i) {
    vinput::audio::CountBufferedCaptureSamples(&count, chunk);
  }
  Expect(count == 40000, "sample count accumulates per chunk without storing PCM");
  vinput::audio::CountBufferedCaptureSamples(&count, {});
  Expect(count == 40000, "empty chunk does not change count");
  count = 0;
  vinput::audio::CountBufferedCaptureSamples(&count, chunk);
  Expect(count == 4, "new session count starts from zero");
  std::cout << "Buffered capture counter tests passed\n";
}
