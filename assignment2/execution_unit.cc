#include "execution_unit.h"
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

  // Next Legal Current indicates the boundary when the next instruction is legally allowed to enter -> IPC boundary
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
    impl_->next_legal_next = current_boundary + impl_->config.initiation_interval;
  } else {
    impl_->next_legal_next = impl_->next_legal_current;
  }

  impl_->operated = true;
}

void ExecutionPipeline::Commit() { throw std::runtime_error("ExecutionPipeline::Commit: TODO(A2-EXEC-05)"); }

ExecutionSnapshot ExecutionPipeline::Snapshot() const {
  throw std::runtime_error("ExecutionPipeline::Snapshot: TODO(A2-EXEC-05)");
}

bool ExecutionPipeline::HasWork() const { throw std::runtime_error("ExecutionPipeline::HasWork: TODO(A2-EXEC-05)"); }
} // namespace pipesim
