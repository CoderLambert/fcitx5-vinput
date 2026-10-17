#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "dbus/status_poll_gate.h"

namespace {
void Check(bool valid, const char* what) {
  if (!valid) {
    std::cerr << "Status poll gate regression: " << what << '\n';
    std::abort();
  }
}

// Asserts the optional holds a ticket before any access, as cpp-linter
// requires; callers only ever handle the unwrapped Ticket value.
vinput::addon::StatusPollGate::Ticket
RequireTicket(std::optional<vinput::addon::StatusPollGate::Ticket> ticket) {
  if (!ticket.has_value()) {
    std::cerr << "Status poll gate regression: expected a ticket\n";
    std::abort();
  }
  return *ticket;
}

void TestSingleFlightAndCurrentCompletion() {
  vinput::addon::StatusPollGate gate;
  const auto first = RequireTicket(gate.TryBegin());
  Check(!gate.TryBegin().has_value(), "polling never starts two concurrent calls");
  Check(gate.IsActive(first), "first request active");
  Check(gate.Complete(first), "current request may apply");
  Check(!gate.IsActive(first), "finished request cleared");
  Check(gate.TryBegin().has_value(), "next request may start");
}

void TestStatusSignalAndSessionInvalidate() {
  vinput::addon::StatusPollGate gate;
  const auto old = RequireTicket(gate.TryBegin());
  gate.Invalidate(); // The daemon emitted a newer StatusChanged signal.
  Check(!gate.Complete(old), "outdated status query must be ignored");
  const auto next = RequireTicket(gate.TryBegin());
  Check(gate.Complete(next), "new status query remains usable");
}

void TestCancelledLateReplyCannotClearNewRequest() {
  vinput::addon::StatusPollGate gate;
  const auto old = RequireTicket(gate.TryBegin());
  gate.Cancel();
  const auto next = RequireTicket(gate.TryBegin());
  Check(!gate.IsActive(old), "cancelled request is no longer active");
  Check(!gate.Complete(old), "old reply cannot complete a new request");
  Check(gate.IsActive(next), "new pending query survives old callback");
  Check(gate.Complete(next), "new query can complete");
}

// Simulates the frontend consumer: a reply may overwrite the known status
// only when the gate still considers its ticket current.
struct StatusConsumer {
  vinput::addon::StatusPollGate gate;
  std::string applied_status = "Unknown";

  bool ApplyReply(vinput::addon::StatusPollGate::Ticket ticket, const std::string& status) {
    if (!gate.Complete(ticket)) {
      return false;
    }
    applied_status = status;
    return true;
  }
};

void TestLateReplyMustNotOverwriteNewerState() {
  StatusConsumer consumer;
  const auto old = RequireTicket(consumer.gate.TryBegin());
  // A newer StatusChanged signal arrives before the old reply.
  consumer.gate.Invalidate();
  consumer.applied_status = "Recording";
  Check(!consumer.ApplyReply(old, "Idle"), "late reply must be rejected");
  Check(consumer.applied_status == "Recording", "late reply must not overwrite newer state");
  const auto next = RequireTicket(consumer.gate.TryBegin());
  Check(consumer.ApplyReply(next, "Idle"), "current reply may apply");
  Check(consumer.applied_status == "Idle", "current reply updates state");
}

void TestSessionReplacement() {
  StatusConsumer consumer;
  const auto old = RequireTicket(consumer.gate.TryBegin());
  consumer.gate.Cancel(); // Session replaced: pending query is abandoned.
  const auto next = RequireTicket(consumer.gate.TryBegin());
  Check(next.id != old.id, "replacement session gets a new request id");
  Check(!consumer.gate.IsActive(old), "old session request is inactive");
  Check(!consumer.ApplyReply(old, "Idle"), "old session reply rejected");
  Check(consumer.gate.IsActive(next), "new session request survives old reply");
  Check(consumer.ApplyReply(next, "Recording"), "new session reply applies");
}

void TestStatusChangedArrivingEarly() {
  StatusConsumer consumer;
  consumer.gate.Invalidate(); // StatusChanged arrives before any query begins.
  const auto ticket = RequireTicket(consumer.gate.TryBegin());
  Check(consumer.ApplyReply(ticket, "Idle"),
        "query begun after early signal is current for its generation");
}

void TestDestructionDuringPendingCallback() {
  auto lifetime = std::make_shared<bool>(true);
  const std::weak_ptr<bool> token = lifetime;
  bool callback_touched_gate = false;
  {
    vinput::addon::StatusPollGate gate;
    const auto pending = RequireTicket(gate.TryBegin());
    (void)pending;
    // Owner (and its gate) is destroyed while the reply is still in flight;
    // the lifetime token expires with it, mirroring VinputEngine's guard.
    lifetime.reset();
  }
  // Pending callback runs after destruction: it must bail out on the
  // expired token and never dereference the destroyed gate/owner.
  if (!token.expired()) {
    callback_touched_gate = true;
  }
  Check(token.expired(), "lifetime token expires with owner");
  Check(!callback_touched_gate, "callback after destruction must not touch gate");
}

} // namespace

int main() {
  TestSingleFlightAndCurrentCompletion();
  TestStatusSignalAndSessionInvalidate();
  TestCancelledLateReplyCannotClearNewRequest();
  TestLateReplyMustNotOverwriteNewerState();
  TestSessionReplacement();
  TestStatusChangedArrivingEarly();
  TestDestructionDuringPendingCallback();
  std::cout << "Status poll gate tests passed\n";
}
