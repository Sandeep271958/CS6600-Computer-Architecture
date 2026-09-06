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

  const NodeId pc = builder.AddRegister(RegisterRole::kProgramCounter, "PC", new RegisterAdapter(core, RegisterKind::kPc));
  const NodeId if_id = builder.AddRegister(RegisterRole::kIfId, "IF_ID", new RegisterAdapter(core, RegisterKind::kIfId));
  const NodeId id_ex = builder.AddRegister(RegisterRole::kIdEx, "ID_EX", new RegisterAdapter(core, RegisterKind::kIdEx));
  const NodeId ex_mem = builder.AddRegister(RegisterRole::kExMem, "EX_MEM", new RegisterAdapter(core, RegisterKind::kExMem));
  const NodeId mem_wb = builder.AddRegister(RegisterRole::kMemWb, "MEM_WB", new RegisterAdapter(core, RegisterKind::kMemWb));

  const NodeId fetch = builder.AddStage(StageRole::kFetch, "IF", new StageAdapter(core, StageRole::kFetch));
  const NodeId decode = builder.AddStage(StageRole::kDecode, "ID", new StageAdapter(core, StageRole::kDecode));
  const NodeId execute = builder.AddStage(StageRole::kExecute, "EX", new StageAdapter(core, StageRole::kExecute));
  const NodeId memory = builder.AddStage(StageRole::kMemory, "MEM", new StageAdapter(core, StageRole::kMemory));
  const NodeId writeback = builder.AddStage(StageRole::kWriteback, "WB", new StageAdapter(core, StageRole::kWriteback));

  const NodeId l1i = builder.AddCache(cache::CacheRole::kL1I, "L1I", new cache::CacheController(cache::CacheRole::kL1I, config.l1i, machine->TransactionIds()));
  const NodeId l2i = builder.AddCache(cache::CacheRole::kL2I, "L2I", new cache::CacheController(cache::CacheRole::kL2I, config.l2i, machine->TransactionIds()));
  const NodeId l1d = builder.AddCache(cache::CacheRole::kL1D, "L1D", new cache::CacheController(cache::CacheRole::kL1D, config.l1d, machine->TransactionIds()));
  const NodeId l2d = builder.AddCache(cache::CacheRole::kL2D, "L2D", new cache::CacheController(cache::CacheRole::kL2D, config.l2d, machine->TransactionIds()));

  const NodeId main_ctrl_node = builder.AddUnit(UnitRole::kMainControl, "MAIN_CONTROL", main_control);
  const NodeId hazard_ctrl_node = builder.AddUnit(UnitRole::kHazardControl, "HAZARD_CONTROL", hazard_control);

  const NodeId add_node = builder.AddUnit(UnitRole::kAdd, "ADD", pipelines[0]);
  const NodeId sub_node = builder.AddUnit(UnitRole::kSub, "SUB", pipelines[1]);
  const NodeId and_node = builder.AddUnit(UnitRole::kAnd, "AND", pipelines[2]);
  const NodeId or_node = builder.AddUnit(UnitRole::kOr, "OR", pipelines[3]);
  const NodeId xor_node = builder.AddUnit(UnitRole::kXor, "XOR", pipelines[4]);
  const NodeId control_node = builder.AddUnit(UnitRole::kControl, "CONTROL", pipelines[5]);
  const NodeId agu_node = builder.AddUnit(UnitRole::kAgu, "AGU", pipelines[6]);

  builder.Connect(pc, fetch, ConnectionType::kData);
  builder.Connect(fetch, if_id, ConnectionType::kClockedData);
  builder.Connect(if_id, decode, ConnectionType::kData);
  builder.Connect(decode, id_ex, ConnectionType::kClockedData);
  builder.Connect(id_ex, execute, ConnectionType::kData);
  builder.Connect(execute, ex_mem, ConnectionType::kClockedData);
  builder.Connect(ex_mem, memory, ConnectionType::kData);
  builder.Connect(memory, mem_wb, ConnectionType::kClockedData);
  builder.Connect(mem_wb, writeback, ConnectionType::kData);

  builder.Connect(decode, add_node, ConnectionType::kClockedData);
  builder.Connect(decode, sub_node, ConnectionType::kClockedData);
  builder.Connect(decode, and_node, ConnectionType::kClockedData);
  builder.Connect(decode, or_node, ConnectionType::kClockedData);
  builder.Connect(decode, xor_node, ConnectionType::kClockedData);
  builder.Connect(decode, control_node, ConnectionType::kClockedData);
  builder.Connect(decode, agu_node, ConnectionType::kClockedData);

  builder.Connect(add_node, execute, ConnectionType::kData);
  builder.Connect(sub_node, execute, ConnectionType::kData);
  builder.Connect(and_node, execute, ConnectionType::kData);
  builder.Connect(or_node, execute, ConnectionType::kData);
  builder.Connect(xor_node, execute, ConnectionType::kData);
  builder.Connect(control_node, execute, ConnectionType::kData);
  builder.Connect(agu_node, memory, ConnectionType::kData);

  builder.Connect(main_ctrl_node, decode, ConnectionType::kControl);

  builder.Connect(hazard_ctrl_node, pc, ConnectionType::kStall);
  builder.Connect(hazard_ctrl_node, if_id, ConnectionType::kStall);
  builder.Connect(hazard_ctrl_node, id_ex, ConnectionType::kBubble);

  builder.Connect(control_node, pc, ConnectionType::kRedirect);

  builder.Connect(fetch, l1i, ConnectionType::kData);
  builder.Connect(l1i, l2i, ConnectionType::kData);
  builder.Connect(memory, l1d, ConnectionType::kData);
  builder.Connect(l1d, l2d, ConnectionType::kData);

  return builder.Build(machine);
}
} // namespace pipesim