#include "execution_unit/internal.h"
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
  throw std::runtime_error("ExecutionPipeline::OutputIntent: TODO(A2-EXEC-02)");
}

bool ExecutionPipeline::CanAccept(std::uint64_t dispatch_boundary, bool output_will_be_consumed) const {
  (void)dispatch_boundary;
  (void)output_will_be_consumed;
  throw std::runtime_error("ExecutionPipeline::CanAccept: TODO(A2-EXEC-03)");
}

void ExecutionPipeline::Operate(std::uint64_t current_boundary, const std::optional<ExecutionOperation> &dispatch,
                                bool consume_output) {
  if (!impl_->begun || impl_->operated)
    throw std::runtime_error("execution pipeline Operate lifecycle violation");
  (void)current_boundary;
  (void)dispatch;
  (void)consume_output;
  throw std::runtime_error("ExecutionPipeline::Operate: TODO(A2-EXEC-04)");
}

void ExecutionPipeline::Commit() { throw std::runtime_error("ExecutionPipeline::Commit: TODO(A2-EXEC-05)"); }

ExecutionSnapshot ExecutionPipeline::Snapshot() const {
  throw std::runtime_error("ExecutionPipeline::Snapshot: TODO(A2-EXEC-05)");
}

bool ExecutionPipeline::HasWork() const { throw std::runtime_error("ExecutionPipeline::HasWork: TODO(A2-EXEC-05)"); }
} // namespace pipesim
