#include "execution_unit.h"
#include "execution_config.h"
#include "execution_unit/helpers.h"
#include "execution_unit/internal.h"
#include <algorithm>
#include <stdexcept>
namespace pipesim {
const char *ExecutionUnitKindName(ExecutionUnitKind kind) {
  switch (kind) {
  case ExecutionUnitKind::kAdd:
    return "ADD";
  case ExecutionUnitKind::kSub:
    return "SUB";
  case ExecutionUnitKind::kAnd:
    return "AND";
  case ExecutionUnitKind::kOr:
    return "OR";
  case ExecutionUnitKind::kXor:
    return "XOR";
  case ExecutionUnitKind::kControl:
    return "CONTROL";
  case ExecutionUnitKind::kAgu:
    return "AGU";
  }
  return "UNKNOWN";
}

ExecutionPipeline::ExecutionPipeline(ExecutionUnitKind kind, const ExecutionUnitConfig &config)
    : impl_(new Impl(kind, config)) {}
ExecutionPipeline::~ExecutionPipeline() = default;

void ExecutionPipeline::Reset() {
  uint32_t latency = impl_->config.latency;
  impl_->current.assign(latency + 1, std::nullopt);
  impl_->next.assign(latency + 1, std::nullopt);
  impl_->next_legal_current = 0;
  impl_->next_legal_next = 0;
  impl_->begun = false;
  impl_->operated = false;
}

void ExecutionPipeline::BeginCycle() {
  if (impl_->begun) {
    throw std::runtime_error("execution pipeline already begun");
  }

  impl_->next = impl_->current;
  impl_->next_legal_next = impl_->next_legal_current;
  impl_->begun = true;
  impl_->operated = false;
}
ExecutionUnitKind ExecutionPipeline::kind() const { return impl_->kind; }
const ExecutionUnitConfig &ExecutionPipeline::config() const { return impl_->config; }

std::optional<ExecutionResult> ExecutionPipeline::OutputIntent() const {
  if (!impl_->current.empty() && impl_->current.back().has_value()) {
    return impl_->ResultFor(impl_->current.back().value());
  }
  return std::nullopt;
}

bool ExecutionPipeline::CanAccept(std::uint64_t dispatch_boundary, bool output_will_be_consumed) const {
  if (dispatch_boundary >= impl_->next_legal_current) {
    bool pipeline_is_full = std::all_of(impl_->current.begin(), impl_->current.end(),
                                        [](const auto &stage) { return stage.has_value(); });
    if (!output_will_be_consumed && pipeline_is_full)
      return false;
    else
      return true;
  } else
    return false;
}

void ExecutionPipeline::Operate(std::uint64_t current_boundary, const std::optional<ExecutionOperation> &dispatch,
                                bool consume_output) {
  if (!impl_->begun || impl_->operated)
    throw std::runtime_error("execution pipeline Operate lifecycle violation");
  (void)current_boundary;
  (void)dispatch;

  uint32_t depth = impl_->current.size();
  std::vector<bool> will_be_free(depth, false);

  // Find the bubbles back to front
  will_be_free[depth - 1] = !impl_->current[depth - 1].has_value() || consume_output;
  for (int i = depth - 2; i >= 0; i--) {
    will_be_free[i] = !impl_->current[i].has_value() || will_be_free[i + 1];
  }

  // Move instructions through the pipeline
  for (uint32_t i = 1; i < depth; i++) {
    if (will_be_free[i]) {
      impl_->next[i] = impl_->current[i - 1];
    } else {
      impl_->next[i] = impl_->current[i];
    }
  }

  // First stage gets the new instruction
  if (will_be_free[0]) {
    impl_->next[0] = dispatch;
  } else {
    impl_->next[0] = impl_->current[0];
  }

  // Update Initiation Interval tracking
  if (dispatch.has_value()) {
    impl_->next_legal_next = dispatch->dispatch_boundary + impl_->config.initiation_interval;
  } else {
    impl_->next_legal_next = impl_->next_legal_current;
  }

  impl_->operated = true;
}

void ExecutionPipeline::Commit() {
  if (!impl_->operated) {
    throw std::runtime_error("Called without Operate; Execution Pipeline Commit lifecycle violation");
  }

  impl_->current = impl_->next;
  impl_->next_legal_current = impl_->next_legal_next;
  impl_->operated = false;
  impl_->begun = false;
}

ExecutionSnapshot ExecutionPipeline::Snapshot() const {
  ExecutionSnapshot snapshot;
  snapshot.kind = impl_->kind;
  snapshot.next_legal_dispatch_boundary = impl_->next_legal_current;

  uint32_t latency = impl_->current.size();

  for (uint32_t i = 0; i < latency; ++i) {
    ExecutionStageSnapshot stage_snapshot;
    stage_snapshot.stage = i;

    if (impl_->current[i].has_value()) {
      const auto &op = impl_->current[i].value();
      stage_snapshot.valid = true;
      stage_snapshot.sequence = op.sequence;
      stage_snapshot.pc = op.instruction.pc;
      stage_snapshot.opcode = OpcodeName(op.instruction.opcode);
      stage_snapshot.unit = ExecutionUnitKindName(impl_->kind);
      stage_snapshot.dispatch_boundary = op.dispatch_boundary;
      stage_snapshot.nominal_result_boundary = execution_internal::CheckedResultBoundary(op.dispatch_boundary,
                                                                                         impl_->config.latency);
      stage_snapshot.lhs = op.lhs;
      stage_snapshot.rhs = op.rhs;
      stage_snapshot.store_value = op.store_value;
      stage_snapshot.destination = op.instruction.rd;
      stage_snapshot.writes_register = execution_internal::WritesRegister(op.instruction.opcode, op.instruction.rd);
    }

    snapshot.stages.push_back(stage_snapshot);
  }

  return snapshot;
}

bool ExecutionPipeline::HasWork() const {
  if (!std::any_of(impl_->current.begin(), impl_->current.end(), [](const auto &stage) { return stage.has_value(); }))
    return false;
  else
    return true;
}

std::uint64_t ExecutionPipeline::SquashAfter(std::uint64_t sequence) {
  std::uint64_t removed = 0;
  for (auto &operation : impl_->next) {
    if (operation && operation->sequence > sequence) {
      operation.reset();
      ++removed;
    }
  }
  return removed;
}
} // namespace pipesim
