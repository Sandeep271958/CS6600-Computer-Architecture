#include "speculation/internal.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace pipesim {

SpeculationController::SpeculationController(std::size_t maximum_checkpoints) : impl_(new Impl(maximum_checkpoints)) {
  if (maximum_checkpoints == 0) {
    throw std::runtime_error("maximum checkpoints must be positive");
  }
}

SpeculationController::~SpeculationController() = default;

void SpeculationController::Reset() { impl_->checkpoints.clear(); }

bool SpeculationController::CanFetch() const { return impl_->checkpoints.size() < impl_->maximum_checkpoints; }

bool SpeculationController::CanDispatch(std::uint64_t sequence, const Instruction &instruction) const {

  // Younger Loads, Stores, Branches, Jumps, Halts are not dispatched if older unresolved branches exist
  bool is_restricted = (instruction.opcode == kLw || instruction.opcode == kSw || instruction.opcode == kBeq ||
                        instruction.opcode == kBne || instruction.opcode == kJ || instruction.opcode == kHalt);

  if (!is_restricted)
    return true;

  for (const auto &checkpoint : impl_->checkpoints) {
    if (checkpoint.sequence < sequence)
      return false;
  }
  return true;
}

void SpeculationController::AddCheckpoint(const BranchCheckpoint &checkpoint) {
  // Ensures that dispatch is In-Order
  assert(impl_->checkpoints.empty() || impl_->checkpoints.back().sequence < checkpoint.sequence);
  impl_->checkpoints.push_back(checkpoint);
}

bool SpeculationController::IsSpeculative(std::uint64_t sequence) const {
  for (const auto &checkpoint : impl_->checkpoints) {
    if (checkpoint.sequence < sequence)
      return true;
  }
  return false;
}

std::uint64_t SpeculationController::SpeculationDepth(std::uint64_t sequence) const {
  // Naive Implementation O(N)
  uint64_t speculation_depth = 0;
  for (const auto &checkpoint : impl_->checkpoints) {
    if (checkpoint.sequence < sequence)
      ++speculation_depth;
  }
  return speculation_depth;

  // Assuming checkpoints is maintained in strictly increasing sequence order.
  // O(LogN) Implementation
  // return static_cast<uint64_t>(
  //     std::lower_bound(impl_->checkpoints.begin(), impl_->checkpoints.end(), sequence,
  //                      [](const auto &checkpoint, uint64_t seq) { return checkpoint.sequence < seq; }) -
  //     impl_->checkpoints.begin());
}

RecoveryDecision SpeculationController::Resolve(const BranchResult &result) {
  RecoveryDecision decision = {.mispredicted = false,
                               .branch_sequence = result.sequence,
                               .correct_next_pc = result.pc + 4};

  if (result.taken)
    decision.correct_next_pc = result.target;

  for (auto it = impl_->checkpoints.begin(), end = impl_->checkpoints.end(); it != end; ++it) {
    const auto &checkpoint = *it;
    if (checkpoint.sequence == result.sequence) {
      decision.mispredicted = (checkpoint.predicted_next_pc != decision.correct_next_pc);
      impl_->checkpoints.erase(it);
      return decision;
    }
  }

  throw std::runtime_error("resolved branch has no checkpoint");
}

void SpeculationController::RemoveYoungerCheckpoints(std::uint64_t sequence) {
  auto it = std::find_if(impl_->checkpoints.begin(), impl_->checkpoints.end(),
                         [sequence](const auto &cp) { return cp.sequence > sequence; });
  impl_->checkpoints.erase(it, impl_->checkpoints.end());
}

std::size_t SpeculationController::CheckpointCount() const { return impl_->checkpoints.size(); }

} // namespace pipesim
