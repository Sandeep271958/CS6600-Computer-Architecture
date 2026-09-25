#include "predictors/predictor_one.h"

#include <cstdlib>
#include <stdexcept>

namespace pipesim {

namespace {

constexpr unsigned kScSumMetadataShift = 50;
constexpr std::uint64_t kScSumMetadataMask = 0x1FFULL;

}

std::string PredictorOne::Name() const { return "PAPER_ONE"; }

std::uint32_t PredictorOne::GetScIndex(size_t table_idx, std::uint32_t pc) const {
  switch (table_idx) {
  case 0:
    return (pc >> 1) % kScTableSize;
  case 1:
    return ((pc >> 1) ^ ghr_) % kScTableSize;
  case 2:
    return (path_history_ >> 1) % kScTableSize;
  case 3:
    return ghr_ % kScTableSize;
  default:
    return ((pc >> 1) ^ (ghr_ >> table_idx) ^ path_history_) % kScTableSize;
  }
}

std::uint32_t PredictorOne::GetLoopIndex(std::uint32_t pc) const {
  // Use PC[8:3] (6 bits) for 64-entry table
  return (pc >> 3) % kLoopTableSize;
}

std::uint32_t PredictorOne::GetLoopTag(std::uint32_t pc) const {
  // Use PC[15:3] (13 bits) as tag
  return (pc >> 3) & ((1u << kLoopTagBits) - 1u);
}

void PredictorOne::Reset() {
  metrics_ = PredictorMetrics();

  // Initialize SC tables (4 tables, each 512 entries initialized to 0)
  sc_tables_.assign(kScNumTables, std::vector<int8_t>(kScTableSize, 0));

  // Initialize Loop Predictor table
  loop_table_.assign(kLoopTableSize, LoopEntry{false, 0, 0, 0, false, 0, 0});

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
    auto get_centered_val = [](int8_t ctr) { return 2 * static_cast<int>(ctr) + 1; };

    for (size_t t = 0; t < sc_tables_.size(); ++t) {
      sc_sum += get_centered_val(sc_tables_[t][GetScIndex(t, pc)]);
    }
  }

  const bool sc_taken = sc_sum > 0;
  const bool sc_override = std::abs(sc_sum) > kScThreshold &&
                           sc_taken != tage_taken;
  const bool tage_or_sc_taken = sc_override ? sc_taken : tage_taken;

  // Step 5 - Loop Predictor Lookup
  std::uint32_t loop_idx = GetLoopIndex(pc);
  LoopEntry &le = loop_table_[loop_idx];
  bool loop_hit = le.valid && (le.tag == GetLoopTag(pc)) && le.confidence;
  bool loop_pred = false;
  std::uint32_t loop_iter_snap = 0;
  if (loop_hit) {
    loop_iter_snap = le.iter;
    // iter counts taken-branches completed so far in this invocation.
    // Predict taken while iter < trip-1; not-taken on the exit iteration
    // (iter == trip-1 means this is the last taken, next is exit).
    loop_pred = (le.iter + 1 < le.trip);
    // Do NOT speculatively advance iter here — Update will do it after
    // the real outcome is known, avoiding corruption on mispredictions.
  }

  // Final taken: loop overrides SC/TAGE when confident
  const bool final_taken_with_loop = loop_hit ? loop_pred : tage_or_sc_taken;

  BranchPrediction prediction;
  prediction.branch_found = branch_found;
  prediction.taken = final_taken_with_loop;
  prediction.target = target;
  prediction.confidence = static_cast<std::uint32_t>(std::abs(sc_sum));

  // Pack metadata into information field for Update()
  std::uint64_t metadata = 0;
  if (tage_taken)
    metadata |= (1ULL << 0);
  if (sc_override)
    metadata |= (1ULL << 1);
  if (loop_hit)
    metadata |= (1ULL << 2); // loop predictor was active
  // Pack SC table indices — kScIdxBits/kScIdxMask derived from kScTableSize in
  // header
  for (size_t t = 0; t < kScNumTables; ++t) {
    uint32_t idx = GetScIndex(t, pc);
    metadata |= (static_cast<std::uint64_t>(idx) & kScIdxMask) << (4 + t * kScIdxBits);
  }
  // Pack loop iter snapshot (10 bits) at bit 40
  metadata |= (static_cast<std::uint64_t>(loop_iter_snap) & 0x3FFULL) << 40;
  metadata |= (static_cast<std::uint64_t>(sc_sum) & kScSumMetadataMask)
              << kScSumMetadataShift;
  prediction.information = metadata;

  if (branch_found) {
    ++metrics_.branches_found;
    if (final_taken_with_loop)
      ++metrics_.predicted_taken;
    else
      ++metrics_.predicted_not_taken;
  } else {
    ++metrics_.branches_not_found;
  }

  return prediction;
}

void PredictorOne::ObserveInstruction(const Instruction &instruction, const BranchPrediction &prediction) {
  // Just to update metrics and a Sanity Check

  const bool actual_branch = instruction.opcode == Opcode::kBeq || instruction.opcode == Opcode::kBne ||
                             instruction.opcode == Opcode::kJ;

  if (prediction.branch_found && !actual_branch) {
    ++metrics_.false_branch_predictions;
  }

  else if (!prediction.branch_found && actual_branch) {
    ++metrics_.missed_branches;
  }
}

void PredictorOne::Update(const BranchResult &result, const BranchPrediction &prediction) {
  ++metrics_.updates;

  std::uint64_t metadata = prediction.information;
  bool tage_taken = (metadata & (1ULL << 0)) != 0;
  bool sc_override = (metadata & (1ULL << 1)) != 0;
  int sc_sum = static_cast<int>(
      (metadata >> kScSumMetadataShift) & kScSumMetadataMask);
  if ((sc_sum & 0x100) != 0) sc_sum -= 0x200;

  bool tage_mispredicted = (tage_taken != result.taken);

  // Step 7 - SC Update
  // Train SC if TAGE mispredicted, or if SC override was within threshold
  // margin, or if SC overrode
  if (sc_override || std::abs(sc_sum) < kScThreshold || tage_mispredicted) {
    if (!sc_tables_.empty()) {
      auto clamp = [](int8_t val, int min_val, int max_val) -> int8_t {
        if (val < min_val)
          return min_val;
        if (val > max_val)
          return max_val;
        return val;
      };

      (void)result.pc; // pc unused; suppress warning if needed in future extensions
      for (size_t t = 0; t < sc_tables_.size(); ++t) {
        // Unpack using same kScIdxBits/kScIdxMask from header
        std::uint32_t idx = static_cast<std::uint32_t>((metadata >> (4 + t * kScIdxBits)) & kScIdxMask);
        if (result.taken) {
          sc_tables_[t][idx] = clamp(sc_tables_[t][idx] + 1, -32, 31);
        } else {
          sc_tables_[t][idx] = clamp(sc_tables_[t][idx] - 1, -32, 31);
        }
      }
    }
  }

  // Step 8 - Loop Predictor Update
  bool loop_hit_saved = (metadata & (1ULL << 2)) != 0;
  std::uint32_t loop_iter_at_predict = static_cast<std::uint32_t>((metadata >> 40) & 0x3FFULL);

  std::uint32_t lpc = result.pc;
  std::uint32_t l_idx = GetLoopIndex(lpc);
  std::uint32_t l_tag = GetLoopTag(lpc);
  LoopEntry &entry = loop_table_[l_idx];

  if (entry.valid && entry.tag == l_tag) {
    // Existing entry for this PC — advance iter based on actual outcome.
    if (result.taken) {
      // Still looping: commit the increment.
      entry.iter = loop_iter_at_predict + 1;

      if (entry.confidence && entry.iter >= entry.trip) {
        // Confident loop predicted exit but branch took again — trip changed.
        entry.confidence = false;
        entry.confidence_seen = 0;
        entry.trip = 0;
        entry.iter = 0;
      }
    } else {
      // Branch not-taken = loop exited; actual trip = iter_at_predict + 1
      // (one more taken iteration was completed before this exit).
      std::uint32_t actual_trip = loop_iter_at_predict + 1;
      if (!entry.confidence) {
        // Learning phase: check if trip count has stabilised.
        if (entry.trip == actual_trip) {
          if (++entry.confidence_seen >= kLoopConfidenceThreshold) {
            entry.confidence = true;
            entry.confidence_seen = 0;
          }
        } else {
          // New trip count observed — restart learning.
          entry.trip = actual_trip;
          entry.confidence_seen = 1;
        }
      } else {
        // Confident mode: verify trip count still matches.
        if (entry.trip != actual_trip) {
          entry.confidence = false;
          entry.trip = actual_trip;
          entry.confidence_seen = 1;
        }
      }
      entry.iter = 0; // reset counter for next loop invocation
    }
  } else {
    // No valid entry for this PC — allocate if slot is free or aged out.
    if (!entry.valid || entry.age == 0) {
      entry.valid = true;
      entry.tag = l_tag;
      entry.confidence = false;
      entry.confidence_seen = 0;
      entry.age = kLoopAgeMax;
      if (result.taken) {
        entry.iter = 1; // first iteration completed
        entry.trip = 0; // unknown yet
      } else {
        entry.iter = 0;
        entry.trip = 1; // exited immediately — trip count is 1
      }
    } else {
      // Age down the existing unrelated entry before evicting.
      entry.age -= 1;
    }
  }

  // Step 9 - Update global history registers
  ghr_ = (ghr_ << 1) | (result.taken ? 1ULL : 0ULL);
  path_history_ = (path_history_ << 1) | ((result.pc >> 1) & 1u);

  // Update metrics for accuracy tracking
  (void)loop_hit_saved; // captured in metadata; silence unused-variable warning
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
  constexpr std::uint64_t kScCounterBits = 6;
  constexpr std::uint64_t kLoopIterBits = 10;
  constexpr std::uint64_t kLoopTripBits = 10;
  constexpr std::uint64_t kLoopAgeBits = 3;
  constexpr std::uint64_t kLoopConfidenceSeenBits = 2;

  const std::uint64_t sc_bits =
      kScNumTables * kScTableSize * kScCounterBits;
  const std::uint64_t loop_entry_bits =
      1 + kLoopTagBits + kLoopIterBits + kLoopTripBits + 1 + kLoopAgeBits +
      kLoopConfidenceSeenBits;
  const std::uint64_t loop_bits = kLoopTableSize * loop_entry_bits;
  const std::uint64_t history_bits =
      sizeof(ghr_) * 8 + sizeof(path_history_) * 8;

  return sc_bits + loop_bits + history_bits;
}

const PredictorMetrics &PredictorOne::Metrics() const { return metrics_; }

} // namespace pipesim
