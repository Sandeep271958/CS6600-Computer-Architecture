#include "cache/cache_controller.h"
#include "design/adapters.h"
#include "design/control.h"
#include "design/core.h"
#include "design/instruction_helpers.h"
#include "execution_unit.h"
#include "pipesim.h"
#include <array>
#include <memory>
#include <stdexcept>

namespace pipesim {
using namespace design_internal;
std::unique_ptr<Processor> BuildDesign(Machine *machine) {
  if (machine == nullptr)
    throw std::runtime_error("BuildDesign machine is null");
  ProcessorBuilder builder;
  const cache::SystemConfig &config = machine->Configuration();
  std::array<ExecutionPipeline *, 7> pipelines = {};
  for (std::size_t i = 0; i < pipelines.size(); ++i)
    pipelines[i] = new ExecutionPipeline(static_cast<ExecutionUnitKind>(i),
                                         config.execution_units[i]);
  MainControl *main_control = new MainControl();
  HazardControl *hazard_control = new HazardControl();
  std::shared_ptr<Core> core(
      new Core(machine, pipelines, main_control, hazard_control));
  const NodeId pc =
      builder.AddRegister(RegisterRole::kProgramCounter, "PC",
                          new RegisterAdapter(core, RegisterKind::kPc));
  const NodeId fetch = builder.AddStage(
      StageRole::kFetch, "IF", new StageAdapter(core, StageRole::kFetch));
  builder.Connect(pc, fetch, ConnectionType::kData);
  const NodeId l1i = builder.AddCache(
      cache::CacheRole::kL1I, "L1I",
      new cache::CacheController(cache::CacheRole::kL1I, config.l1i,
                                 machine->TransactionIds()));
  builder.Connect(fetch, l1i, ConnectionType::kData);
  (void)main_control;
  (void)hazard_control;

  NodeId id_issue =
      builder.AddRegister(RegisterRole::kIfId, "id_issue",
                          new RegisterAdapter(core, RegisterKind::kIfId));
  builder.Connect(fetch, id_issue, ConnectionType::kClockedData);

  NodeId add_node = builder.AddUnit(UnitRole::kAdd, "add", pipelines[0]);
  NodeId sub_node = builder.AddUnit(UnitRole::kSub, "sub", pipelines[1]);
  NodeId and_node = builder.AddUnit(UnitRole::kAnd, "and", pipelines[2]);
  NodeId or_node = builder.AddUnit(UnitRole::kOr, "or", pipelines[3]);
  NodeId xor_node = builder.AddUnit(UnitRole::kXor, "xor", pipelines[4]);
  NodeId control_node =
      builder.AddUnit(UnitRole::kControl, "control", pipelines[5]);
  NodeId agu_node = builder.AddUnit(UnitRole::kAgu, "agu", pipelines[6]);
}
} // namespace pipesim
