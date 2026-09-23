#include "predictors/predictor_one.h"

#include <stdexcept>

namespace pipesim {

std::string PredictorOne::Name() const { return "PAPER_ONE"; }

std::uint32_t PredictorOne::GetScIndex(size_t table_idx, std::uint32_t pc) const {
  switch (table_idx) {
    case 0: return (pc >> 1) % kScTableSize;
    case 1: return ((pc >> 1) ^ ghr_) % kScTableSize;
    case 2: return (path_history_ >> 1) % kScTableSize;
    case 3: return ghr_ % kScTableSize;
    default: return ((pc >> 1) ^ (ghr_ >> table_idx) ^ path_history_) % kScTableSize;
  }
}

void PredictorOne::Reset() {
  metrics_ = PredictorMetrics();

  // Initialize SC tables (4 tables, each 512 entries initialized to 0)
  sc_tables_.assign(kScNumTables, std::vector<int8_t>(kScTableSize, 0));

  // Initialize Loop Predictor table
  loop_table_.assign(kLoopTableSize, LoopEntry{false, 0, 0, 0, false, 0});

  // Reset history
  ghr_ = 0;
  path_history_ = 0;
}

BranchPrediction PredictorOne::Predict(std::uint32_t pc) {
  ++metrics_.lookups;

  // Placeholder for BTB and TAGE
  bool branch_found = false;
  std::uint32_t target = 0;
  bool tage_taken = false;

  // Step 4 - Statistical Corrector (SC) Lookup
  int sc_sum = 0;

  if (!sc_tables_.empty()) {
    auto get_centered_val = [](int8_t ctr) {
      return 2 * static_cast<int>(ctr) + 1;
    };

    for (size_t t = 0; t < sc_tables_.size(); ++t) {
      sc_sum += get_centered_val(sc_tables_[t][GetScIndex(t, pc)]);
    }
  }

  bool sc_override = false;
  bool final_taken = tage_taken;

  // Override TAGE if SC has strong opposing conviction
  if (tage_taken && sc_sum < -kScThreshold) {
    sc_override = true;
    final_taken = false;
  } else if (!tage_taken && sc_sum > kScThreshold) {
    sc_override = true;
    final_taken = true;
  }

  bool loop_hit = false; // Stub loop logic

  BranchPrediction prediction;
  prediction.branch_found = branch_found;
  prediction.taken = final_taken;
  prediction.target = target;
  prediction.confidence = std::abs(sc_sum);

  // Pack metadata into information field for Update()
  // Pack metadata into information field for Update()
  std::uint64_t metadata = 0;
  if (tage_taken) metadata |= (1ULL << 0);
  if (sc_override) metadata |= (1ULL << 1);
  if (loop_hit)   metadata |= (1ULL << 2);
  // Store sign of sc_sum (positive if >0)
  if (sc_sum > 0) metadata |= (1ULL << 3);
  // Pack SC table indices (9 bits each) starting at bit 4
  for (size_t t = 0; t < kScNumTables; ++t) {
    uint32_t idx = GetScIndex(t, pc);
    metadata |= (static_cast<std::uint64_t>(idx) & 0x1FFULL) << (4 + t * 9);
  }
  prediction.information = metadata;

  if (branch_found) {
    ++metrics_.branches_found;
    if (final_taken)
      ++metrics_.predicted_taken;
    else
      ++metrics_.predicted_not_taken;
  } else {
    ++metrics_.branches_not_found;
  }

  return prediction;
}

void PredictorOne::ObserveInstruction(const Instruction &instruction,
                                      const BranchPrediction &prediction) {
  (void)instruction;
  (void)prediction;
  throw std::runtime_error(
      "PredictorOne::ObserveInstruction: TODO(PA3-PREDICTOR-ONE)");
}

void PredictorOne::Update(const BranchResult &result,
                          const BranchPrediction &prediction) {
  ++metrics_.updates;

  std::uint64_t metadata = prediction.information;
  bool tage_taken = (metadata & (1ULL << 0)) != 0;
  bool sc_override = (metadata & (1ULL << 1)) != 0;
  bool sc_sum_positive = (metadata & (1ULL << 3)) != 0;
  int sc_sum = static_cast<int>(prediction.confidence);
  if (!sc_sum_positive) {
    sc_sum = -sc_sum;
  }

  bool tage_mispredicted = (tage_taken != result.taken);

  // Step 7 - SC Update
  // Train SC if TAGE mispredicted, or if SC override was within threshold margin, or if SC overrode
  if (sc_override || std::abs(sc_sum) < kScThreshold || tage_mispredicted) {
    if (!sc_tables_.empty()) {
      auto clamp = [](int8_t val, int min_val, int max_val) -> int8_t {
        if (val < min_val) return min_val;
        if (val > max_val) return max_val;
        return val;
      };

      std::uint32_t pc = result.pc;
      for (size_t t = 0; t < sc_tables_.size(); ++t) {
        std::uint32_t idx = static_cast<std::uint32_t>((metadata >> (4 + t * 9)) & 0x1FFULL);
        if (result.taken) {
          sc_tables_[t][idx] = clamp(sc_tables_[t][idx] + 1, -32, 31);
        } else {
          sc_tables_[t][idx] = clamp(sc_tables_[t][idx] - 1, -32, 31);
        }
      }
    }
  }

  // Update metrics for accuracy tracking
  if (!prediction.branch_found) {
    ++metrics_.missed_branches;
  } else {
    bool final_pred = (prediction.branch_found && prediction.taken);
    if (final_pred == result.taken) {
      if (result.taken && prediction.target != result.target) {
        ++metrics_.target_mispredictions;
      } else {
        ++metrics_.correct_predictions;
      }
    } else {
      ++metrics_.direction_mispredictions;
    }
  }
}

std::uint64_t PredictorOne::StorageBits() const {
  throw std::runtime_error(
      "PredictorOne::StorageBits: TODO(PA3-PREDICTOR-ONE)");
}

const PredictorMetrics &PredictorOne::Metrics() const { return metrics_; }

} // namespace pipesim
