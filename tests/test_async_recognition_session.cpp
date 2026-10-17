#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "daemon/asr/runtime/async_recognition_session.h"
#include "daemon/asr/runtime/recognition_contract.h"

namespace {

using vinput::daemon::asr::AsyncRecognitionSession;
using vinput::daemon::asr::RecognitionEvent;
using vinput::daemon::asr::RecognitionEventKind;
using vinput::daemon::asr::RecognitionSession;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Async session regression: " << message << '\n';
    std::abort();
  }
}

// Observable fake backend state, kept alive by the test after the session
// (and its owned backend) are destroyed.
struct FakeState {
  std::mutex mutex;
  std::condition_variable gate_cv;
  bool gate_open = true;
  bool entered_push = false;
  std::vector<int16_t> received;
  int push_calls = 0;
  int finish_calls = 0;
  int cancel_calls = 0;
};

class FakeSession final : public RecognitionSession {
public:
  explicit FakeSession(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    std::unique_lock<std::mutex> lock(state_->mutex);
    ++state_->push_calls;
    state_->entered_push = true;
    state_->gate_cv.notify_all();
    state_->gate_cv.wait(lock, [this] { return state_->gate_open; });
    state_->received.insert(state_->received.end(), pcm.begin(), pcm.end());
    pending_.push_back({.kind = RecognitionEventKind::PartialText, .text = "p", .error = {}});
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  bool Finish(std::string* error) override {
    const std::scoped_lock lock(state_->mutex);
    ++state_->finish_calls;
    pending_.push_back({.kind = RecognitionEventKind::Completed, .text = {}, .error = {}});
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  void Cancel() override {
    const std::scoped_lock lock(state_->mutex);
    ++state_->cancel_calls;
    pending_.push_back({.kind = RecognitionEventKind::Completed, .text = {}, .error = {}});
  }

  std::vector<RecognitionEvent> PollEvents() override {
    const std::scoped_lock lock(state_->mutex);
    auto events = std::move(pending_);
    pending_.clear();
    return events;
  }

private:
  std::shared_ptr<FakeState> state_;
  std::vector<RecognitionEvent> pending_;
};

std::shared_ptr<FakeState> MakeState(bool gate_open) {
  auto state = std::make_shared<FakeState>();
  state->gate_open = gate_open;
  return state;
}

void WaitEntered(const std::shared_ptr<FakeState>& state) {
  std::unique_lock<std::mutex> lock(state->mutex);
  const bool ok = state->gate_cv.wait_for(lock, std::chrono::seconds(5),
                                          [&state] { return state->entered_push; });
  Expect(ok, "worker entered backend PushAudio");
}

void OpenGate(const std::shared_ptr<FakeState>& state) {
  {
    const std::scoped_lock lock(state->mutex);
    state->gate_open = true;
  }
  state->gate_cv.notify_all();
}

int CountCompleted(const std::vector<RecognitionEvent>& events) {
  int count = 0;
  for (const auto& event : events) {
    if (event.kind == RecognitionEventKind::Completed) {
      ++count;
    }
  }
  return count;
}

void TestFinishFlushesInOrderExactlyOnce() {
  auto state = MakeState(true);
  AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 1024);
  std::string error;
  Expect(session.PushAudio(std::vector<int16_t>{1, 2}, &error), "push 1");
  Expect(session.PushAudio(std::vector<int16_t>{3}, &error), "push 2");
  Expect(session.Finish(&error), "finish succeeds after flushing queue");
  {
    const std::scoped_lock lock(state->mutex);
    Expect(state->received == std::vector<int16_t>({1, 2, 3}), "chunks delivered in order");
    Expect(state->finish_calls == 1, "backend Finish called once");
  }
  Expect(CountCompleted(session.PollEvents()) == 1, "Completed emitted exactly once");
  Expect(session.PollEvents().empty(), "events are not replayed");
  Expect(session.Finish(&error), "second Finish returns cached result");
  const std::scoped_lock lock(state->mutex);
  Expect(state->finish_calls == 1, "second Finish must not re-finish backend");
}

void TestQueueBoundDropsIncomingChunk() {
  auto state = MakeState(false);
  AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 4);
  std::string error;
  Expect(session.PushAudio(std::vector<int16_t>{1, 2}, &error), "push in-flight chunk");
  WaitEntered(state); // Worker is inside the backend with chunk 1.
  Expect(session.PushAudio(std::vector<int16_t>{3, 4}, &error), "push queues chunk");
  Expect(session.PushAudio(std::vector<int16_t>{5, 6}, &error), "push fills queue to bound");
  Expect(session.PushAudio(std::vector<int16_t>{7, 8}, &error), "overflow push still returns");
  Expect(session.DroppedChunks() == 1, "overflowing chunk is dropped, not blocked");
  Expect(session.DroppedSamples() == 2, "dropped samples counted");
  OpenGate(state);
  Expect(session.Finish(&error), "finish after drops");
  const std::scoped_lock lock(state->mutex);
  Expect(state->received == std::vector<int16_t>({1, 2, 3, 4, 5, 6}),
         "dropped chunk never delivered");
}

void TestCancelClearsQueueAndCompletesOnce() {
  auto state = MakeState(false);
  AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 1024);
  std::string error;
  Expect(session.PushAudio(std::vector<int16_t>{1}, &error), "push in-flight");
  WaitEntered(state);
  Expect(session.PushAudio(std::vector<int16_t>{2}, &error), "push queued");
  session.Cancel();
  OpenGate(state);
  Expect(!session.Finish(&error), "Finish after Cancel reports failure");
  {
    const std::scoped_lock lock(state->mutex);
    Expect(state->received == std::vector<int16_t>({1}), "queued chunk is discarded by Cancel");
    Expect(state->cancel_calls == 1, "backend Cancel called exactly once");
    Expect(state->finish_calls == 0, "backend Finish not called after Cancel");
  }
  Expect(CountCompleted(session.PollEvents()) == 1, "Completed exactly once after Cancel");
}

void TestDestructorCancelsAndJoinsPromptly() {
  auto state = MakeState(true);
  const auto started = std::chrono::steady_clock::now();
  {
    AsyncRecognitionSession session(std::make_unique<FakeSession>(state), 1024);
    std::string error;
    Expect(session.PushAudio(std::vector<int16_t>{7, 8}, &error), "push before destruction");
  } // Destructor: Cancel + join; backend must not outlive the worker call.
  const auto elapsed = std::chrono::steady_clock::now() - started;
  Expect(elapsed < std::chrono::seconds(5), "destructor joins promptly");
  const std::scoped_lock lock(state->mutex);
  Expect(state->cancel_calls == 1, "destructor cancels backend exactly once");
}

// Equivalent of the controller's deferred retirement at session level:
// after Cancel, a local reference released on another thread must not join
// the worker while the deferred holder still owns the session. The gate
// keeps the backend inside PushAudio; the releaser must report completion
// (signalled via condition variable, not a fixed-ms performance gate)
// while the gate is still closed. Only clearing the deferred holder on
// this thread may join, and repeated Cancel must stay exactly-once.
void TestDeferredReleaseDoesNotJoinOnReleasingThread() {
  auto state = MakeState(false);
  auto session =
      std::make_shared<AsyncRecognitionSession>(std::make_unique<FakeSession>(state), 1024);
  std::string error;
  Expect(session->PushAudio(std::vector<int16_t>{1}, &error), "push in-flight");
  WaitEntered(state); // Worker is blocked inside backend PushAudio.
  Expect(session->PushAudio(std::vector<int16_t>{2}, &error), "push queued");
  session->Cancel();
  session->Cancel(); // Idempotent: must not cause a second backend Cancel.

  std::vector<std::shared_ptr<AsyncRecognitionSession>> deferred;
  deferred.push_back(session);

  std::mutex release_mutex;
  std::condition_variable release_cv;
  bool released = false;
  std::thread releaser([&] {
    session.reset();
    {
      const std::scoped_lock lock(release_mutex);
      released = true;
    }
    release_cv.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(release_mutex);
    const bool ok = release_cv.wait_for(lock, std::chrono::seconds(5), [&] { return released; });
    Expect(ok, "releasing a non-last reference returns while backend is gated");
  }
  releaser.join();
  {
    const std::scoped_lock lock(state->mutex);
    Expect(state->cancel_calls == 0, "backend Cancel not yet run while PushAudio is gated");
  }

  OpenGate(state);
  deferred.clear(); // Main-thread retirement: destructor joins worker here.
  const std::scoped_lock lock(state->mutex);
  Expect(state->received == std::vector<int16_t>({1}), "queued chunk discarded by Cancel");
  Expect(state->cancel_calls == 1, "backend Cancel exactly once after repeated Cancel");
  Expect(state->finish_calls == 0, "backend Finish not called after Cancel");
}

} // namespace

int main() {
  TestFinishFlushesInOrderExactlyOnce();
  TestQueueBoundDropsIncomingChunk();
  TestCancelClearsQueueAndCompletesOnce();
  TestDestructorCancelsAndJoinsPromptly();
  TestDeferredReleaseDoesNotJoinOnReleasingThread();
  std::cout << "Async recognition session tests passed\n";
}
