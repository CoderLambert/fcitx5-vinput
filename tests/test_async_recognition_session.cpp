#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "daemon/asr/runtime/async_recognition_session.h"

namespace {

void Require(bool value, const char* reason) {
  if (!value) {
    std::cerr << "Async ASR regression: " << reason << '\n';
    std::abort();
  }
}

struct FakeState {
  std::mutex mutex;
  std::vector<int16_t> received;
  std::thread::id consumer_thread;
  std::atomic<bool> cancelled{false};
};

class SlowFakeSession final : public vinput::daemon::asr::RecognitionSession {
public:
  explicit SlowFakeSession(std::shared_ptr<FakeState> state, int delay_ms = 25)
      : state_(std::move(state)), delay_ms_(delay_ms) {}

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->consumer_thread = std::this_thread::get_id();
      state_->received.insert(state_->received.end(), pcm.begin(), pcm.end());
    }
    if (error) {
      error->clear();
    }
    return true;
  }

  bool Finish(std::string* error) override {
    events_.push_back({vinput::daemon::asr::RecognitionEventKind::FinalText, "done", {}});
    events_.push_back({vinput::daemon::asr::RecognitionEventKind::Completed, {}, {}});
    if (error) {
      error->clear();
    }
    return true;
  }

  void Cancel() override {
    state_->cancelled = true;
    events_.push_back({vinput::daemon::asr::RecognitionEventKind::Completed, {}, {}});
  }

  std::vector<vinput::daemon::asr::RecognitionEvent> PollEvents() override {
    auto events = std::move(events_);
    events_.clear();
    return events;
  }

private:
  std::shared_ptr<FakeState> state_;
  int delay_ms_;
  std::vector<vinput::daemon::asr::RecognitionEvent> events_;
};

void TestCaptureDoesNotWaitForSlowBackend() {
  auto state = std::make_shared<FakeState>();
  vinput::daemon::asr::AsyncRecognitionSession session(std::make_unique<SlowFakeSession>(state));
  std::string error;
  std::vector<int16_t> expected;
  const auto producer_thread = std::this_thread::get_id();
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    const std::vector<int16_t> chunk(800, static_cast<int16_t>(i));
    expected.insert(expected.end(), chunk.begin(), chunk.end());
    Require(session.PushAudio(chunk, &error), "enqueue should succeed below 3s budget");
  }
  const auto enqueue_time = std::chrono::steady_clock::now() - start;
  Require(enqueue_time < std::chrono::milliseconds(350),
          "enqueue time must not include twenty 25ms backend invocations");
  Require(session.Finish(&error), "finalization must drain queued chunks");

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    Require(state->received == expected, "consumer must see every PCM sample in order");
    Require(state->consumer_thread != producer_thread,
            "backend work must execute off the producer thread");
  }
  const auto events = session.PollEvents();
  int final_count = 0;
  int completed_count = 0;
  for (const auto& event : events) {
    final_count += event.kind == vinput::daemon::asr::RecognitionEventKind::FinalText ? 1 : 0;
    completed_count += event.kind == vinput::daemon::asr::RecognitionEventKind::Completed ? 1 : 0;
  }
  Require(final_count == 1 && completed_count == 1, "finalization events are not duplicated");
}

void TestOverflowFailsWithoutSilentDrop() {
  auto state = std::make_shared<FakeState>();
  vinput::daemon::asr::AsyncRecognitionSession session(
      std::make_unique<SlowFakeSession>(state, 100), 1600);
  std::string error;
  std::vector<int16_t> chunk(800, 42);
  bool rejected = false;
  for (int i = 0; i < 50; ++i) {
    if (!session.PushAudio(chunk, &error)) {
      rejected = true;
      break;
    }
  }
  Require(rejected && !error.empty(), "bounded queue must explicitly reject overload");
  session.Cancel();
}

void TestCancelReturnsWithoutWaitingForDecode() {
  auto state = std::make_shared<FakeState>();
  vinput::daemon::asr::AsyncRecognitionSession session(
      std::make_unique<SlowFakeSession>(state, 200), 3200);
  std::string error;
  std::vector<int16_t> chunk(800, 12);
  Require(session.PushAudio(chunk, &error), "first push");
  const auto start = std::chrono::steady_clock::now();
  session.Cancel();
  Require(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100),
          "Cancel is a nonblocking cancellation request");
}

} // namespace

int main() {
  TestCaptureDoesNotWaitForSlowBackend();
  TestOverflowFailsWithoutSilentDrop();
  TestCancelReturnsWithoutWaitingForDecode();
  std::cout << "Async streaming session tests passed\n";
}
