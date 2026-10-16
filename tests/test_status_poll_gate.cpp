#include <cstdlib>
#include <iostream>

#include "dbus/status_poll_gate.h"

namespace {
void Check(bool valid, const char* what) {
  if (!valid) {
    std::cerr << "Status poll gate regression: " << what << '\n';
    std::abort();
  }
}

void TestSingleFlightAndCurrentCompletion() {
  vinput::addon::StatusPollGate gate;
  const auto first = gate.TryBegin();
  Check(first.has_value(), "first request created");
  Check(!gate.TryBegin().has_value(), "polling never starts two concurrent calls");
  Check(gate.IsActive(*first), "first request active");
  Check(gate.Complete(*first), "current request may apply");
  Check(!gate.IsActive(*first), "finished request cleared");
  Check(gate.TryBegin().has_value(), "next request may start");
}

void TestStatusSignalAndSessionInvalidate() {
  vinput::addon::StatusPollGate gate;
  const auto old = *gate.TryBegin();
  gate.Invalidate(); // The daemon emitted a newer StatusChanged signal.
  Check(!gate.Complete(old), "outdated status query must be ignored");
  const auto next = *gate.TryBegin();
  Check(gate.Complete(next), "new status query remains usable");
}

void TestCancelledLateReplyCannotClearNewRequest() {
  vinput::addon::StatusPollGate gate;
  const auto old = *gate.TryBegin();
  gate.Cancel();
  const auto next = *gate.TryBegin();
  Check(!gate.IsActive(old), "cancelled request is no longer active");
  Check(!gate.Complete(old), "old reply cannot complete a new request");
  Check(gate.IsActive(next), "new pending query survives old callback");
  Check(gate.Complete(next), "new query can complete");
}

} // namespace

int main() {
  TestSingleFlightAndCurrentCompletion();
  TestStatusSignalAndSessionInvalidate();
  TestCancelledLateReplyCannotClearNewRequest();
  std::cout << "Status poll gate tests passed\n";
}
