#include "cache/replacement.h"

#include "cache/cache_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace pipesim {
namespace cache {
namespace {

[[maybe_unused]] void CheckCoordinates(std::uint32_t set, std::uint32_t way, std::uint32_t sets, std::uint32_t ways) {
  if (set >= sets || way >= ways) {
    throw std::runtime_error("replacement-policy coordinate out of range");
  }
}

// Tree-PLRU. Implement Reset/BeginCycle/OnHit/OnInsert/
// PeekVictim/SelectVictim/DebugState per SPEC.md/ABI.md.
//
// Worked example (current/next commit discipline only -- the same shape
// as tutorial.cc's ValueRegister::Commit): Commit() below is fully wired.
// Every other method here needs the same current_/planned_ separation, but
// with the actual tree-bit read/update logic, which is not given.
class TreePlru final : public ReplacementPolicy {
public:
  TreePlru(std::uint32_t sets, std::uint32_t ways) : sets_(sets), ways_(ways), current_(), planned_() {
    if (!IsPowerOfTwo(ways_) || ways_ == 0) {
      throw std::runtime_error("Tree-PLRU requires power-of-two ways >= 1");
    }

    // ways-1 internal nodes (0 nodes for the degenerate 1-way case)
    current_.assign(sets_, std::vector<bool>(ways_ - 1));
    planned_.assign(sets_, std::vector<bool>(ways_ - 1));
  }

  void Reset() override {
    for (std::uint32_t i = 0; i < sets_; i++) {
      for (std::uint32_t j = 0; j < ways_ - 1; j++) {
        current_[i][j] = false;
        planned_[i][j] = false;
      }
    }
    begun_ = false;
    event_planned_ = false;
  }

  void BeginCycle() override {
    if (begun_) {
      throw std::runtime_error("TreePlru::BeginCycle: already begun");
    }

    planned_ = current_;
    begun_ = true;
  }

  void OnHit(std::uint32_t set, std::uint32_t way) override {
    if (!begun_) {
      throw std::runtime_error("TreePlru::OnHit: cycle not begun");
    }
    event_planned_ = true;

    std::uint32_t node = way + ways_ - 1;

    while (node > 0) {
      std::uint32_t parent = (node - 1) / 2;
      if (node % 2 != 0) {
        planned_[set][parent] = true;
      } else {
        planned_[set][parent] = false;
      }
      node = parent;
    }
  }

  void OnInsert(std::uint32_t set, std::uint32_t way) override { OnHit(set, way); }

  int SelectVictim(std::uint32_t set, const std::vector<bool> &eligible) override {
    if (!begun_) {
      throw std::runtime_error("TreePlru::SelectVictim: cycle not begun");
    }
    event_planned_ = true;
    int target_way = PeekVictim(set, eligible);
    OnHit(set, target_way);
    return target_way;
  }

  int PeekVictim(std::uint32_t set, const std::vector<bool> &eligible) const override {
    std::uint32_t node = 0;
    std::uint32_t base_way = 0;
    std::uint32_t span = ways_;

    if (eligible.size() != ways_) {
      throw std::runtime_error("TreePlru::PeekVictim: eligible size mismatch");
    }

    if (std::none_of(eligible.begin(), eligible.end(), [](bool e) { return e; })) {
      throw std::runtime_error("TreePlru::PeekVictim: no eligible ways found");
    }

    while (node < ways_ - 1) {
      std::uint32_t half_span = span / 2;
      // PeekVictim is observational: must read current_ state only (spec §15.3)
      bool go_right = current_[set][node];

      bool left_eligible = false;
      bool right_eligible = false;

      for (std::uint32_t i = base_way; i < base_way + half_span; i++) {
        if (eligible[i]) {
          left_eligible = true;
        }
      }
      for (std::uint32_t i = base_way + half_span; i < base_way + span; i++) {
        if (eligible[i]) {
          right_eligible = true;
        }
      }

      if (!go_right && !left_eligible) {
        go_right = true;
      } else if (go_right && !right_eligible) {
        go_right = false;
      }

      if (go_right) {
        node = 2 * node + 2;
        base_way = base_way + half_span;
      } else {
        node = 2 * node + 1;
      }
      span = half_span;
    }

    return base_way;
  }

  std::vector<std::uint32_t> DebugState(std::uint32_t set) const override {
    (void)set;

    std::vector<std::uint32_t> state;
    state.reserve(ways_ - 1);
    for (std::uint32_t i = 0; i < ways_ - 1; i++) {
      state.push_back(current_[set][i] ? 1 : 0);
    }

    return state;
  }

  // Worked example: the general current/next commit discipline (compare
  // tutorial.cc's ValueRegister::Commit). Every other method above must
  // maintain the same current_/planned_ separation.
  void Commit() override {
    if (!begun_)
      throw std::runtime_error("Tree-PLRU Commit without begin");
    current_ = planned_;
    begun_ = false;
    event_planned_ = false;
  }

private:
  std::uint32_t sets_;
  std::uint32_t ways_;
  std::vector<std::vector<bool>> current_;
  std::vector<std::vector<bool>> planned_;
  bool begun_ = false;
  bool event_planned_ = false;
};

// SRRIP. Implement Reset/BeginCycle/OnHit/OnInsert/
// PeekVictim/SelectVictim/DebugState per SPEC.md/ABI.md (2-bit RRPV,
// init 3, hit 0, insertion 2, saturating aging, lowest-way tie-break).
class Srrip final : public ReplacementPolicy {
public:
  Srrip(std::uint32_t sets, std::uint32_t ways)
      : sets_(sets), ways_(ways), current_(sets, std::vector<std::uint8_t>(ways, 3)),
        planned_(sets, std::vector<std::uint8_t>(ways, 3)) {}

  void Reset() override {
    for (std::uint32_t i = 0; i < sets_; i++) {
      for (std::uint32_t j = 0; j < ways_; j++) {
        current_[i][j] = 3;
        planned_[i][j] = 3;
      }
    }
    begun_ = false;
    event_planned_ = false;
  }

  void BeginCycle() override {
    if (begun_) {
      throw std::runtime_error("SRRIP::BeginCycle: already begun");
    }

    planned_ = current_;
    begun_ = true;
    event_planned_ = false;
  }

  void OnHit(std::uint32_t set, std::uint32_t way) override {
    if (!begun_) {
      throw std::runtime_error("SRRIP::OnHit: cycle not begun");
    }
    event_planned_ = true;
    planned_[set][way] = 0;
  }

  void OnInsert(std::uint32_t set, std::uint32_t way) override {
    if (!begun_) {
      throw std::runtime_error("SRRIP::OnInsert: cycle not begun");
    }
    event_planned_ = true;
    planned_[set][way] = 2;
  }

  int SelectVictim(std::uint32_t set, const std::vector<bool> &eligible) override {
    if (!begun_) {
      throw std::runtime_error("SRRIP::SelectVictim: cycle not begun");
    }
    event_planned_ = true;
    int victim_way = PeekVictim(set, eligible);

    std::uint8_t max_rrvp = planned_[set][victim_way];
    std::uint8_t diff = 3 - max_rrvp;

    for (std::uint32_t i = 0; i < ways_; i++) {
      planned_[set][i] = std::min<std::uint8_t>(3, planned_[set][i] + diff);
    }

    return victim_way;
  }

  int PeekVictim(std::uint32_t set, const std::vector<bool> &eligible) const override {

    int victim_way = -1;
    int max_rrvp = -1;

    for (std::uint32_t i = 0; i < ways_; i++) {
      if (eligible[i]) {
        if (planned_[set][i] > max_rrvp) {
          max_rrvp = planned_[set][i];
          victim_way = i;
        }
      }
    }
    if (victim_way == -1) {
      throw std::runtime_error("Srrip::PeekVictim: no eligible ways found");
    }
    return victim_way;
  }

  std::vector<std::uint32_t> DebugState(std::uint32_t set) const override {

    std::vector<std::uint32_t> state;
    state.reserve(ways_);
    for (std::uint32_t i = 0; i < ways_; i++) {
      state.push_back(current_[set][i]);
    }

    return state;
  }

  void Commit() override {
    if (!begun_)
      throw std::runtime_error("SRRIP Commit without begin");
    current_ = planned_;
    begun_ = false;
    event_planned_ = false;
  }

private:
  std::uint32_t sets_;
  std::uint32_t ways_;
  std::vector<std::vector<std::uint8_t>> current_;
  std::vector<std::vector<std::uint8_t>> planned_;
  bool begun_ = false;
  bool event_planned_ = false;
};

} // namespace

// return the requested concrete policy for the supplied
//  sets/ways, or fail on an impossible enumerator. Worked example below.
std::unique_ptr<ReplacementPolicy> MakeReplacementPolicy(ReplacementKind kind, std::uint32_t sets, std::uint32_t ways) {
  switch (kind) {
  case ReplacementKind::kTreePlru:
    return std::unique_ptr<ReplacementPolicy>(new TreePlru(sets, ways));
  case ReplacementKind::kSrrip:
    return std::unique_ptr<ReplacementPolicy>(new Srrip(sets, ways));
  }
  throw std::runtime_error("unknown replacement policy");
}

} // namespace cache
} // namespace pipesim
