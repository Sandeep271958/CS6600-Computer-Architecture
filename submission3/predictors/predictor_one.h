#pragma once

#include "branch_prediction.h"
#include <cstdint>
#include <vector>


namespace pipesim {

class PredictorOne final : public BranchPredictor {
public:
  std::string Name() const override;
  void Reset() override;
  BranchPrediction Predict(std::uint32_t pc) override;
  void ObserveInstruction(const Instruction &instruction,
                          const BranchPrediction &prediction) override;
  void Update(const BranchResult &result,
              const BranchPrediction &prediction) override;
  std::uint64_t StorageBits() const override;
  const PredictorMetrics &Metrics() const override;

private:
  PredictorMetrics metrics_;

  struct LoopEntry {
    bool valid;
    std::uint32_t tag;
    std::uint32_t iter;
    std::uint32_t trip;
    bool confidence;
    std::uint8_t age;
  };

  // Statistical Corrector sizing
  static constexpr size_t kScNumTables = 4;
  static constexpr size_t kScTableSize = 512;
  static constexpr int kScThreshold = 18;
  std::vector<std::vector<int8_t>> sc_tables_;

  // Loop Predictor sizing
  static constexpr size_t kLoopTableSize = 64;
  std::vector<LoopEntry> loop_table_;

  // History registers
  std::uint64_t ghr_ = 0;
  std::uint32_t path_history_ = 0;

  // Helpers
  std::uint32_t GetScIndex(size_t table_idx, std::uint32_t pc) const;
};

} // namespace pipesim
