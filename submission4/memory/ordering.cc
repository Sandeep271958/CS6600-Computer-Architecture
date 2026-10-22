#include "pa4.h"
namespace multicore {

MemoryOrderDecision decide_memory_action(const MemoryRequest &request, const MemoryOrderUnit &order_unit,
                                         const SystemConfig &configuration) {

  // PA4-ORDER-01: Choose the TSO action. The framework moves requests between queues.
  // Default decision initialized to {MemoryAction::Wait, 0}
  MemoryOrderDecision decision;

  switch (request.operation) {
  case MemoryOperation::Load: {
    decision.kind = MemoryAction::Issue; // Assume no matching stores; can bypass

    // Search in reverse (youngest to oldest) to forward from the latest store
    for (auto it = order_unit.stores.rbegin(); it != order_unit.stores.rend(); ++it) {
      if (it->address == request.address) {
        decision = {MemoryAction::Forward, it->value};
        // If store matches load address, forward youngest value
        break;
      }
    }
    break;
  }

  case MemoryOperation::Store: {
    if (order_unit.stores.size() >= configuration.store_entries) {
      // The buffer is full; stall the core and retain default (Wait) to drain the oldest store
    } else {
      // If not full, store in the buffer
      decision.kind = MemoryAction::BufferStore;
    }
    break;
  }

  case MemoryOperation::Fence: {
    if (order_unit.stores.empty()) {
      decision.kind = MemoryAction::CompleteFence;
      // If no stores are pending, the Fence completes immediately
    } else {
      // If stores remain, retain default (Wait)
    }
    break;
  }

  case MemoryOperation::Exchange: {
    if (!order_unit.stores.empty()) {
      // Indivisible exchange requires buffer to drain; retain default (Wait)
    } else {
      decision.kind = MemoryAction::Issue;
    }
    break;
  }

  default: {
    break;
  }
  }

  return decision;
}

} // namespace multicore
