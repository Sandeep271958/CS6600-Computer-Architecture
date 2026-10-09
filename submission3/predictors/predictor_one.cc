#include "predictors/predictor_one.h"

#include <cstdlib>
#include <stdexcept>

namespace pipesim {

namespace {

constexpr unsigned kScSumMetadataShift = 50;
constexpr std::uint64_t kScSumMetadataMask = 0x1FFULL;

} // namespace

PredictorOne::~PredictorOne() {
  auto pct = [](uint64_t c, uint64_t s) -> std::string {
    if (s == 0) return "N/A";
    char buf[32]; snprintf(buf, sizeof(buf), "%.1f%%", 100.0 * c / s);
    return buf;
  };
  fprintf(stderr, "COMPONENT_STATS_BEGIN\n");
  fprintf(stderr, "T0 selected=%llu correct=%llu acc=%s reads=%llu writes=%llu\n",
          (unsigned long long)cs_t0_.selected, (unsigned long long)cs_t0_.correct, pct(cs_t0_.correct, cs_t0_.selected).c_str(),
          (unsigned long long)cs_t0_.reads, (unsigned long long)cs_t0_.writes);
  fprintf(stderr, "T1 selected=%llu correct=%llu acc=%s reads=%llu writes=%llu\n",
          (unsigned long long)cs_t1_.selected, (unsigned long long)cs_t1_.correct, pct(cs_t1_.correct, cs_t1_.selected).c_str(),
          (unsigned long long)cs_t1_.reads, (unsigned long long)cs_t1_.writes);
  fprintf(stderr, "T2 selected=%llu correct=%llu acc=%s reads=%llu writes=%llu\n",
          (unsigned long long)cs_t2_.selected, (unsigned long long)cs_t2_.correct, pct(cs_t2_.correct, cs_t2_.selected).c_str(),
          (unsigned long long)cs_t2_.reads, (unsigned long long)cs_t2_.writes);
  fprintf(stderr, "T3 selected=%llu correct=%llu acc=%s reads=%llu writes=%llu\n",
          (unsigned long long)cs_t3_.selected, (unsigned long long)cs_t3_.correct, pct(cs_t3_.correct, cs_t3_.selected).c_str(),
          (unsigned long long)cs_t3_.reads, (unsigned long long)cs_t3_.writes);
  fprintf(stderr, "T4 selected=%llu correct=%llu acc=%s reads=%llu writes=%llu\n",
          (unsigned long long)cs_t4_.selected, (unsigned long long)cs_t4_.correct, pct(cs_t4_.correct, cs_t4_.selected).c_str(),
          (unsigned long long)cs_t4_.reads, (unsigned long long)cs_t4_.writes);
  fprintf(stderr, "T5 selected=%llu correct=%llu acc=%s reads=%llu writes=%llu\n",
          (unsigned long long)cs_t5_.selected, (unsigned long long)cs_t5_.correct, pct(cs_t5_.correct, cs_t5_.selected).c_str(),
          (unsigned long long)cs_t5_.reads, (unsigned long long)cs_t5_.writes);
  fprintf(stderr, "T6 selected=%llu correct=%llu acc=%s reads=%llu writes=%llu\n",
          (unsigned long long)cs_t6_.selected, (unsigned long long)cs_t6_.correct, pct(cs_t6_.correct, cs_t6_.selected).c_str(),
          (unsigned long long)cs_t6_.reads, (unsigned long long)cs_t6_.writes);
  fprintf(stderr, "SC selected=%llu correct=%llu acc=%s reads=%llu writes=%llu\n",
          (unsigned long long)cs_sc_.selected, (unsigned long long)cs_sc_.correct, pct(cs_sc_.correct, cs_sc_.selected).c_str(),
          (unsigned long long)cs_sc_.reads, (unsigned long long)cs_sc_.writes);
  fprintf(stderr, "BTB selected=%llu correct=%llu acc=%s reads=%llu writes=%llu\n",
          (unsigned long long)cs_btb_.selected, (unsigned long long)cs_btb_.correct, pct(cs_btb_.correct, cs_btb_.selected).c_str(),
          (unsigned long long)cs_btb_.reads, (unsigned long long)cs_btb_.writes);
  fprintf(stderr, "Loop selected=%llu correct=%llu acc=%s reads=%llu writes=%llu\n",
          (unsigned long long)cs_loop_.selected, (unsigned long long)cs_loop_.correct, pct(cs_loop_.correct, cs_loop_.selected).c_str(),
          (unsigned long long)cs_loop_.reads, (unsigned long long)cs_loop_.writes);
  fprintf(stderr, "COMPONENT_STATS_END\n");
}

std::string PredictorOne::Name() const { return "PAPER_ONE"; }

std::uint32_t PredictorOne::GetScIndex(size_t table_idx,
                                       std::uint32_t pc) const {
  std::uint64_t ghr_lower = 0;
  for (int i = 0; i < 64; ++i) {
    if (ghr_[i])
      ghr_lower |= (1ULL << i);
  }

  switch (table_idx) {
  case 0:
    return (pc >> 1) % kScTableSize;
  case 1:
    return ((pc >> 1) ^ ghr_lower) % kScTableSize;
  case 2:
    return (path_history_ >> 1) % kScTableSize;
  case 3:
    return ghr_lower % kScTableSize;
  default:
    return ((pc >> 1) ^ (ghr_lower >> table_idx) ^ path_history_) %
           kScTableSize;
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
  return (pc ^ csr1 ^ (csr2 << 1) ^ path_history_) & mask;
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
  use_alt_on_na_ = 8;
  sc_threshold_ = kScThreshold;
  sc_tc_ = 0;

  // Reset per-component instrumentation counters
  cs_t0_ = cs_t1_ = cs_t2_ = cs_t3_ = cs_t4_ = cs_t5_ = cs_t6_ = {};
  cs_sc_ = cs_loop_ = cs_btb_ = {};

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

  // --- Account reads for this prediction ---
  // BTB: 1 read. T0: 1 read. T1-T6: 1 read each. SC: kScNumTables reads. Loop: 1 read.
  cs_btb_.reads++;
  cs_t0_.reads++;
  cs_t1_.reads++; cs_t2_.reads++; cs_t3_.reads++;
  cs_t4_.reads++; cs_t5_.reads++; cs_t6_.reads++;
  cs_sc_.reads += kScNumTables;
  cs_loop_.reads++;

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
  bool provider_taken = base_taken;
  bool alternate_taken = base_taken;

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
    provider_taken = prov_pred;
    bool prov_is_weak = (prov_ctr == 3 || prov_ctr == 4);

    std::uint8_t alt_ctr = base_table_[indices[0]].ctr;
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
    else if (alt_provider == 6)
      alt_ctr = t6_[indices[6]].ctr;
    alternate_taken = (alt_ctr >= 4);

    // For a weak, newly allocated provider, the alternate is the next tagged
    // hit or T0 when no second tagged table hit.  T0 is a real alternate, not
    // a sentinel meaning "no alternate".
    if (prov_u == 0 && prov_is_weak) {
      tage_taken = (use_alt_on_na_ >= 8) ? alternate_taken : prov_pred;
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
      std::abs(sc_sum) > sc_threshold_ && sc_taken != tage_taken;
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
  unsigned index_shift = 0;
  for (int i = 0; i <= 6; ++i) {
    const std::uint64_t mask = (1ULL << kIndexWidths[i]) - 1ULL;
    prediction.tage_indices |=
        (static_cast<std::uint64_t>(indices[i]) & mask) << index_shift;
    index_shift += static_cast<unsigned>(kIndexWidths[i]);
  }
  unsigned tag_shift = 0;
  for (int i = 1; i <= 6; ++i) {
    const std::uint64_t mask = (1ULL << kTagWidths[i]) - 1ULL;
    prediction.tage_tags |=
        (static_cast<std::uint64_t>(tags[i]) & mask) << tag_shift;
    tag_shift += static_cast<unsigned>(kTagWidths[i]);
  }
  prediction.tage_provider_context =
      static_cast<std::uint8_t>(provider & 0x7) |
      static_cast<std::uint8_t>((alt_provider & 0x7) << 3) |
      static_cast<std::uint8_t>(provider_taken ? 0x40 : 0) |
      static_cast<std::uint8_t>(alternate_taken ? 0x80 : 0);

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

  prediction.information = metadata;

  // Pack which component provided the final prediction into bits 60-63:
  // 0=T0, 1..6=T1..T6, 7=SC override, 8=Loop
  uint64_t comp_id = 0;
  if (loop_hit) {
    comp_id = 8;
  } else if (sc_override) {
    comp_id = 7;
  } else {
    comp_id = static_cast<uint64_t>(provider); // 0 = T0, 1-6 = T1-T6
  }
  metadata |= (comp_id & 0xFULL) << 60;
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
    const std::uint32_t btb_idx =
        (instruction.pc >> 2) % static_cast<std::uint32_t>(kBtbSize);
    const std::uint16_t btb_tag =
        static_cast<std::uint16_t>((instruction.pc >> 11) & 0x3FFF);
    if (btb_[btb_idx].valid && btb_[btb_idx].tag == btb_tag) {
      btb_[btb_idx].valid = false;
      ++cs_btb_.writes;
    }
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
  std::uint32_t pc = result.pc;

  // Use the indices and provider captured at prediction time.  Recomputing
  // them here would use newer global/path history if an older branch resolved
  // in the meantime and could train an entry that did not make the prediction.
  std::uint32_t indices[7];
  unsigned index_shift = 0;
  for (int i = 0; i <= 6; ++i) {
    const std::uint64_t mask = (1ULL << kIndexWidths[i]) - 1ULL;
    indices[i] = static_cast<std::uint32_t>(
        (prediction.tage_indices >> index_shift) & mask);
    index_shift += static_cast<unsigned>(kIndexWidths[i]);
  }
  const int provider = prediction.tage_provider_context & 0x7;
  const int alt_provider = (prediction.tage_provider_context >> 3) & 0x7;
  const bool provider_taken_saved =
      (prediction.tage_provider_context & 0x40) != 0;
  const bool alternate_taken_saved =
      (prediction.tage_provider_context & 0x80) != 0;

  // Step 1: BTB Update.  Learn branch identity even when this particular
  // execution is not taken; the control unit still supplies the static target.
  std::uint32_t btb_idx = (pc >> 2) % kBtbSize;
  btb_[btb_idx].valid = true;
  btb_[btb_idx].tag = (pc >> 11) & 0x3FFF;
  btb_[btb_idx].target = result.target;
  cs_btb_.writes++;

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

    // Dynamic TAGE tuning: update use_alt_on_na_ based on old state
    if (prov_entry->u == 0 && (prov_entry->ctr == 3 || prov_entry->ctr == 4)) {
      const bool prov_pred = provider_taken_saved;
      const bool alt_pred = alternate_taken_saved;

      if (prov_pred != alt_pred) {
        if (alt_pred == result.taken) {
          if (use_alt_on_na_ < 15) use_alt_on_na_++;
        } else {
          if (use_alt_on_na_ > 0) use_alt_on_na_--;
        }
      }
    }

    // Update counter
    if (result.taken)
      prov_entry->ctr = std::min<std::uint8_t>(prov_entry->ctr + 1, 7);
    else if (prov_entry->ctr > 0)
      prov_entry->ctr--;

    // Update useful bit
    if (alt_provider != provider) {
      const bool prov_correct =
          (provider_taken_saved == result.taken);
      const bool alt_pred = alternate_taken_saved;

      bool alt_correct = (alt_pred == result.taken);
      if (prov_correct && !alt_correct)
        prov_entry->u = std::min<std::uint8_t>(prov_entry->u + 1, 3);
      else if (!prov_correct && alt_correct && prov_entry->u > 0)
        prov_entry->u--;
    }
  }

  // --- Account writes for this update ---
  // T0 is written when it is the provider.  A tagged provider is written once
  // here; allocation/decay writes are accounted below.
  // SC: conditionally written (kScNumTables writes). Loop: always 1 write.
  // BTB: 1 write for every resolved branch.
  cs_t0_.writes += (provider == 0) ? 1 : 0;
  if (provider == 1) cs_t1_.writes++;
  else if (provider == 2) cs_t2_.writes++;
  else if (provider == 3) cs_t3_.writes++;
  else if (provider == 4) cs_t4_.writes++;
  else if (provider == 5) cs_t5_.writes++;
  else if (provider == 6) cs_t6_.writes++;
  if (sc_override || std::abs(sc_sum) < sc_threshold_ || tage_mispredicted)
    cs_sc_.writes += kScNumTables;
  cs_loop_.writes++;

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
      if ((useful_reset_counter_ & 127) == 0) {
        // Force allocation randomly to break lockout
        candidates.push_back(provider + 1);
      }
    }

    if (candidates.empty()) {
      // Graceful decay: decrement u bits across candidate tables
      for (int i = provider + 1; i <= 6; ++i) {
        if (i == 1 && t1_[indices[1]].u > 0) {
          t1_[indices[1]].u--;
          cs_t1_.writes++;
        } else if (i == 2 && t2_[indices[2]].u > 0) {
          t2_[indices[2]].u--;
          cs_t2_.writes++;
        } else if (i == 3 && t3_[indices[3]].u > 0) {
          t3_[indices[3]].u--;
          cs_t3_.writes++;
        } else if (i == 4 && t4_[indices[4]].u > 0) {
          t4_[indices[4]].u--;
          cs_t4_.writes++;
        } else if (i == 5 && t5_[indices[5]].u > 0) {
          t5_[indices[5]].u--;
          cs_t5_.writes++;
        } else if (i == 6 && t6_[indices[6]].u > 0) {
          t6_[indices[6]].u--;
          cs_t6_.writes++;
        }
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
      unsigned tag_shift = 0;
      for (int i = 1; i < chosen; ++i)
        tag_shift += static_cast<unsigned>(kTagWidths[i]);
      const std::uint64_t tag_mask =
          (1ULL << kTagWidths[chosen]) - 1ULL;
      chosen_entry->tag = static_cast<std::uint16_t>(
          (prediction.tage_tags >> tag_shift) & tag_mask);
      chosen_entry->ctr = result.taken ? 4 : 3; // Weakly taken or not-taken
      chosen_entry->u = 0;
      if (chosen == 1) cs_t1_.writes++;
      else if (chosen == 2) cs_t2_.writes++;
      else if (chosen == 3) cs_t3_.writes++;
      else if (chosen == 4) cs_t4_.writes++;
      else if (chosen == 5) cs_t5_.writes++;
      else if (chosen == 6) cs_t6_.writes++;
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
    cs_t1_.writes += kT1Size;
    cs_t2_.writes += kT2Size;
    cs_t3_.writes += kT3Size;
    cs_t4_.writes += kT4Size;
    cs_t5_.writes += kT5Size;
    cs_t6_.writes += kT6Size;
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
    cs_t1_.writes += kT1Size;
    cs_t2_.writes += kT2Size;
    cs_t3_.writes += kT3Size;
    cs_t4_.writes += kT4Size;
    cs_t5_.writes += kT5Size;
    cs_t6_.writes += kT6Size;
    useful_reset_counter_ = 0;
  }
  // ---------------------------------------

  // Step 7 - SC Update
  // Train SC if TAGE mispredicted, or if SC override was within threshold
  // margin, or if SC overrode
  if (sc_override || std::abs(sc_sum) < sc_threshold_ || tage_mispredicted) {
    if (!sc_tables_.empty()) {
      auto clamp = [](int8_t val, int min_val, int max_val) -> int8_t {
        if (val < min_val)
          return min_val;
        if (val > max_val)
          return max_val;
        return val;
      };

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

  // Dynamic SC threshold tuning
  bool sc_taken = (sc_sum > 0);
  if (std::abs(sc_sum) >= sc_threshold_ - 2 && std::abs(sc_sum) <= sc_threshold_ + 2) {
    if (sc_taken != tage_taken) {
      if (sc_taken == result.taken) {
        if (sc_tc_ < 31) sc_tc_++;
      } else {
        if (sc_tc_ > -32) sc_tc_--;
      }
      if (sc_tc_ == 31 && sc_threshold_ > 6) {
        sc_threshold_--;
        sc_tc_ = 0;
      } else if (sc_tc_ == -32 && sc_threshold_ < 30) {
        sc_threshold_++;
        sc_tc_ = 0;
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

  // Update Path History using the TARGET of the branch (like actual TAGE-SC-L)
  path_history_ = (path_history_ << 1) ^ (result.target >> 2);
  path_history_ &= 0xFFFF; // Keep it bounded to 16 bits
  // Update metrics for accuracy tracking
  (void)loop_hit_saved; // captured in metadata; silence unused-variable warning
  const bool predicted_taken = prediction.branch_found && prediction.taken;
  if (predicted_taken != result.taken) {
    ++metrics_.direction_mispredictions;
  } else if (result.taken && prediction.target != result.target) {
    ++metrics_.target_mispredictions;
  } else {
    ++metrics_.correct_predictions;
  }

  // --- Per-component accuracy tracking ---
  {
    uint64_t comp_id = (metadata >> 60) & 0xF;
    bool correct = (prediction.branch_found && (prediction.taken == result.taken) &&
                    !(prediction.taken && result.taken && prediction.target != result.target));
    ComponentStats *cs = nullptr;
    switch (comp_id) {
      case 0: cs = &cs_t0_; break;
      case 1: cs = &cs_t1_; break;
      case 2: cs = &cs_t2_; break;
      case 3: cs = &cs_t3_; break;
      case 4: cs = &cs_t4_; break;
      case 5: cs = &cs_t5_; break;
      case 6: cs = &cs_t6_; break;
      case 7: cs = &cs_sc_; break;
      case 8: cs = &cs_loop_; break;
      default: break;
    }
    if (cs && prediction.branch_found) {
      ++cs->selected;
      if (correct) ++cs->correct;
    }
  }
}

std::uint64_t PredictorOne::StorageBits() const {
  constexpr std::uint64_t kScCounterBits = 6;
  constexpr std::uint64_t kLoopIterBits = 10;
  constexpr std::uint64_t kLoopTripBits = 10;
  constexpr std::uint64_t kLoopAgeBits = 3;
  constexpr std::uint64_t kLoopConfidenceSeenBits = 2;

  // --- Student 3 (Friend's) Budget ---
  const std::uint64_t sc_bits = kScNumTables * kScTableSize * kScCounterBits;
  const std::uint64_t loop_entry_bits = 1 + kLoopTagBits + kLoopIterBits +
                                        kLoopTripBits + 1 + kLoopAgeBits +
                                        kLoopConfidenceSeenBits;
  const std::uint64_t loop_bits = kLoopTableSize * loop_entry_bits;

  // --- Student 2 (Your) TAGE Budget ---
  // BTB: Valid (1) + Tag (14) + Target (32)
  const std::uint64_t btb_bits = kBtbSize * (1 + 14 + 32);

  // Base Predictor: 3-bit counter
  const std::uint64_t base_bits = kBaseSize * 3;

  // Tagged tables: valid (1) + ctr (3) + u (2) + tag.
  const std::uint64_t t1_bits = kT1Size * (1 + 3 + 2 + kTagWidths[1]);
  const std::uint64_t t2_bits = kT2Size * (1 + 3 + 2 + kTagWidths[2]);
  const std::uint64_t t3_bits = kT3Size * (1 + 3 + 2 + kTagWidths[3]);
  const std::uint64_t t4_bits = kT4Size * (1 + 3 + 2 + kTagWidths[4]);
  const std::uint64_t t5_bits = kT5Size * (1 + 3 + 2 + kTagWidths[5]);
  const std::uint64_t t6_bits = kT6Size * (1 + 3 + 2 + kTagWidths[6]);

  const std::uint64_t tage_bits =
      t1_bits + t2_bits + t3_bits + t4_bits + t5_bits + t6_bits;

  // Exact history registers: 130-bit GHR + 16-bit path history
  const std::uint64_t history_bits = 130 + 16;

  // Useful bit reset counter (from guide)
  const std::uint64_t reset_counter_bits = 20;

  // Dynamic chooser/threshold state used to make future predictions.
  const std::uint64_t adaptive_state_bits = 4 + 5 + 6;

  return sc_bits + loop_bits + btb_bits + base_bits + tage_bits + history_bits +
         reset_counter_bits + adaptive_state_bits;
}

const PredictorMetrics &PredictorOne::Metrics() const { return metrics_; }

} // namespace pipesim
