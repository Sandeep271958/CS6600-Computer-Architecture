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

  for (std::size_t i = 0; i < pipelines_.size(); ++i) {
    auto intent = pipelines_[i]->OutputIntent();
    if (!intent.has_value()) {
      continue;
    }

    if (pipelines_[i]->kind() == pipesim::ExecutionUnitKind::kAgu) {
      snapshot = SnapshotFor(intent->instruction);
      if (!machine_->PendingDataHasCapacity()) {
        machine_->RecordCpuStall(pipesim::CpuStallReason::kPendingDataFull);
        snapshot.action = "STALL";
      } else if (!machine_->DataRequestLinkCanAccept()) {
        machine_->RecordCpuStall(pipesim::CpuStallReason::kDcacheInput);
        snapshot.action = "STALL";
      } else {
        bool accepted =
            machine_->TryIssueData(intent->sequence, intent->address,
                                   intent->memory_write, intent->store_value);
        if (accepted) {
          consume_output_[i] = true;
          snapshot.action = "HANDOFF";
        } else {
          snapshot.action = "STALL";
        }
      }
    } else {
      machine_->CompleteNonMemory(intent->sequence, intent->value,
                                  intent->halt);
      consume_output_[i] = true;
    }
  }
}

void Core::Execute() {
  StageSnapshot &snapshot =
      snapshots_[static_cast<std::size_t>(StageRole::kExecute)];
  snapshot = StageSnapshot();
  snapshot.action = "EMPTY";
}

void Core::Decode() {
  StageSnapshot &snapshot =
      snapshots_[static_cast<std::size_t>(StageRole::kDecode)];
  snapshot = StageSnapshot();
  snapshot.action = "EMPTY";

  std::optional<pipesim::ExecutionOperation> dispatch_op = std::nullopt;
  pipesim::ExecutionUnitKind target_unit = pipesim::ExecutionUnitKind::kAdd;

  if (if_id_.valid) {
    snapshot = SnapshotFor(if_id_.fetched.instruction);

    DecodeResult decode_result =
        main_control_->Decode(if_id_.fetched.instruction);

    target_unit = decode_result.unit;

    HazardResult hazard_result = hazard_control_->Check(
        machine_, if_id_.fetched.sequence, if_id_.fetched.instruction,
        decode_result.writes_register);

    if (hazard_result == HazardResult::kRaw) {
      snapshot.action = "STALL";
      next_fetch_stopped_ = true;
    } else {
      ExecutionPipeline *target_pipeline =
          pipelines_[static_cast<std::size_t>(decode_result.unit)];

      if (!target_pipeline->CanAccept(current_boundary_, false)) {
        snapshot.action = "STALL";
        next_fetch_stopped_ = true;
      } else {
        bool allocated = machine_->TryAllocateCompletion(
            if_id_.fetched.sequence, if_id_.fetched.instruction,
            decode_result.writes_register, std::nullopt);

        if (!allocated) {
          snapshot.action = "STALL";
          next_fetch_stopped_ = true;
        } else {
          snapshot.action = "DISPATCH";
          if_id_consumed_ = true;

          if (target_unit == pipesim::ExecutionUnitKind::kControl) {
            control_recognized_this_cycle_ = true;
            bool taken = false;
            std::uint32_t target = 0;
            auto opcode = if_id_.fetched.instruction.opcode;

            if (opcode == pipesim::Opcode::kBeq) {
              std::uint32_t first =
                  machine_->ReadRegister(if_id_.fetched.instruction.rs1);
              std::uint32_t second =
                  machine_->ReadRegister(if_id_.fetched.instruction.rs2);
              taken = (first == second);
              target = if_id_.fetched.instruction.immediate;
            } else if (opcode == pipesim::Opcode::kBne) {
              std::uint32_t first =
                  machine_->ReadRegister(if_id_.fetched.instruction.rs1);
              std::uint32_t second =
                  machine_->ReadRegister(if_id_.fetched.instruction.rs2);
              taken = (first != second);
              target = if_id_.fetched.instruction.immediate;
            } else if (opcode == pipesim::Opcode::kJ) {
              taken = true;
              target = if_id_.fetched.instruction.immediate;
            }

            if (taken) {
              machine_->InvalidateFetchesAfter(if_id_.fetched.sequence);
              next_pc_ = target;
            }
          }

          pipesim::ExecutionOperation op;
          op.sequence = if_id_.fetched.sequence;
          op.instruction = if_id_.fetched.instruction;
          op.dispatch_boundary = current_boundary_;
          dispatch_op = op;
        }
      }
    }
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

void Core::Fetch() {
  StageSnapshot &snapshot =
      snapshots_[static_cast<std::size_t>(StageRole::kFetch)];
  snapshot = StageSnapshot();
  snapshot.pc = pc_;

  if (!if_id_.valid || if_id_consumed_) {
    FetchedInstruction fetched;
    if (machine_->PeekFetched(&fetched)) {
      if (!fetch_stopped_) {
        next_if_id_.valid = true;
        next_if_id_.fetched = fetched;
        next_if_id_.decode = main_control_->Decode(fetched.instruction);
        next_if_id_.control_recognized = false;
        machine_->ConsumeFetched();
        snapshot.action = "ACCEPTED";
      } else {
        next_if_id_.valid = false;
      }
    }
  }

  if (!fetch_stopped_) {
    if (!machine_->FetchQueueHasCapacity()) {
      machine_->RecordCpuStall(CpuStallReason::kFetchQueueFull);
      if (snapshot.action == "EMPTY")
        snapshot.action = "STALL";
    } else if (!machine_->FetchRequestLinkCanAccept()) {
      machine_->RecordCpuStall(CpuStallReason::kIcacheInput);
      if (snapshot.action == "EMPTY")
        snapshot.action = "STALL";
    } else {
      if (machine_->TryIssueFetch(pc_)) {
        next_pc_ = pc_ + 4;
        if (snapshot.action == "EMPTY")
          snapshot.action = "FETCH";
      }
    }
  }
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
