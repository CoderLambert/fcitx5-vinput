#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

namespace vinput::daemon::asr {

inline constexpr std::size_t kMaxStreamingDiagnosticSamples = 16000 * 60;

inline bool StreamingAudioDiagnosticsEnabled(const char* opt_in) {
  return opt_in != nullptr && std::string_view(opt_in) == "1";
}

inline std::span<const float> DiagnosticSamplePrefix(std::size_t stored,
                                                      std::span<const float> next,
                                                      std::size_t limit =
                                                          kMaxStreamingDiagnosticSamples) {
  if (stored >= limit) {
    return {};
  }
  return next.first(std::min(next.size(), limit - stored));
}

} // namespace vinput::daemon::asr
