#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

// Owns all queued PCM. A bounded single-consumer thread executes the actual
// streaming backend while the PipeWire callback only enqueues owned chunks.
class AsyncRecognitionSession final : public RecognitionSession {
public:
  explicit AsyncRecognitionSession(std::unique_ptr<RecognitionSession> backend,
                                   std::size_t max_queued_samples = 16000 * 3);
  ~AsyncRecognitionSession() override;
  AsyncRecognitionSession(const AsyncRecognitionSession&) = delete;
  AsyncRecognitionSession& operator=(const AsyncRecognitionSession&) = delete;

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override;
  bool Finish(std::string* error) override;
  void Cancel() override;
  std::vector<RecognitionEvent> PollEvents() override;

private:
  struct State;
  static void Run(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;
  std::thread worker_;
};

} // namespace vinput::daemon::asr
