#pragma once
#include <array>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include "branch_prediction.h"
#include "design/control.h"
#include "execution_unit.h"
#include "pipesim.h"
#include "speculation.h"

namespace pipesim::design_internal {
struct FrontendLatch {
  bool valid = false;
  FetchedInstruction fetched;
  bool decoded = false;
  bool control_recognized = false;
  bool prediction_observed = false;
  DecodeResult decode;
};

struct ExecutionLatch {
  bool valid = false;
  std::uint64_t sequence = 0;
  Instruction instruction;
  std::uint32_t lhs = 0;
  std::uint32_t rhs = 0;
  std::uint32_t store_value = 0;
  std::uint32_t result = 0;
  bool writes_register = false;
  bool memory_read = false;
  bool memory_write = false;
  bool halt = false;
  bool redirect = false;
  std::uint32_t next_pc = 0;
};

enum class RegisterKind { kPc, kIfId, kIdEx, kExMem, kMemWb };

class Core {
 public:
  Core(Machine* machine, const std::array<ExecutionPipeline*, 7>& pipelines,
       MainControl* main_control, HazardControl* hazard_control,
       BranchPredictionSystem* branch_prediction,
       SpeculationController* speculation_control);
  void ResetRegister(RegisterKind);
  void CommitRegister(RegisterKind);
  std::string RegisterSnapshot(RegisterKind) const;
  void ResetStage(StageRole);
  StageSnapshot Snapshot(StageRole) const;
  void Writeback();
  void Memory();
  void Execute();
  void Decode();
  void Fetch();

 private:
  void BeginCycle();
  static StageSnapshot SnapshotFor(const Instruction&);
  Machine* machine_;
  std::array<ExecutionPipeline*, 7> pipelines_;
  MainControl* main_control_;
  HazardControl* hazard_control_;
  BranchPredictionSystem* branch_prediction_;
  SpeculationController* speculation_control_;
  std::uint32_t pc_ = 0, next_pc_ = 0;
  bool fetch_stopped_ = false, next_fetch_stopped_ = false;
  FrontendLatch if_id_, next_if_id_;
  ExecutionLatch id_ex_, next_id_ex_;
  ExecutionLatch ex_mem_, next_ex_mem_;
  ExecutionLatch mem_wb_, next_mem_wb_;
  std::uint64_t current_boundary_ = 0;
  std::array<bool, 7> consume_output_ = {};
  bool if_id_consumed_ = false;
  bool control_recognized_this_cycle_ = false;
  bool recovery_recorded_this_cycle_ = false;
  bool pipelines_operated_ = false;
  bool cycle_begun_ = false;
  StageSnapshot snapshots_[5];
};
}  // namespace pipesim::design_internal
