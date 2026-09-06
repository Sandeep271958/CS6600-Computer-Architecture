#include "execution_unit/helpers.h"
#include "execution_unit/internal.h"
#include <stdexcept>
namespace pipesim {
ExecutionPipeline::Impl::Impl(ExecutionUnitKind unit_kind, const ExecutionUnitConfig &unit_config)
    : kind(unit_kind), config(unit_config) {
  if (static_cast<std::size_t>(kind) > static_cast<std::size_t>(ExecutionUnitKind::kAgu))
    throw std::runtime_error("invalid execution-unit kind");
  if (config.initiation_interval == 0)
    throw std::runtime_error("zero execution initiation interval");

  current.assign(config.latency + 1, std::nullopt);
  next.assign(config.latency + 1, std::nullopt);
  next_legal_current = 0;
  next_legal_next = 0;
  begun = false;
  operated = false;
}

ExecutionResult ExecutionPipeline::Impl::ResultFor(const ExecutionOperation &operation) const {
  ExecutionResult result;
  result.sequence = operation.sequence;
  result.instruction = operation.instruction;
  result.nominal_result_boundary = execution_internal::CheckedResultBoundary(operation.dispatch_boundary,
                                                                             config.latency);

  switch (kind) {
  case ExecutionUnitKind::kAdd:
    result.value = operation.lhs + operation.rhs;
    return result;
  case ExecutionUnitKind::kSub:
    result.value = operation.lhs - operation.rhs;
    return result;
  case ExecutionUnitKind::kAnd:
    result.value = operation.lhs & operation.rhs;
    return result;
  case ExecutionUnitKind::kOr:
    result.value = operation.lhs | operation.rhs;
    return result;
  case ExecutionUnitKind::kXor:
    result.value = operation.lhs ^ operation.rhs;
    return result;
  case ExecutionUnitKind::kControl:
    switch (operation.instruction.opcode) {
    case Opcode::kBeq:
      result.redirect = (operation.lhs == operation.rhs);
      result.next_pc = operation.instruction.immediate;
      break;
    case Opcode::kBne:
      result.redirect = (operation.lhs != operation.rhs);
      result.next_pc = operation.instruction.immediate;
      break;
    case Opcode::kJ:
      result.redirect = true;
      result.next_pc = operation.instruction.immediate;
      break;
    case Opcode::kHalt:
      result.halt = true;
      break;
    default:
      break;
    }
    return result;
  case ExecutionUnitKind::kAgu:
    result.memory = true;
    result.address = operation.lhs + operation.rhs;
    result.memory_write = (operation.instruction.opcode == Opcode::kSw);
    result.store_value = operation.store_value;
    return result;
  }
  throw std::runtime_error("ResultFor: unknown execution unit kind");
}
} // namespace pipesim
