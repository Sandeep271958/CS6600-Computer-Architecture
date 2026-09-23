#include "predictors/predictor_one.h"

#include <stdexcept>

namespace pipesim {

std::string PredictorOne::Name() const { return "PAPER_ONE"; }

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

    // T0: PC only (bits 9:1)
    sc_sum += get_centered_val(sc_tables_[0][(pc >> 1) % kScTableSize]);
    // T1: GHR XOR PC
    sc_sum +=
        get_centered_val(sc_tables_[1][((pc >> 1) ^ ghr_) % kScTableSize]);
    // T2: Path history
    sc_sum +=
        get_centered_val(sc_tables_[2][(path_history_ >> 1) % kScTableSize]);
    // T3: GHR 20-bit
    sc_sum += get_centered_val(sc_tables_[3][ghr_ % kScTableSize]);
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
  std::uint64_t metadata = 0;
  if (tage_taken)
    metadata |= (1ULL << 0);
  if (sc_override)
    metadata |= (1ULL << 1);
  if (loop_hit)
    metadata |= (1ULL << 2);
  if (sc_sum > 0)
    metadata |= (1ULL << 3);
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
  (void)result;
  (void)prediction;
  throw std::runtime_error("PredictorOne::Update: TODO(PA3-PREDICTOR-ONE)");
}

std::uint64_t PredictorOne::StorageBits() const {
  throw std::runtime_error(
      "PredictorOne::StorageBits: TODO(PA3-PREDICTOR-ONE)");
}

const PredictorMetrics &PredictorOne::Metrics() const { return metrics_; }

} // namespace pipesim
