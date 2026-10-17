#include <array>
#include <cstdlib>
#include <iostream>

#include "daemon/asr/streaming_diagnostic_policy.h"

namespace {
void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Streaming audio diagnostic regression: " << message << '\n';
    std::abort();
  }
}

void TestOptIn() {
  using vinput::daemon::asr::StreamingAudioDiagnosticsEnabled;
  Expect(!StreamingAudioDiagnosticsEnabled(nullptr), "unset opt-in disables PCM storage");
  Expect(!StreamingAudioDiagnosticsEnabled(""), "empty opt-in disables PCM storage");
  Expect(!StreamingAudioDiagnosticsEnabled("0"), "zero disables PCM storage");
  Expect(!StreamingAudioDiagnosticsEnabled("true"), "only an explicit 1 opts in");
  Expect(StreamingAudioDiagnosticsEnabled("1"), "explicit 1 enables diagnostic PCM");
}

void TestBoundedPrefix() {
  using vinput::daemon::asr::DiagnosticSamplePrefix;
  constexpr std::array<float, 6> samples{0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F};
  const auto first = DiagnosticSamplePrefix(0, samples, 4);
  Expect(first.size() == 4, "large incoming chunk is truncated to budget");
  Expect(*first.begin() == samples.at(0) && *(first.begin() + 3) == samples.at(3),
         "head samples stay ordered");
  const auto tail = DiagnosticSamplePrefix(3, samples, 4);
  Expect(tail.size() == 1 && *tail.begin() == samples.at(0), "remaining quota is respected");
  Expect(DiagnosticSamplePrefix(4, samples, 4).empty(), "at-capacity blocks more samples");
  Expect(DiagnosticSamplePrefix(99, samples, 4).empty(), "over-capacity remains bounded");
  Expect(DiagnosticSamplePrefix(0, {}).empty(), "empty sample is safe");
}
} // namespace

int main() {
  TestOptIn();
  TestBoundedPrefix();
  std::cout << "Streaming diagnostic policy tests passed\n";
}
