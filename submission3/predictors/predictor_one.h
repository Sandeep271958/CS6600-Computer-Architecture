#pragma once

#include "branch_prediction.h"
#include <bitset>
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
    std::uint32_t iter; // taken-branch count within current loop invocation
    std::uint32_t trip; // learned trip count (exit on iter == trip)
    bool confidence;    // true once trip count has stabilised
    std::uint8_t age;   // eviction age counter
    std::uint8_t confidence_seen; // consecutive matching trip observations
  };

  // BTB and TAGE Structures
  struct BTBEntry {
    bool valid = false;
    std::uint16_t tag = 0;
    std::uint32_t target = 0;
  };

  struct BaseEntry {
    std::uint8_t ctr = 3; // 3 bit counter :- 000-111
  };

  struct TageEntry {
    bool valid = false;
    std::uint16_t tag = 0;
    std::uint8_t ctr = 3; // 3 bit counter: 0-7
    std::uint8_t u = 0;   // 2 bit useful counter: 0-3
  };

  // Table Dimensions from the architectural guide
  static constexpr size_t kBtbSize = 512;
  static constexpr size_t kBaseSize = 4096;
  static constexpr size_t kT1Size = 512;
  static constexpr size_t kT2Size = 512;
  static constexpr size_t kT3Size = 512;
  static constexpr size_t kT4Size = 512;
  static constexpr size_t kT5Size = 256;
  static constexpr size_t kT6Size = 128;

  std::vector<BTBEntry> btb_;
  std::vector<BaseEntry> base_table_;

  // Tagged tables
  std::vector<TageEntry> t1_;
  std::vector<TageEntry> t2_;
  std::vector<TageEntry> t3_;
  std::vector<TageEntry> t4_;
  std::vector<TageEntry> t5_;
  std::vector<TageEntry> t6_;
  std::uint32_t useful_reset_counter_ = 0;

  // TAGE configurations
  static constexpr int kHistoryLengths[7] = {0, 6, 11, 20, 37, 68, 130};
  static constexpr int kIndexWidths[7] = {12, 9, 9, 9, 9, 8, 7};
  static constexpr int kTagWidths[7] = {0, 8, 8, 9, 10, 10, 11};

  std::uint32_t Fold(int history_length, int fold_width) const;
  std::uint32_t GetTageIndex(std::uint32_t pc, int table_idx) const;
  std::uint32_t GetTageTag(std::uint32_t pc, int table_idx) const;

  // Statistical Corrector sizing
  static constexpr size_t kScNumTables = 4;
  static constexpr size_t kScTableSize = 512;
  static constexpr int kScThreshold = 22;
  // Derived: bit-width of one SC index for metadata packing (log2 of
  // kScTableSize)
  static constexpr int kScIdxBits = (kScTableSize <= 1)      ? 0
                                    : (kScTableSize <= 2)    ? 1
                                    : (kScTableSize <= 4)    ? 2
                                    : (kScTableSize <= 8)    ? 3
                                    : (kScTableSize <= 16)   ? 4
                                    : (kScTableSize <= 32)   ? 5
                                    : (kScTableSize <= 64)   ? 6
                                    : (kScTableSize <= 128)  ? 7
                                    : (kScTableSize <= 256)  ? 8
                                    : (kScTableSize <= 512)  ? 9
                                    : (kScTableSize <= 1024) ? 10
                                                             : 11;
  static constexpr std::uint64_t kScIdxMask = kScTableSize - 1;
  std::vector<std::vector<int8_t>> sc_tables_;

  // Loop Predictor sizing
  static constexpr size_t kLoopTableSize = 64;
  static constexpr std::uint32_t kLoopTagBits = 13; // PC[15:3]
  static constexpr std::uint8_t kLoopAgeMax = 7;    // 3-bit age counter
  static constexpr std::uint32_t kLoopConfidenceThreshold =
      4; // need 3 consistent trips
  std::vector<LoopEntry> loop_table_;

  // History registers
  std::bitset<130> ghr_;
  std::uint32_t path_history_ = 0;

  // Helpers
  std::uint32_t GetScIndex(size_t table_idx, std::uint32_t pc) const;
  std::uint32_t GetLoopIndex(std::uint32_t pc) const;
  std::uint32_t GetLoopTag(std::uint32_t pc) const;
};

} // namespace pipesim
