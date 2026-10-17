#pragma once

#include <cstdint>
#include <optional>

namespace vinput::addon {

// Tracks one asynchronous status request and rejects replies from old
// sessions or older D-Bus status signals.
class StatusPollGate {
public:
  struct Ticket {
    std::uint64_t id;
    std::uint64_t generation;
  };

  std::optional<Ticket> TryBegin() {
    if (active_) {
      return std::nullopt;
    }
    active_ = true;
    return Ticket{.id = ++request_id_, .generation = generation_};
  }

  void Invalidate() { ++generation_; }

  [[nodiscard]] bool IsActive(Ticket ticket) const { return active_ && request_id_ == ticket.id; }

  bool Complete(Ticket ticket) {
    if (!IsActive(ticket)) {
      return false;
    }
    active_ = false;
    return ticket.generation == generation_;
  }

  void Cancel() {
    active_ = false;
    ++generation_;
    ++request_id_;
  }

private:
  std::uint64_t generation_ = 0;
  std::uint64_t request_id_ = 0;
  bool active_ = false;
};

} // namespace vinput::addon
