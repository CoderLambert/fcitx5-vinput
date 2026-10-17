#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

// Wraps a streaming session so PushAudio/PollEvents run on one worker thread
// instead of the audio capture callback. The callback only enqueues an owned
// copy. The queue is bounded (default 3 s at 16 kHz): when full, the incoming
// chunk is counted as dropped and the session fails explicitly rather than
// blocking the realtime callback or silently losing audio.
class AsyncRecognitionSession final : public RecognitionSession {
public:
  explicit AsyncRecognitionSession(std::unique_ptr<RecognitionSession> backend,
                                   std::size_t max_queued_samples = std::size_t{16000} * 3);
  ~AsyncRecognitionSession() override;

  AsyncRecognitionSession(const AsyncRecognitionSession&) = delete;
  AsyncRecognitionSession& operator=(const AsyncRecognitionSession&) = delete;
  AsyncRecognitionSession(AsyncRecognitionSession&&) = delete;
  AsyncRecognitionSession& operator=(AsyncRecognitionSession&&) = delete;

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override;
  bool Finish(std::string* error) override;
  void Cancel() override;
  std::vector<RecognitionEvent> PollEvents() override;

  [[nodiscard]] std::size_t DroppedChunks() const;
  [[nodiscard]] std::size_t DroppedSamples() const;

private:
  void Run();
  void AppendEvents(std::vector<RecognitionEvent> events);
  void Complete(bool success, std::string error);

  std::unique_ptr<RecognitionSession> backend_;
  const std::size_t max_queued_samples_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable complete_cv_;
  std::deque<std::vector<int16_t>> queue_;
  std::vector<RecognitionEvent> events_;
  std::size_t queued_samples_ = 0;
  std::size_t dropped_chunks_ = 0;
  std::size_t dropped_samples_ = 0;
  bool finish_requested_ = false;
  bool cancel_requested_ = false;
  bool complete_ = false;
  bool success_ = false;
  std::string error_;

  std::thread worker_; // Joined in the destructor before members are torn down.
};

} // namespace vinput::daemon::asr
