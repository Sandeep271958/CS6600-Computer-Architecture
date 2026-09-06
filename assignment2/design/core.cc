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
  switch (kind) {
  case RegisterKind::kPc:
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
    break;
  case RegisterKind::kIfId:
    if_id_ = FrontendLatch();
    next_if_id_ = FrontendLatch();
    break;
  case RegisterKind::kIdEx:
    id_ex_ = ExecutionLatch();
    next_id_ex_ = ExecutionLatch();
    break;
  case RegisterKind::kExMem:
    ex_mem_ = ExecutionLatch();
    next_ex_mem_ = ExecutionLatch();
    break;
  case RegisterKind::kMemWb:
    mem_wb_ = ExecutionLatch();
    next_mem_wb_ = ExecutionLatch();
    break;
  }
}
void Core::CommitRegister(RegisterKind kind) {
  // Only check for operate completion on PC commit (first register)
  if (kind == RegisterKind::kPc && (!cycle_begun_ || !pipelines_operated_))
    throw std::runtime_error("CPU core commit without a complete operate pass");

  switch (kind) {
  case RegisterKind::kPc:
    pc_ = next_pc_;
    fetch_stopped_ = next_fetch_stopped_;
    if_id_ = next_if_id_;
    break;
  case RegisterKind::kIfId:
    if_id_ = next_if_id_;
    break;
  case RegisterKind::kIdEx:
    id_ex_ = next_id_ex_;
    break;
  case RegisterKind::kExMem:
    ex_mem_ = next_ex_mem_;
    break;
  case RegisterKind::kMemWb:
    mem_wb_ = next_mem_wb_;
    break;
  }

  if (kind == RegisterKind::kPc) {
    if (current_boundary_ == std::numeric_limits<std::uint64_t>::max())
      throw std::runtime_error("CPU boundary counter overflow");
    ++current_boundary_;
    cycle_begun_ = false;
  }
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

  for (std::size_t index = 0; index < pipelines_.size(); ++index) {
    const std::optional<ExecutionResult> output =
        pipelines_[index]->OutputIntent();
    if (!output) continue;

    snapshot = SnapshotFor(output->instruction);
    if (static_cast<ExecutionUnitKind>(index) == ExecutionUnitKind::kAgu) {
      if (!machine_->PendingDataHasCapacity()) {
        machine_->RecordCpuStall(CpuStallReason::kPendingDataFull);
        snapshot.action = "STALL";
      } else if (!machine_->DataRequestLinkCanAccept()) {
        machine_->RecordCpuStall(CpuStallReason::kDcacheInput);
        snapshot.action = "STALL";
      } else if (machine_->TryIssueData(output->sequence, output->address,
                                        output->memory_write,
                                        output->store_value)) {
        consume_output_[index] = true;
        snapshot.action = "HANDOFF";
      } else {
        snapshot.action = "STALL";
      }
    } else {
      machine_->CompleteNonMemory(output->sequence, output->value,
                                  output->halt);
      consume_output_[index] = true;
      snapshot.action = "COMPLETE";
      if (static_cast<ExecutionUnitKind>(index) == ExecutionUnitKind::kControl)
        next_fetch_stopped_ = false;
    }
  }
}

void Core::Execute() {
  StageSnapshot &snapshot =
      snapshots_[static_cast<std::size_t>(StageRole::kExecute)];
  snapshot = StageSnapshot();
  snapshot.action = "EMPTY";

  next_ex_mem_ = ExecutionLatch();
  if (!id_ex_.valid) return;

  DecodeResult dec = main_control_->Decode(id_ex_.instruction);
  next_ex_mem_.valid = true;
  next_ex_mem_.sequence = id_ex_.sequence;
  next_ex_mem_.instruction = id_ex_.instruction;
  next_ex_mem_.lhs = id_ex_.lhs;
  next_ex_mem_.rhs = id_ex_.rhs;
  next_ex_mem_.store_value = id_ex_.store_value;
  next_ex_mem_.writes_register = dec.writes_register;

  const Opcode opcode = id_ex_.instruction.opcode;
  next_ex_mem_.memory_read = opcode == Opcode::kLw;
  next_ex_mem_.memory_write = opcode == Opcode::kSw;
  next_ex_mem_.halt = opcode == Opcode::kHalt;

  snapshot = SnapshotFor(id_ex_.instruction);
  snapshot.action = "RUN";
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

    // Spec §4.2: checks in exact order:
    // 1. Completion capacity
    // 2. RAW
    // 3. WAW
    // 4. Unit availability
    // 5. Completion reservation (fixed-result boundary slot)

      if (!machine_->CompletionQueueHasCapacity()) {
        machine_->RecordCpuStall(CpuStallReason::kCompletionQueueFull);
      snapshot.action = "STALL";
      next_fetch_stopped_ = true;
    } else if (hazard_result == HazardResult::kRaw) {
        machine_->RecordCpuStall(CpuStallReason::kOutstandingRaw);
      snapshot.action = "STALL";
      next_fetch_stopped_ = true;
    } else if (decode_result.writes_register &&
               machine_->HasOlderDestination(if_id_.fetched.sequence,
                                             if_id_.fetched.instruction.rd)) {
      // WAW: younger write cannot overtake older write to same destination
        machine_->RecordCpuStall(CpuStallReason::kOutstandingWaw);
      snapshot.action = "STALL";
      next_fetch_stopped_ = true;
    } else {
      ExecutionPipeline *target_pipeline =
          pipelines_[static_cast<std::size_t>(decode_result.unit)];

            if (!target_pipeline->CanAccept(
              current_boundary_,
              consume_output_[static_cast<std::size_t>(decode_result.unit)])) {
        // Unit availability
          machine_->RecordCpuStall(CpuStallReason::kExecutionUnitBusy);
        snapshot.action = "STALL";
        next_fetch_stopped_ = true;
      } else {
        // Completion reservation: only for fixed (non-AGU) units
        std::optional<std::uint64_t> result_boundary = std::nullopt;
        bool slot_ok = true;
        if (decode_result.unit != pipesim::ExecutionUnitKind::kAgu) {
          // Spec: nominal_result_boundary = dispatch_boundary + latency + 1
          // Framework requires result_boundary >= current_boundary + 2
          std::uint64_t rb = current_boundary_ +
                             static_cast<std::uint64_t>(target_pipeline->config().latency) + 2;
          result_boundary = rb;
          if (!machine_->NormalCompletionSlotAvailable(rb)) {
            slot_ok = false;
          }
        }

        if (!slot_ok) {
                machine_->RecordCpuStall(CpuStallReason::kExecutionCompletionSlot);
                snapshot.action = "STALL";
                next_fetch_stopped_ = true;
          next_fetch_stopped_ = true;
        } else {
          bool allocated = machine_->TryAllocateCompletion(
              if_id_.fetched.sequence, if_id_.fetched.instruction,
              decode_result.writes_register, result_boundary);

          if (!allocated) {
            machine_->RecordCpuStall(CpuStallReason::kExecutionCompletionSlot);
            snapshot.action = "STALL";
            next_fetch_stopped_ = true;
            next_fetch_stopped_ = true;
          } else {
            snapshot.action = "DISPATCH";
            if_id_consumed_ = true;
            next_if_id_.valid = false;
            next_fetch_stopped_ = false;

            if (target_unit == pipesim::ExecutionUnitKind::kControl) {
              control_recognized_this_cycle_ = true;
              // Spec §6.1: fetch stops at recognition until CONTROL result is consumed
              next_fetch_stopped_ = true;

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

              if (taken || opcode == Opcode::kHalt) {
                machine_->InvalidateFetchesAfter(if_id_.fetched.sequence);
                if (taken) next_pc_ = target;
              }
            }

            pipesim::ExecutionOperation op;
            op.sequence = if_id_.fetched.sequence;
            op.instruction = if_id_.fetched.instruction;
            op.dispatch_boundary = current_boundary_;
            // Latch operands at dispatch (spec §4.2: dispatch is atomic)
            op.lhs = machine_->ReadRegister(if_id_.fetched.instruction.rs1);
            op.rhs = (if_id_.fetched.instruction.opcode == pipesim::Opcode::kAddi ||
                      if_id_.fetched.instruction.opcode == pipesim::Opcode::kLw  ||
                      if_id_.fetched.instruction.opcode == pipesim::Opcode::kSw)
                         ? static_cast<std::uint32_t>(if_id_.fetched.instruction.immediate)
                         : machine_->ReadRegister(if_id_.fetched.instruction.rs2);
            // SW: store value is rs2 (separate from rhs which is the address offset)
            op.store_value = machine_->ReadRegister(if_id_.fetched.instruction.rs2);
            dispatch_op = op;
          }
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

  if (!fetch_stopped_ && !control_recognized_this_cycle_ &&
      (!if_id_.valid || if_id_consumed_)) {
    FetchedInstruction fetched;
    if (machine_->PeekFetched(&fetched)) {
      next_if_id_.valid = true;
      next_if_id_.fetched = fetched;
      next_if_id_.decode = main_control_->Decode(fetched.instruction);
      next_if_id_.control_recognized = false;
      machine_->ConsumeFetched();
      snapshot.action = "ACCEPTED";
    }
  }

  if (!fetch_stopped_ && !control_recognized_this_cycle_) {
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
