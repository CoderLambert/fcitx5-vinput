#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "daemon/runtime/startup_pcm_buffer.h"

namespace {

void ExpectSamples(const std::vector<int16_t>& got, const std::vector<int16_t>& expected) {
  if (got != expected) {
    std::cerr << "Startup audio samples out of order or duplicated\n";
    std::abort();
  }
}

void TestStartupPrefixBeforeLiveSamples() {
  vinput::daemon::runtime::StartupPcmBuffer buffer;
  const std::array<int16_t, 3> first{1000, 2000, 3000};
  const std::array<int16_t, 2> second{4000, 5000};
  buffer.Append(first);
  buffer.Append(second);

  auto pending = buffer.TakeWithGain(1.0F);
  const std::array<int16_t, 2> live{6000, 7000};
  pending.insert(pending.end(), live.begin(), live.end());
  ExpectSamples(pending, {1000, 2000, 3000, 4000, 5000, 6000, 7000});
  ExpectSamples(buffer.TakeWithGain(1.0F), {});
}

void TestPrefixGainedOnceAndCancelDoesNotLeak() {
  vinput::daemon::runtime::StartupPcmBuffer buffer;
  const std::array<int16_t, 3> prefix{1000, -1000, 20000};
  buffer.Append(prefix);
  ExpectSamples(buffer.TakeWithGain(2.0F), {2000, -2000, 32767});

  const std::array<int16_t, 2> canceled{1, 2};
  buffer.Append(canceled);
  buffer.Clear();
  ExpectSamples(buffer.TakeWithGain(1.0F), {});

  const std::array<int16_t, 1> next{300};
  buffer.Append(next);
  ExpectSamples(buffer.TakeWithGain(0.5F), {150});
}

} // namespace

int main() {
  TestStartupPrefixBeforeLiveSamples();
  TestPrefixGainedOnceAndCancelDoesNotLeak();
  std::cout << "Startup PCM order tests passed\n";
}
