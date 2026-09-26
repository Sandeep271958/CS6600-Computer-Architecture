#include "design/core.h"
#include "design/instruction_helpers.h"
#include <limits>
#include <stdexcept>
namespace pipesim::design_internal {
Core::Core(Machine *machine, const std::array<ExecutionPipeline *, 7> &pipelines, MainControl *main_control,
           HazardControl *hazard_control, BranchPredictionSystem *branch_prediction,
           SpeculationController *speculation_control)
    : machine_(machine), pipelines_(pipelines), main_control_(main_control), hazard_control_(hazard_control),
      branch_prediction_(branch_prediction), speculation_control_(speculation_control) {
  if (machine_ == nullptr || main_control_ == nullptr || hazard_control_ == nullptr || branch_prediction_ == nullptr ||
      speculation_control_ == nullptr)
    throw std::runtime_error("CPU core received a null component");
  for (std::size_t i = 0; i < pipelines_.size(); ++i)
    if (pipelines_[i] == nullptr || pipelines_[i]->kind() != static_cast<ExecutionUnitKind>(i))
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
    output << "pc=0x" << std::hex << pc_ << std::dec << " stopped=" << (fetch_stopped_ ? 1 : 0);
    return output.str();
  case RegisterKind::kIfId:
    if (!if_id_.valid)
      return "EMPTY";
    output << OpcodeName(if_id_.fetched.instruction.opcode) << "@0x" << std::hex << if_id_.fetched.instruction.pc
           << std::dec << "#" << if_id_.fetched.sequence;
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

void Core::ResetStage(StageRole role) { snapshots_[static_cast<std::size_t>(role)] = StageSnapshot(); }

StageSnapshot Core::Snapshot(StageRole role) const { return snapshots_[static_cast<std::size_t>(role)]; }

void Core::Writeback() {
  BeginCycle();

  StageSnapshot &snapshot = snapshots_[static_cast<std::size_t>(StageRole::kWriteback)];
  snapshot = StageSnapshot();
  snapshot.action = "EMPTY";

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
  StageSnapshot &snapshot = snapshots_[static_cast<std::size_t>(StageRole::kMemory)];
  snapshot = StageSnapshot();
  snapshot.action = "EMPTY";

  for (std::size_t index = 0; index < pipelines_.size(); ++index) {
    const std::optional<ExecutionResult> output = pipelines_[index]->OutputIntent();
    if (!output)
      continue;

    snapshot = SnapshotFor(output->instruction);

    // Control pipeline output is owned by Core::Execute() (branch resolution).
    if (static_cast<ExecutionUnitKind>(index) == ExecutionUnitKind::kControl)
      continue;

    if (static_cast<ExecutionUnitKind>(index) == ExecutionUnitKind::kAgu) {
      if (!machine_->PendingDataHasCapacity()) {
        machine_->RecordCpuStall(CpuStallReason::kPendingDataFull);
        snapshot.action = "STALL";
      } else if (!machine_->DataRequestLinkCanAccept()) {
        machine_->RecordCpuStall(CpuStallReason::kDcacheInput);
        snapshot.action = "STALL";
      } else if (machine_->TryIssueData(output->sequence, output->address, output->memory_write, output->store_value)) {
        consume_output_[index] = true;
        snapshot.action = "HANDOFF";
      } else {
        snapshot.action = "STALL";
      }
    } else {
      machine_->CompleteNonMemory(output->sequence, output->value, output->halt);
      consume_output_[index] = true;
      snapshot.action = "COMPLETE";
    }
  }
}

void Core::Execute() {
  StageSnapshot &snapshot = snapshots_[static_cast<std::size_t>(StageRole::kExecute)];
  snapshot = StageSnapshot();
  snapshot.action = "EMPTY";

  // Branch resolution fires when the control execution pipeline produces output.
  const std::size_t ctrl_idx = static_cast<std::size_t>(ExecutionUnitKind::kControl);
  const std::optional<ExecutionResult> ctrl_output = pipelines_[ctrl_idx]->OutputIntent();

  if (!ctrl_output)
    return;

  snapshot = SnapshotFor(ctrl_output->instruction);
  const Opcode opcode = ctrl_output->instruction.opcode;
  bool mispredicted = false;

  if (opcode == Opcode::kBeq || opcode == Opcode::kBne || opcode == Opcode::kJ) {
    BranchResult result;
    result.sequence      = ctrl_output->sequence;
    result.pc            = ctrl_output->instruction.pc;
    result.type          = (opcode == Opcode::kBeq || opcode == Opcode::kBne)
                             ? BranchType::kConditional : BranchType::kUnconditional;
    // The control pipeline computed the actual outcome from the latched operands.
    result.taken         = ctrl_output->redirect;
    result.target        = ctrl_output->next_pc;
    result.fetch_cycle   = ctrl_output->fetch_cycle;
    result.resolve_cycle = current_boundary_;

    RecoveryDecision decision = speculation_control_->Resolve(result);
    branch_prediction_->Resolve(result, ctrl_output->predictions); // metrics only

    if (decision.mispredicted) {
      mispredicted = true;
      next_pc_ = decision.correct_next_pc;
      SquashReport report = machine_->SquashAfter(result.sequence);

      // Sum pipeline squash counts
      std::uint64_t pipeline_operations = 0;
      for (std::size_t i = 0; i < pipelines_.size(); ++i) {
        pipeline_operations += pipelines_[i]->SquashAfter(result.sequence);
        consume_output_[i] = false;
      }

      speculation_control_->RemoveYoungerCheckpoints(result.sequence);
      branch_prediction_->RecordSquash(report.fetches, pipeline_operations,
                                       report.completions, report.data_requests);

      // Squash IF/ID only if it holds a younger mis-speculated instruction.
      // Set both current and next so the latch doesn't survive commit.
      if (if_id_.valid && if_id_.fetched.sequence > result.sequence) {
        if_id_.valid      = false;
        next_if_id_.valid = false;
      }
    }
  }

  // The branch/halt always retires normally regardless of prediction outcome.
  // On misprediction the squash loop cleared consume_output_[ctrl_idx]; restore it.
  machine_->CompleteNonMemory(ctrl_output->sequence, ctrl_output->value, ctrl_output->halt);
  consume_output_[ctrl_idx] = true;
  next_fetch_stopped_ = false;
  snapshot.action = mispredicted ? "SQUASH" : "COMPLETE";
}

void Core::Decode() {
  StageSnapshot &snapshot = snapshots_[static_cast<std::size_t>(StageRole::kDecode)];
  snapshot = StageSnapshot();
  snapshot.action = "EMPTY";

  std::optional<pipesim::ExecutionOperation> dispatch_op = std::nullopt;
  pipesim::ExecutionUnitKind target_unit = pipesim::ExecutionUnitKind::kAdd;

  if (if_id_.valid) {
    snapshot = SnapshotFor(if_id_.fetched.instruction);

    DecodeResult decode_result = main_control_->Decode(if_id_.fetched.instruction);

    target_unit = decode_result.unit;

    // Observe the decoded instruction. Returns true if the predictor incorrectly
    // identified this non-branch as a branch (false branch). Redirect to pc+4
    // and squash all younger work immediately.
    bool false_branch = branch_prediction_->ObserveInstruction(if_id_.fetched.instruction, if_id_.fetched.predictions);
    if (false_branch) {
      next_pc_ = if_id_.fetched.instruction.pc + 4;
      machine_->InvalidateFetchesAfter(if_id_.fetched.sequence);
      if_id_consumed_ = true;
      next_if_id_.valid = false;
      next_fetch_stopped_ = false;
      snapshot.action = "DISPATCH"; // consumed, not stalled
    }

    if (!false_branch) {

      HazardResult hazard_result = hazard_control_->Check(machine_, if_id_.fetched.sequence, if_id_.fetched.instruction,
                                                          decode_result.writes_register);

      if (!machine_->CompletionQueueHasCapacity()) {
        machine_->RecordCpuStall(CpuStallReason::kCompletionQueueFull);
        snapshot.action = "STALL";
        next_fetch_stopped_ = true;
      } else if (hazard_result == HazardResult::kRaw) {
        machine_->RecordCpuStall(CpuStallReason::kOutstandingRaw);
        snapshot.action = "STALL";
        next_fetch_stopped_ = true;
      } else if (decode_result.writes_register &&
                 machine_->HasOlderDestination(if_id_.fetched.sequence, if_id_.fetched.instruction.rd)) {
        machine_->RecordCpuStall(CpuStallReason::kOutstandingWaw);
        snapshot.action = "STALL";
        next_fetch_stopped_ = true;
      } else {
        ExecutionPipeline *target_pipeline = pipelines_[static_cast<std::size_t>(decode_result.unit)];

        if (!speculation_control_->CanDispatch(if_id_.fetched.sequence, if_id_.fetched.instruction)) {
          machine_->RecordCpuStall(CpuStallReason::kControlDisabled);
          snapshot.action = "STALL";
          next_fetch_stopped_ = true;
        } else {

          if (!target_pipeline->CanAccept(current_boundary_,
                                          consume_output_[static_cast<std::size_t>(decode_result.unit)])) {
            machine_->RecordCpuStall(CpuStallReason::kExecutionUnitBusy);
            snapshot.action = "STALL";
            next_fetch_stopped_ = true;
          } else {
            std::optional<std::uint64_t> result_boundary = std::nullopt;
            bool slot_ok = true;
            if (decode_result.unit != pipesim::ExecutionUnitKind::kAgu) {
              std::uint64_t rb = current_boundary_ + static_cast<std::uint64_t>(target_pipeline->config().latency) + 2;
              result_boundary = rb;
              if (!machine_->NormalCompletionSlotAvailable(rb)) {
                slot_ok = false;
              }
            }

            if (!slot_ok) {
              machine_->RecordCpuStall(CpuStallReason::kExecutionCompletionSlot);
              snapshot.action = "STALL";
              next_fetch_stopped_ = true;
            } else {
              bool allocated = machine_->TryAllocateCompletion(if_id_.fetched.sequence, if_id_.fetched.instruction,
                                                               decode_result.writes_register, result_boundary);

              if (!allocated) {
                machine_->RecordCpuStall(CpuStallReason::kExecutionCompletionSlot);
                snapshot.action = "STALL";
                next_fetch_stopped_ = true;
              } else {
                snapshot.action = "DISPATCH";
                if_id_consumed_ = true;
                next_if_id_.valid = false;
                next_fetch_stopped_ = false;

                if (target_unit == pipesim::ExecutionUnitKind::kControl) {
                  auto opcode = if_id_.fetched.instruction.opcode;

                  if (opcode == Opcode::kHalt) {
                    // kHalt terminates the program: stop all further fetching this
                    // cycle and invalidate any instructions already in the fetch queue
                    // that are younger than halt (they must never execute).
                    control_recognized_this_cycle_ = true;
                    next_fetch_stopped_ = true;
                    machine_->InvalidateFetchesAfter(if_id_.fetched.sequence);
                  } else {
                    // kBeq / kBne / kJ — speculative branch.
                    // DO NOT stop fetch here: CanFetch() in Fetch gates further
                    // speculation based on checkpoint capacity; stopping fetch here
                    // would defeat the entire speculative-execution design.
                    // DO NOT read registers or compute taken/target: that is the
                    // CONTROL execution unit's job. Reading stale register values at
                    // Decode and overwriting next_pc_ would discard the predictor's
                    // redirect that was already applied in Fetch this cycle.
                    // DO NOT call InvalidateFetchesAfter here: the instructions
                    // fetched on the predicted path are correct-path speculation;
                    // squashing them here would remove valid work.
                    //
                    // Record the checkpoint so Execute can recover on misprediction.
                    BranchCheckpoint cp;
                    cp.sequence = if_id_.fetched.sequence;
                    cp.predicted_next_pc = if_id_.fetched.predictions.next_pc;
                    cp.predictions = if_id_.fetched.predictions;
                    speculation_control_->AddCheckpoint(cp);
                    branch_prediction_->RecordSpeculativeDispatch();
                  }
                }

                pipesim::ExecutionOperation op;
                op.sequence = if_id_.fetched.sequence;
                op.instruction = if_id_.fetched.instruction;
                op.dispatch_boundary = current_boundary_;
                op.fetch_cycle = if_id_.fetched.fetch_cycle;
                op.predictions = if_id_.fetched.predictions;
                op.lhs = machine_->ReadRegister(if_id_.fetched.instruction.rs1);
                op.rhs = (if_id_.fetched.instruction.opcode == pipesim::Opcode::kAddi ||
                          if_id_.fetched.instruction.opcode == pipesim::Opcode::kLw ||
                          if_id_.fetched.instruction.opcode == pipesim::Opcode::kSw)
                             ? static_cast<std::uint32_t>(if_id_.fetched.instruction.immediate)
                             : machine_->ReadRegister(if_id_.fetched.instruction.rs2);
                op.store_value = machine_->ReadRegister(if_id_.fetched.instruction.rs2);
                dispatch_op = op;
              }
            }
          }
        } // end CanDispatch else
      }
    } // end !false_branch
  }

  for (std::size_t i = 0; i < pipelines_.size(); ++i) {
    bool is_target = if_id_consumed_ && (static_cast<std::size_t>(target_unit) == i);
    pipelines_[i]->Operate(current_boundary_, is_target ? dispatch_op : std::nullopt, consume_output_[i]);
  }
  pipelines_operated_ = true;
}

void Core::Fetch() {
  StageSnapshot &snapshot = snapshots_[static_cast<std::size_t>(StageRole::kFetch)];
  snapshot = StageSnapshot();
  snapshot.pc = pc_;

  if (!fetch_stopped_ && !control_recognized_this_cycle_ && (!if_id_.valid || if_id_consumed_)) {
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
      if (!speculation_control_->CanFetch()) {
        machine_->RecordCpuStall(CpuStallReason::kControlDisabled);
        if (snapshot.action == "EMPTY")
          snapshot.action = "STALL";
      } else {
        BranchPredictionSet predictions = branch_prediction_->Predict(pc_, current_boundary_);
        if (!machine_->TryIssuePredictedFetch(pc_, predictions, current_boundary_))
          return;

        next_pc_ = predictions.next_pc;

        if (snapshot.action == "EMPTY")
          snapshot.action = "FETCH";

        const auto depth = speculation_control_->CheckpointCount();
        if (depth != 0)
          branch_prediction_->RecordSpeculativeFetch(depth);
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
