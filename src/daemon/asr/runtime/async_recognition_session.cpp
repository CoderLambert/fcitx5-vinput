#include "daemon/asr/runtime/async_recognition_session.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <utility>

namespace vinput::daemon::asr {

struct AsyncRecognitionSession::State {
  State(std::unique_ptr<RecognitionSession> session, std::size_t limit)
      : backend(std::move(session)), max_queued_samples(limit) {}

  std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable complete_cv;
  std::unique_ptr<RecognitionSession> backend;
  std::deque<std::vector<int16_t>> queue;
  std::vector<RecognitionEvent> events;
  std::size_t queued_samples = 0;
  const std::size_t max_queued_samples;
  bool finish_requested = false;
  bool cancel_requested = false;
  bool complete = false;
  bool success = false;
  std::string error;
};

AsyncRecognitionSession::AsyncRecognitionSession(std::unique_ptr<RecognitionSession> backend,
                                                 std::size_t max_queued_samples)
    : state_(std::make_shared<State>(std::move(backend), max_queued_samples)),
      worker_([state = state_] { Run(std::move(state)); }) {}

AsyncRecognitionSession::~AsyncRecognitionSession() {
  Cancel();
  // Finish all backend calls before the ASR manager can destroy the recognizer
  // referenced by a Sherpa streaming session. Capture-error teardown is
  // deferred to the daemon main thread by DaemonRuntimeController.
  if (worker_.joinable()) {
    worker_.join();
  }
}

bool AsyncRecognitionSession::PushAudio(std::span<const int16_t> pcm, std::string* error) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->cancel_requested || state_->finish_requested || state_->complete) {
    if (error) {
      *error = state_->error.empty() ? "Streaming session is no longer recording." : state_->error;
    }
    return false;
  }
  if (pcm.size() > state_->max_queued_samples - state_->queued_samples) {
    if (error) {
      *error = "Streaming audio backlog exceeded the 3-second queue budget.";
    }
    return false; // Caller must signal a recording error; do not silently drop.
  }
  if (!pcm.empty()) {
    state_->queue.emplace_back(pcm.begin(), pcm.end());
    state_->queued_samples += pcm.size();
    state_->wake.notify_one();
  }
  if (error) {
    error->clear();
  }
  return true;
}

bool AsyncRecognitionSession::Finish(std::string* error) {
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (!state_->complete && !state_->cancel_requested) {
    state_->finish_requested = true;
    state_->wake.notify_one();
  }
  state_->complete_cv.wait(lock, [&] { return state_->complete; });
  if (error) {
    *error = state_->error;
  }
  return state_->success;
}

void AsyncRecognitionSession::Cancel() {
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->complete) {
      return;
    }
    state_->cancel_requested = true;
    state_->queue.clear();
    state_->queued_samples = 0;
  }
  state_->wake.notify_one();
}

std::vector<RecognitionEvent> AsyncRecognitionSession::PollEvents() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  auto events = std::move(state_->events);
  state_->events.clear();
  return events;
}

void AsyncRecognitionSession::Run(std::shared_ptr<State> state) {
  try {
    while (true) {
      std::vector<int16_t> chunk;
      bool cancel = false;
      bool finish = false;
      {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->wake.wait(lock, [&] {
          return state->cancel_requested || state->finish_requested || !state->queue.empty();
        });
        if (state->cancel_requested) {
          cancel = true;
        } else if (!state->queue.empty()) {
          chunk = std::move(state->queue.front());
          state->queue.pop_front();
          state->queued_samples -= chunk.size();
        } else if (state->finish_requested) {
          finish = true;
        }
      }

      if (cancel) {
        state->backend->Cancel();
        auto events = state->backend->PollEvents();
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          for (auto& event : events) {
            state->events.push_back(std::move(event));
          }
          state->complete = true;
          state->success = false;
        }
        state->complete_cv.notify_all();
        return;
      }

      std::string error;
      bool ok = false;
      if (finish) {
        ok = state->backend->Finish(&error);
      } else {
        ok = state->backend->PushAudio(chunk, &error);
      }
      auto events = state->backend->PollEvents();
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (auto& event : events) {
          state->events.push_back(std::move(event));
        }
        if (finish) {
          state->complete = true;
          state->success = ok;
          state->error = std::move(error);
        } else if (!ok) {
          state->error = error.empty() ? "Streaming backend rejected audio." : std::move(error);
          state->cancel_requested = true;
          state->queue.clear();
          state->queued_samples = 0;
        }
      }
      if (finish) {
        state->complete_cv.notify_all();
        return;
      }
    }
  } catch (const std::exception& ex) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->error = ex.what();
  } catch (...) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->error = "Streaming worker failed unexpectedly.";
  }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->complete = true;
    state->success = false;
  }
  state->complete_cv.notify_all();
}

} // namespace vinput::daemon::asr
