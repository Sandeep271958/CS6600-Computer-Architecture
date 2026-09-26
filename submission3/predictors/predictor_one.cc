#include "predictors/predictor_one.h"

#include <cstdlib>
#include <stdexcept>

namespace pipesim {

namespace {

constexpr unsigned kScSumMetadataShift = 50;
constexpr std::uint64_t kScSumMetadataMask = 0x1FFULL;

} // namespace

std::string PredictorOne::Name() const { return "PAPER_ONE"; }

std::uint32_t PredictorOne::GetScIndex(size_t table_idx,
                                       std::uint32_t pc) const {

  for (int i = 0; i < 64; ++i) {
    if (ghr_[i])
      ghr_lower |= (1ULL << i);
  }

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

std::uint32_t PredictorOne::Fold(int history_length, int fold_width) const {
  std::uint32_t result = 0;
  for (int i = 0; i < history_length; ++i) {
    if (ghr_[i]) {
      result ^= (1u << (i % fold_width));
    }
  }
  return result & ((1u << fold_width) - 1u);
}

std::uint32_t PredictorOne::GetTageIndex(std::uint32_t pc,
                                         int table_idx) const {
  int n = kIndexWidths[table_idx];
  std::uint32_t csr = Fold(kHistoryLengths[table_idx], n);
  std::uint32_t mask = (1u << n) - 1u;
  // Hash formula from the Workload Guide
  return (pc ^ (pc >> 2) ^ csr ^ path_history_) & mask;
}

std::uint32_t PredictorOne::GetTageTag(std::uint32_t pc, int table_idx) const {
  int tag_width = kTagWidths[table_idx];
  std::uint32_t csr1 = Fold(kHistoryLengths[table_idx], tag_width);
  std::uint32_t csr2 = Fold(kHistoryLengths[table_idx], tag_width - 1);
  std::uint32_t mask = (1u << tag_width) - 1u;
  // Hash formula from the Workload Guide
  return (pc ^ csr1 ^ (csr2 << 1)) & mask;
}

void PredictorOne::Reset() {
  metrics_ = PredictorMetrics();

  // Initialize SC tables (4 tables, each 512 entries initialized to 0)
  sc_tables_.assign(kScNumTables, std::vector<int8_t>(kScTableSize, 0));

  // Initialize Loop Predictor table
  loop_table_.assign(kLoopTableSize, LoopEntry{false, 0, 0, 0, false, 0, 0});

  // Intitalise BTB and TAGE tables
  btb_.assign(kBtbSize, BTBEntry{false, 0, 0});
  base_table_.assign(kBaseSize, BaseEntry{3});
  t1_.assign(kT1Size, TageEntry{false, 0, 3, 0});
  t2_.assign(kT2Size, TageEntry{false, 0, 3, 0});
  t3_.assign(kT3Size, TageEntry{false, 0, 3, 0});
  t4_.assign(kT4Size, TageEntry{false, 0, 3, 0});
  t5_.assign(kT5Size, TageEntry{false, 0, 3, 0});
  t6_.assign(kT6Size, TageEntry{false, 0, 3, 0});

  useful_reset_counter_ = 0;

  // Reset history
  ghr_.reset();
  path_history_ = 0;
}

BranchPrediction PredictorOne::Predict(std::uint32_t pc) {
  ++metrics_.lookups;

  // --- Step 1: BTB Lookup ---
  std::uint32_t btb_idx = (pc >> 2) % kBtbSize;
  std::uint16_t btb_tag = (pc >> 11) & 0x3FFF; // 14-bit tag
  bool branch_found = btb_[btb_idx].valid && (btb_[btb_idx].tag == btb_tag);
  std::uint32_t target = branch_found ? btb_[btb_idx].target : 0;

  // --- Step 2 & 3: TAGE Lookups ---
  int provider = 0; // 0 means base predictor (T0)
  int alt_provider = 0;

  // Store hit information for the tables
  bool hit[7] = {false};
  std::uint32_t indices[7] = {0};
  std::uint32_t tags[7] = {0};

  // Base Predictor (T0) lookup
  indices[0] = (pc >> 2) % kBaseSize;
  bool base_taken = base_table_[indices[0]].ctr >= 4;

  // Query T1 to T6
  for (int i = 1; i <= 6; ++i) {
    indices[i] = GetTageIndex(pc, i);
    tags[i] = GetTageTag(pc, i);

    // Check if valid and tag matches based on the correct table
    if (i == 1)
      hit[i] = t1_[indices[i]].valid && (t1_[indices[i]].tag == tags[i]);
    else if (i == 2)
      hit[i] = t2_[indices[i]].valid && (t2_[indices[i]].tag == tags[i]);
    else if (i == 3)
      hit[i] = t3_[indices[i]].valid && (t3_[indices[i]].tag == tags[i]);
    else if (i == 4)
      hit[i] = t4_[indices[i]].valid && (t4_[indices[i]].tag == tags[i]);
    else if (i == 5)
      hit[i] = t5_[indices[i]].valid && (t5_[indices[i]].tag == tags[i]);
    else if (i == 6)
      hit[i] = t6_[indices[i]].valid && (t6_[indices[i]].tag == tags[i]);
  }

  // Find provider (longest hit) and alt_provider (second longest hit)
  for (int i = 6; i >= 1; --i) {
    if (hit[i]) {
      if (provider == 0) {
        provider = i;
      } else if (alt_provider == 0) {
        alt_provider = i;
        break; // Found both
      }
    }
  }

  // --- Step 4 & 5: Provider Selection ---
  bool tage_taken = base_taken;
  bool use_alt = false;

  if (provider > 0) {
    // Get the counter and useful bit of the provider
    std::uint8_t prov_ctr, prov_u;
    if (provider == 1) {
      prov_ctr = t1_[indices[1]].ctr;
      prov_u = t1_[indices[1]].u;
    } else if (provider == 2) {
      prov_ctr = t2_[indices[2]].ctr;
      prov_u = t2_[indices[2]].u;
    } else if (provider == 3) {
      prov_ctr = t3_[indices[3]].ctr;
      prov_u = t3_[indices[3]].u;
    } else if (provider == 4) {
      prov_ctr = t4_[indices[4]].ctr;
      prov_u = t4_[indices[4]].u;
    } else if (provider == 5) {
      prov_ctr = t5_[indices[5]].ctr;
      prov_u = t5_[indices[5]].u;
    } else {
      prov_ctr = t6_[indices[6]].ctr;
      prov_u = t6_[indices[6]].u;
    }

    bool prov_pred = (prov_ctr >= 4);
    bool prov_is_weak = (prov_ctr == 3 || prov_ctr == 4);

    // If newly allocated (u == 0) and weak, we might use alt_provider instead
    if (prov_u == 0 && prov_is_weak && alt_provider > 0) {
      use_alt = true;

      std::uint8_t alt_ctr;
      if (alt_provider == 1)
        alt_ctr = t1_[indices[1]].ctr;
      else if (alt_provider == 2)
        alt_ctr = t2_[indices[2]].ctr;
      else if (alt_provider == 3)
        alt_ctr = t3_[indices[3]].ctr;
      else if (alt_provider == 4)
        alt_ctr = t4_[indices[4]].ctr;
      else if (alt_provider == 5)
        alt_ctr = t5_[indices[5]].ctr;
      else
        alt_ctr = base_table_[indices[0]]
                      .ctr; // If no alt tagged table, fallback to base

      tage_taken = (alt_ctr >= 4);
    } else {
      tage_taken = prov_pred;
    }
  }

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

  const bool sc_taken = sc_sum > 0;
  const bool sc_override =
      std::abs(sc_sum) > kScThreshold && sc_taken != tage_taken;
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
    metadata |= (1ULL << 2);
  if (sc_sum > 0)
    metadata |= (1ULL << 3);

  // Pack SC table indices
  for (size_t t = 0; t < kScNumTables; ++t) {
    uint32_t idx = GetScIndex(t, pc);
    metadata |= (static_cast<std::uint64_t>(idx) & kScIdxMask)
                << (4 + t * kScIdxBits);
  }

  // Pack loop iter snapshot
  metadata |= (static_cast<std::uint64_t>(loop_iter_snap) & 0x3FFULL) << 40;

  // Pack SC sum
  metadata |= (static_cast<std::uint64_t>(sc_sum) & kScSumMetadataMask)
              << kScSumMetadataShift;

  // Pack TAGE specific metadata
  metadata |= (static_cast<std::uint64_t>(provider) & 0x7ULL)
              << 50; // Bits 50-52 (3 bits)
  metadata |= (static_cast<std::uint64_t>(alt_provider) & 0x7ULL)
              << 53; // Bits 53-55 (3 bits)
  if (use_alt)
    metadata |= (1ULL << 56); // Bit 56
  // ----------------------------------------

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

void PredictorOne::ObserveInstruction(const Instruction &instruction,
                                      const BranchPrediction &prediction) {
  // Just to update metrics and a Sanity Check

  const bool actual_branch = instruction.opcode == Opcode::kBeq ||
                             instruction.opcode == Opcode::kBne ||
                             instruction.opcode == Opcode::kJ;

  if (prediction.branch_found && !actual_branch) {
    ++metrics_.false_branch_predictions;
  }

  else if (!prediction.branch_found && actual_branch) {
    ++metrics_.missed_branches;
  }
}

void PredictorOne::Update(const BranchResult &result,
                          const BranchPrediction &prediction) {
  ++metrics_.updates;

  std::uint64_t metadata = prediction.information;
  bool tage_taken = (metadata & (1ULL << 0)) != 0;
  bool sc_override = (metadata & (1ULL << 1)) != 0;
  int sc_sum =
      static_cast<int>((metadata >> kScSumMetadataShift) & kScSumMetadataMask);
  if ((sc_sum & 0x100) != 0)
    sc_sum -= 0x200;

  bool tage_mispredicted = (tage_taken != result.taken);

  // --- TAGE UPDATE LOGIC (Step 1 to 5) ---
  int provider = static_cast<int>((metadata >> 50) & 0x7ULL);
  int alt_provider = static_cast<int>((metadata >> 53) & 0x7ULL);
  std::uint32_t pc = result.pc;

  // Recompute indices for the tables
  std::uint32_t indices[7];
  indices[0] = (pc >> 2) % kBaseSize;
  for (int i = 1; i <= 6; ++i) {
    indices[i] = GetTageIndex(pc, i);
  }

  // Step 1: BTB Update
  if (result.taken) {
    std::uint32_t btb_idx = (pc >> 2) % kBtbSize;
    btb_[btb_idx].valid = true;
    btb_[btb_idx].tag = (pc >> 11) & 0x3FFF;
    btb_[btb_idx].target = result.target;
  }

  // Step 2 & 3: Provider Counter & Useful Bit Update
  if (provider == 0) {
    if (result.taken)
      base_table_[indices[0]].ctr =
          std::min<std::uint8_t>(base_table_[indices[0]].ctr + 1, 7);
    else if (base_table_[indices[0]].ctr > 0)
      base_table_[indices[0]].ctr--;
  } else {
    TageEntry *prov_entry = nullptr;
    if (provider == 1)
      prov_entry = &t1_[indices[1]];
    else if (provider == 2)
      prov_entry = &t2_[indices[2]];
    else if (provider == 3)
      prov_entry = &t3_[indices[3]];
    else if (provider == 4)
      prov_entry = &t4_[indices[4]];
    else if (provider == 5)
      prov_entry = &t5_[indices[5]];
    else
      prov_entry = &t6_[indices[6]];

    // Update counter
    if (result.taken)
      prov_entry->ctr = std::min<std::uint8_t>(prov_entry->ctr + 1, 7);
    else if (prov_entry->ctr > 0)
      prov_entry->ctr--;

    // Update useful bit
    if (alt_provider != provider) {
      bool prov_correct = (tage_taken == result.taken);
      bool alt_pred = false;
      if (alt_provider == 0)
        alt_pred = (base_table_[indices[0]].ctr >= 4);
      else if (alt_provider == 1)
        alt_pred = (t1_[indices[1]].ctr >= 4);
      else if (alt_provider == 2)
        alt_pred = (t2_[indices[2]].ctr >= 4);
      else if (alt_provider == 3)
        alt_pred = (t3_[indices[3]].ctr >= 4);
      else if (alt_provider == 4)
        alt_pred = (t4_[indices[4]].ctr >= 4);
      else if (alt_provider == 5)
        alt_pred = (t5_[indices[5]].ctr >= 4);
      else
        alt_pred = (t6_[indices[6]].ctr >= 4);

      bool alt_correct = (alt_pred == result.taken);
      if (prov_correct && !alt_correct)
        prov_entry->u = std::min<std::uint8_t>(prov_entry->u + 1, 3);
      else if (!prov_correct && alt_correct && prov_entry->u > 0)
        prov_entry->u--;
    }
  }

  // Step 4: Allocation on Misprediction
  if (tage_mispredicted && provider < 6) {
    std::vector<int> candidates;
    for (int i = provider + 1; i <= 6; ++i) {
      std::uint8_t u_val = 0;
      if (i == 1)
        u_val = t1_[indices[1]].u;
      else if (i == 2)
        u_val = t2_[indices[2]].u;
      else if (i == 3)
        u_val = t3_[indices[3]].u;
      else if (i == 4)
        u_val = t4_[indices[4]].u;
      else if (i == 5)
        u_val = t5_[indices[5]].u;
      else
        u_val = t6_[indices[6]].u;

      if (u_val == 0)
        candidates.push_back(i);
    }

    if (candidates.empty()) {
      // Graceful decay: decrement u bits across candidate tables
      for (int i = provider + 1; i <= 6; ++i) {
        if (i == 1 && t1_[indices[1]].u > 0)
          t1_[indices[1]].u--;
        else if (i == 2 && t2_[indices[2]].u > 0)
          t2_[indices[2]].u--;
        else if (i == 3 && t3_[indices[3]].u > 0)
          t3_[indices[3]].u--;
        else if (i == 4 && t4_[indices[4]].u > 0)
          t4_[indices[4]].u--;
        else if (i == 5 && t5_[indices[5]].u > 0)
          t5_[indices[5]].u--;
        else if (i == 6 && t6_[indices[6]].u > 0)
          t6_[indices[6]].u--;
      }
    } else {
      // Pick the longest history candidate to allocate
      int chosen = candidates.back();
      TageEntry *chosen_entry = nullptr;
      if (chosen == 1)
        chosen_entry = &t1_[indices[1]];
      else if (chosen == 2)
        chosen_entry = &t2_[indices[2]];
      else if (chosen == 3)
        chosen_entry = &t3_[indices[3]];
      else if (chosen == 4)
        chosen_entry = &t4_[indices[4]];
      else if (chosen == 5)
        chosen_entry = &t5_[indices[5]];
      else
        chosen_entry = &t6_[indices[6]];

      chosen_entry->valid = true;
      chosen_entry->tag = GetTageTag(pc, chosen);
      chosen_entry->ctr = result.taken ? 4 : 3; // Weakly taken or not-taken
      chosen_entry->u = 0;
    }
  }

  // Step 5: Periodic Useful-Bit Reset
  useful_reset_counter_++;
  if (useful_reset_counter_ == 256 * 1024) {
    for (auto &e : t1_)
      e.u &= 0b01;
    for (auto &e : t2_)
      e.u &= 0b01;
    for (auto &e : t3_)
      e.u &= 0b01;
    for (auto &e : t4_)
      e.u &= 0b01;
    for (auto &e : t5_)
      e.u &= 0b01;
    for (auto &e : t6_)
      e.u &= 0b01;
  } else if (useful_reset_counter_ == 512 * 1024) {
    for (auto &e : t1_)
      e.u &= 0b10;
    for (auto &e : t2_)
      e.u &= 0b10;
    for (auto &e : t3_)
      e.u &= 0b10;
    for (auto &e : t4_)
      e.u &= 0b10;
    for (auto &e : t5_)
      e.u &= 0b10;
    for (auto &e : t6_)
      e.u &= 0b10;
    useful_reset_counter_ = 0;
  }
  // ---------------------------------------

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

      (void)result
          .pc; // pc unused; suppress warning if needed in future extensions
      for (size_t t = 0; t < sc_tables_.size(); ++t) {
        // Unpack using same kScIdxBits/kScIdxMask from header
        std::uint32_t idx = static_cast<std::uint32_t>(
            (metadata >> (4 + t * kScIdxBits)) & kScIdxMask);
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
  std::uint32_t loop_iter_at_predict =
      static_cast<std::uint32_t>((metadata >> 40) & 0x3FFULL);

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
  ghr_ <<= 1;
  ghr_.set(0, result.taken);

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

  const std::uint64_t sc_bits = kScNumTables * kScTableSize * kScCounterBits;
  const std::uint64_t loop_entry_bits = 1 + kLoopTagBits + kLoopIterBits +
                                        kLoopTripBits + 1 + kLoopAgeBits +
                                        kLoopConfidenceSeenBits;
  const std::uint64_t loop_bits = kLoopTableSize * loop_entry_bits;
  const std::uint64_t history_bits =
      sizeof(ghr_) * 8 + sizeof(path_history_) * 8;

  return sc_bits + loop_bits + history_bits;
}

const PredictorMetrics &PredictorOne::Metrics() const { return metrics_; }

} // namespace pipesim
