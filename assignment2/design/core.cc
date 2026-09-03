#include "design/core.h"
#include "design/instruction_helpers.h"
#include <limits>
#include <stdexcept>
namespace pipesim::design_internal {
Core::Core(Machine *machine,
           const std::array<ExecutionPipeline *, 7> &pipelines,
           MainControl *main_control, HazardControl *hazard_control)
    : machine_(machine), pipelines_(pipelines), main_control_(main_control),
      hazard_control_(hazard_control) {
  if (machine_ == nullptr || main_control_ == nullptr ||
      hazard_control_ == nullptr)
    throw std::runtime_error("CPU core received a null component");
  for (std::size_t i = 0; i < pipelines_.size(); ++i)
    if (pipelines_[i] == nullptr ||
        pipelines_[i]->kind() != static_cast<ExecutionUnitKind>(i))
      throw std::runtime_error("CPU core execution-unit registration mismatch");
}
void Core::ResetRegister(RegisterKind kind) {
  if (kind != RegisterKind::kPc)
    return;
  pc_ = machine_->EntryPoint();
  next_pc_ = pc_;
  fetch_stopped_ = false;
  next_fetch_stopped_ = false;
  if_id_ = FrontendLatch();
  next_if_id_ = FrontendLatch();
  current_boundary_ = 0;
  cycle_begun_ = false;
  for (auto &s : snapshots_)
    s = StageSnapshot();
}
void Core::CommitRegister(RegisterKind kind) {
  if (kind != RegisterKind::kPc)
    return;
  if (!cycle_begun_ || !pipelines_operated_)
    throw std::runtime_error("CPU core commit without a complete operate pass");
  pc_ = next_pc_;
  fetch_stopped_ = next_fetch_stopped_;
  if_id_ = next_if_id_;
  if (current_boundary_ == std::numeric_limits<std::uint64_t>::max())
    throw std::runtime_error("CPU boundary counter overflow");
  ++current_boundary_;
  cycle_begun_ = false;
}
std::string Core::RegisterSnapshot(RegisterKind kind) const {
  std::ostringstream output;
  switch (kind) {
  case RegisterKind::kPc:
    output << "pc=0x" << std::hex << pc_ << std::dec
           << " stopped=" << (fetch_stopped_ ? 1 : 0);
    return output.str();
  case RegisterKind::kIfId:
    if (!if_id_.valid)
      return "EMPTY";
    output << OpcodeName(if_id_.fetched.instruction.opcode) << "@0x" << std::hex
           << if_id_.fetched.instruction.pc << std::dec << "#"
           << if_id_.fetched.sequence;
    return output.str();
  case RegisterKind::kIdEx:
    return "EXECUTION_PIPELINES";
  case RegisterKind::kExMem:
    return "AGU_HANDOFF";
  case RegisterKind::kMemWb:
    return "COMPLETION_QUEUE";
  }
  return "EMPTY";
}
void Core::ResetStage(StageRole role) {
  snapshots_[static_cast<std::size_t>(role)] = StageSnapshot();
}
StageSnapshot Core::Snapshot(StageRole role) const {
  return snapshots_[static_cast<std::size_t>(role)];
}
void Core::Writeback() {
  BeginCycle();

  // Create an empty snapshot
  StageSnapshot &snapshot =
      snapshots_[static_cast<std::size_t>(StageRole::kWriteback)];
  snapshot = StageSnapshot();
  snapshot.action = "EMPTY";

  // Retire a max of 1 instruction per cycle
  pipesim::Retirement ret;
  bool canRetire = machine_->PeekRetirement(&ret);

  if (canRetire) {
    machine_->ApplyRetirement(ret);
    StageSnapshot retire_snapshot = SnapshotFor(ret.instruction);
    retire_snapshot.action = "RETIRE";
    snapshot = retire_snapshot;
  }
}
void Core::Memory() {
  StageSnapshot &snapshot =
      snapshots_[static_cast<std::size_t>(StageRole::kMemory)];
  snapshot = StageSnapshot();
  snapshot.action = "EMPTY";

  for (int i = 0; i < 7; i++) {
    auto intent = pipelines_[i]->OutputIntent();
    if (!intent.has_value()) {
      continue;
    }

    if (pipelines_[i]->kind() != pipesim::ExecutionUnitKind::kAgu) {
      machine_->CompleteNonMemory(intent->sequence, intent->value,
                                  intent->halt);
      consume_output_[i] = true;
    } else {
      snapshot = SnapshotFor(intent->instruction);
      if (!machine_->PendingDataHasCapacity()) {
        machine_->RecordCpuStall(pipesim::CpuStallReason::kPendingDataFull);
        snapshot.action = "STALL";
      } else if (!machine_->DataRequestLinkCanAccept()) {
        machine_->RecordCpuStall(pipesim::CpuStallReason::kDcacheInput);
        snapshot.action = "STALL";
      } else {
        machine_->TryIssueData(intent->sequence, intent->address,
                               intent->memory_write, intent->store_value);
        consume_output_[i] = true;
        snapshot.action = "HANDOFF";
      }
    }
  }
}

void Core::Execute() {
  StageSnapshot &snapshot =
      snapshots_[static_cast<std::size_t>(StageRole::kExecute)];
  snapshot = StageSnapshot();
  snapshot.action = "EMPTY";

  std::optional<ExecutionOperation> dispatch_op = std::nullopt;

  ExecutionUnitKind target_unit = ExecutionUnitKind::kAdd;

  if (if_id_consumed_) {

    DecodeResult decode_result =
        main_control_->Decode(if_id_.fetched.instruction);

    target_unit = decode_result.unit;

    ExecutionOperation op;
    op.sequence = if_id_.fetched.sequence;
    op.instruction = if_id_.fetched.instruction;
    op.dispatch_boundary = current_boundary_;

    dispatch_op = op;
    snapshot = SnapshotFor(if_id_.fetched.instruction);
    snapshot.action = "OPERATE";
  }

  for (std::size_t i = 0; i < pipelines_.size(); ++i) {
    bool is_target =
        if_id_consumed_ && (static_cast<std::size_t>(target_unit) == i);

    pipelines_[i]->Operate(current_boundary_,
                           is_target ? dispatch_op : std::nullopt,
                           consume_output_[i]);
  }
  pipelines_operated_ = true;
}

void Core::Decode() {
  StageSnapshot &snapshot =
      snapshots_[static_cast<std::size_t>(StageRole::kDecode)];
  snapshot = StageSnapshot();
  snapshot.action = "EMPTY";
  if (!if_id_.valid) {
    return;
  }
  snapshot = SnapshotFor(if_id_.fetched.instruction);

  DecodeResult decode_result =
      main_control_->Decode(if_id_.fetched.instruction);

  HazardResult hazard_result = hazard_control_->Check(
      machine_, if_id_.fetched.sequence, if_id_.fetched.instruction,
      decode_result.writes_register);

  if (hazard_result == HazardResult::kRaw) {
    snapshot.action = "STALL";
    next_fetch_stopped_ = true;
    return;
  }

  ExecutionPipeline *target_pipeline =
      pipelines_[static_cast<std::size_t>(decode_result.unit)];

  if (!target_pipeline->CanAccept(current_boundary_, false)) {
    snapshot.action = "STALL";
    next_fetch_stopped_ = true;
    return;
  }

  snapshot.action = "DISPATCH";
  if_id_consumed_ = true;

  bool allocated = machine_->TryAllocateCompletion(
      if_id_.fetched.sequence, if_id_.fetched.instruction,
      decode_result.writes_register, std::nullopt);

  if (!allocated) {
    snapshot.action = "STALL";
    next_fetch_stopped_ = true;
    if_id_consumed_ = false;
    return;
  }
}

void Core::Fetch() {
  StageSnapshot &snapshot =
      snapshots_[static_cast<std::size_t>(StageRole::kFetch)];
  snapshot = StageSnapshot();
  snapshot.pc = pc_;
  throw std::runtime_error("Core::Fetch: TODO(A2-DESIGN-02)");
}
void Core::BeginCycle() {
  if (cycle_begun_)
    throw std::runtime_error("CPU logic cycle initialized twice");
  next_pc_ = pc_;
  next_fetch_stopped_ = fetch_stopped_;
  next_if_id_ = if_id_;
  consume_output_.fill(false);
  if_id_consumed_ = false;
  control_recognized_this_cycle_ = false;
  pipelines_operated_ = false;
  cycle_begun_ = true;
}
StageSnapshot Core::SnapshotFor(const Instruction &instruction) {
  StageSnapshot snapshot;
  snapshot.valid = true;
  snapshot.pc = instruction.pc;
  snapshot.opcode = OpcodeName(instruction.opcode);
  return snapshot;
}
} // namespace pipesim::design_internal
