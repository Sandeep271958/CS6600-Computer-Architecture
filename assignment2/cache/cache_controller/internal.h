#pragma once
#include "cache/cache_controller.h"
#include "cache/replacement.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace pipesim::cache {
namespace cache_controller_internal {
enum class OwnerState { kUnsent, kInFlight, kResponding, kAckReady };
struct AddressFields {
  std::uint32_t block = 0, set = 0, tag = 0, offset = 0;
};
struct PipelineEntry {
  bool valid = false;
  CacheRequest request;
  std::uint64_t arrival_order = 0;
  AddressFields address;
};
struct OutputEntry {
  bool valid = false;
  CacheResponse response;
  std::uint64_t arrival_order = 0;
};
struct MshrEntry {
  bool valid = false;
  std::uint32_t block_address = 0, set = 0, tag = 0, reserved_way = 0;
  std::uint64_t arrival_order = 0;
  OwnerState state = OwnerState::kUnsent;
  CacheRequest lower_request;
  std::vector<MshrWaiter> waiters;
};
struct WritebackEntry {
  bool valid = false;
  std::uint32_t address = 0, size = 0;
  std::vector<std::uint8_t> data;
  std::uint64_t arrival_order = 0;
  OwnerState state = OwnerState::kUnsent;
  CacheRequest lower_request;
  std::vector<WritebackAckWaiter> waiters;
  std::uint32_t demand_bypasses = 0;
};
enum class ResponseOwner { kNone, kOutput, kMshr, kWriteback };
struct ResponseChoice {
  ResponseOwner owner = ResponseOwner::kNone;
  std::size_t entry = 0, waiter = 0;
  CacheResponse response;
  std::uint64_t arrival_order = 0;
};
enum class RequestOwner { kNone, kMshr, kWriteback };
struct RequestChoice {
  RequestOwner owner = RequestOwner::kNone;
  std::size_t entry = 0;
  CacheRequest request;
  std::uint64_t arrival_order = 0;
  bool demand_blocked_by_writeback = false;
  std::optional<std::size_t> oldest_eligible_writeback;
};
enum class LookupKind {
  kEmpty,
  kHit,
  kMshrMerge,
  kMshrAllocate,
  kWritebackForward,
  kHold
};
enum class LookupStall {
  kNone,
  kOutput,
  kMshrWaiterFull,
  kMshrFull,
  kReservedWay,
  kWritebackOrder,
  kWritebackWaiterFull,
  kWritebackFull
};
struct LookupDecision {
  LookupKind kind = LookupKind::kEmpty;
  LookupStall stall = LookupStall::kNone;
  int hit_way = -1, mshr = -1, victim_way = -1;
};
struct WritebackPlan {
  bool allowed = false;
  std::vector<std::size_t> merge_entries;
  int survivor = -1;
  LookupStall stall = LookupStall::kNone;
};
inline bool RangesOverlap(std::uint32_t a, std::uint32_t as, std::uint32_t b,
                          std::uint32_t bs) {
  const auto ae = static_cast<std::uint64_t>(a) + as,
             be = static_cast<std::uint64_t>(b) + bs;
  return a < be && b < ae;
}
inline bool ResponseLess(const ResponseChoice &a, const ResponseChoice &b) {
  return std::tie(a.arrival_order, a.response.id) <
         std::tie(b.arrival_order, b.response.id);
}
inline TransactionSource SourceForRole(CacheRole role) {
  switch (role) {
  case CacheRole::kL1I:
    return TransactionSource::kL1I;
  case CacheRole::kL1D:
    return TransactionSource::kL1D;
  case CacheRole::kL2I:
    return TransactionSource::kL2I;
  case CacheRole::kL2D:
    return TransactionSource::kL2D;
  }
  throw std::runtime_error("invalid cache role");
}
inline const char *OwnerStateName(OwnerState state) {
  switch (state) {
  case OwnerState::kUnsent:
    return "UNSENT";
  case OwnerState::kInFlight:
    return "IN_FLIGHT";
  case OwnerState::kResponding:
    return "RESPONDING";
  case OwnerState::kAckReady:
    return "ACK_READY";
  }
  return "UNKNOWN";
}
} // namespace cache_controller_internal
using namespace cache_controller_internal;

class CacheController::Impl {
public:
  Impl(CacheRole role, const CacheConfig &config,
       TransactionIdService *transaction_ids)
      : role_(role), config_(config), transaction_ids_(transaction_ids),
        current_lines_(config.sets, std::vector<CacheLine>(config.ways)),
        next_lines_(current_lines_), current_mshrs_(config.mshr_entries),
        next_mshrs_(current_mshrs_),
        current_writebacks_(config.writeback_entries),
        next_writebacks_(current_writebacks_),
        replacement_(MakeReplacementPolicy(config.replacement, config.sets,
                                           config.ways)) {
    if (transaction_ids_ == nullptr) {
      throw std::runtime_error(
          "cache controller transaction-id service is null");
    }
    Reset();
  }

  void Reset() {
    for (auto &set : current_lines_) {
      for (CacheLine &line : set) {
        line = CacheLine();
        line.data.assign(config_.line_size, 0);
      }
    }
    next_lines_ = current_lines_;
    current_index_ = PipelineEntry();
    current_lookup_ = PipelineEntry();
    current_output_ = OutputEntry();
    next_index_ = current_index_;
    next_lookup_ = current_lookup_;
    next_output_ = current_output_;
    current_mshrs_.assign(config_.mshr_entries, MshrEntry());
    next_mshrs_ = current_mshrs_;
    current_writebacks_.assign(config_.writeback_entries, WritebackEntry());
    next_writebacks_ = current_writebacks_;
    current_arrival_ = 0;
    next_arrival_ = 0;
    stats_ = CacheStats();
    next_stats_ = stats_;
    replacement_->Reset();
    begun_ = false;
    operated_ = false;
  }

  void BeginCycle() {
    if (begun_) {
      throw std::runtime_error("cache BeginCycle called twice");
    }
    next_lines_ = current_lines_;
    next_index_ = current_index_;
    next_lookup_ = current_lookup_;
    next_output_ = current_output_;
    next_mshrs_ = current_mshrs_;
    next_writebacks_ = current_writebacks_;
    next_arrival_ = current_arrival_;
    next_stats_ = stats_;
    replacement_->BeginCycle();
    begun_ = true;
    operated_ = false;
  }

  AddressFields MapAddress(std::uint32_t address) const {
    AddressFields fields;
    fields.offset = address % config_.line_size;
    fields.block = address - fields.offset;
    fields.set = (address / config_.line_size) % config_.sets;
    fields.tag = address / (config_.line_size * config_.sets);
    return fields;
  }

  void ValidateInbound(const CacheRequest &request) const {
    ValidateRequestPayload(request);
    if (!RangeFitsLine(request.address, request.size, config_.line_size)) {
      throw std::runtime_error(std::string(CacheRoleName(role_)) +
                               " request crosses a local cache line");
    }
    switch (role_) {
    case CacheRole::kL1I:
    case CacheRole::kL2I:
      if (request.type != RequestType::kRead) {
        throw std::runtime_error("instruction cache received a write request");
      }
      break;
    case CacheRole::kL1D:
      if (request.type != RequestType::kRead &&
          request.type != RequestType::kWrite) {
        throw std::runtime_error("L1D received an illegal request type");
      }
      break;
    case CacheRole::kL2D:
      if (request.type != RequestType::kRead &&
          request.type != RequestType::kWriteback) {
        throw std::runtime_error("L2D received an illegal request type");
      }
      break;
    }
    if ((role_ == CacheRole::kL1I || role_ == CacheRole::kL1D) &&
        request.origin_id != request.id) {
      throw std::runtime_error("L1 CPU request origin must equal link id");
    }
    if ((role_ == CacheRole::kL1I || role_ == CacheRole::kL1D) &&
        (request.size != 4 || request.address % 4 != 0)) {
      throw std::runtime_error("CPU cache request must be one aligned word");
    }
    if ((role_ == CacheRole::kL1D || role_ == CacheRole::kL2D) &&
        request.type != RequestType::kWriteback && request.generation != 0) {
      throw std::runtime_error("data demand carries a fetch generation");
    }
  }

  int FindHit(const AddressFields &address) const {
    int hit = -1;
    for (std::uint32_t way = 0; way < config_.ways; ++way) {
      const CacheLine &line = current_lines_[address.set][way];
      if (line.valid && line.tag == address.tag) {
        if (hit >= 0)
          throw std::runtime_error("duplicate tags in one cache set");
        hit = static_cast<int>(way);
      }
    }
    return hit;
  }

  int FindMshrByBlock(std::uint32_t block) const {
    int found = -1;
    for (std::size_t i = 0; i < current_mshrs_.size(); ++i) {
      if (current_mshrs_[i].valid && current_mshrs_[i].block_address == block) {
        if (found >= 0)
          throw std::runtime_error("duplicate MSHR block owner");
        found = static_cast<int>(i);
      }
    }
    return found;
  }

  int FindMshrByLowerId(std::uint64_t id) const {
    int found = -1;
    for (std::size_t i = 0; i < current_mshrs_.size(); ++i) {
      if (current_mshrs_[i].valid && current_mshrs_[i].lower_request.id == id) {
        if (found >= 0)
          throw std::runtime_error("duplicate MSHR lower id");
        found = static_cast<int>(i);
      }
    }
    return found;
  }

  int FindWritebackByLowerId(std::uint64_t id) const {
    int found = -1;
    for (std::size_t i = 0; i < current_writebacks_.size(); ++i) {
      if (current_writebacks_[i].valid &&
          current_writebacks_[i].lower_request.id == id) {
        if (found >= 0)
          throw std::runtime_error("duplicate writeback lower id");
        found = static_cast<int>(i);
      }
    }
    return found;
  }

  int FindFreeMshr() const {
    for (std::size_t i = 0; i < current_mshrs_.size(); ++i) {
      if (!current_mshrs_[i].valid)
        return static_cast<int>(i);
    }
    return -1;
  }

  int FindFreeWriteback() const {
    for (std::size_t i = 0; i < current_writebacks_.size(); ++i) {
      if (!current_writebacks_[i].valid)
        return static_cast<int>(i);
    }
    return -1;
  }

  std::vector<bool> EligibleWays(std::uint32_t set) const {
    std::vector<bool> eligible(config_.ways, false);
    for (std::uint32_t way = 0; way < config_.ways; ++way) {
      eligible[way] = !current_lines_[set][way].reserved;
    }
    return eligible;
  }

  int PeekVictim(std::uint32_t set) const {
    const std::vector<bool> eligible = EligibleWays(set);
    for (std::uint32_t way = 0; way < config_.ways; ++way) {
      if (eligible[way] && !current_lines_[set][way].valid)
        return way;
    }
    return replacement_->PeekVictim(set, eligible);
  }

  int SelectVictim(std::uint32_t set) {
    int way = PeekVictim(set);
    if (way >= 0) {
      next_lines_[set][way].reserved = true;
    }
    return way;
  }

  // TODO(A2-CACHE-11): compute the transitive UNSENT overlap closure for a
  // prospective writeback of [address, address+size); hold
  // (kWritebackOrder) on any IN_FLIGHT or same-cycle-granted overlap;
  // enforce the bounded combined waiter count; choose the oldest survivor
  // by (arrival_order, id).
  WritebackPlan PlanWriteback(std::uint32_t address, std::uint32_t size,
                              std::size_t new_waiters,
                              int granted_writeback = -1) const {
    (void)address;
    (void)size;
    (void)new_waiters;
    (void)granted_writeback;
    throw std::runtime_error(
        "CacheController::PlanWriteback: TODO(A2-CACHE-11)");
  }

  bool OutputWillDrain(bool response_granted) const {
    if (!response_granted || !current_output_.valid) {
      return false;
    }
    return SelectResponse().owner == ResponseOwner::kOutput;
  }

  LookupDecision DecideLookup(bool output_space,
                              int granted_writeback = -1) const {
    LookupDecision decision;
    if (!current_lookup_.valid) {
      decision.kind = LookupKind::kEmpty;
      return decision;
    }

    const auto &pipeline = current_lookup_;
    int hit = FindHit(pipeline.address);
    if (hit >= 0) {
      if (!output_space) {
        decision.kind = LookupKind::kHold;
        decision.stall = LookupStall::kOutput;
        return decision;
      }
      decision.kind = LookupKind::kHit;
      decision.hit_way = hit;
      return decision;
    }

    if (pipeline.request.type == RequestType::kWriteback) {
      throw std::runtime_error(
          "CacheController::DecideLookup: TODO(A2-CACHE-10)");
    }

    int mshr = FindMshrByBlock(pipeline.address.block);
    if (mshr >= 0) {
      if (current_mshrs_[mshr].waiters.size() >=
          config_.mshr_waiters_per_entry) {
        decision.kind = LookupKind::kHold;
        decision.stall = LookupStall::kMshrWaiterFull;
        return decision;
      }
      decision.kind = LookupKind::kMshrMerge;
      decision.mshr = mshr;
      return decision;
    }

    int free_mshr = FindFreeMshr();
    if (free_mshr < 0) {
      decision.kind = LookupKind::kHold;
      decision.stall = LookupStall::kMshrFull;
      return decision;
    }

    int victim = PeekVictim(pipeline.address.set);
    if (victim < 0) {
      decision.kind = LookupKind::kHold;
      decision.stall = LookupStall::kReservedWay;
      return decision;
    }

    const CacheLine &victim_line = current_lines_[pipeline.address.set][victim];
    if (victim_line.valid && victim_line.dirty) {
      std::uint32_t wb_address =
          (victim_line.tag * config_.sets + pipeline.address.set) *
          config_.line_size;
      WritebackPlan plan =
          PlanWriteback(wb_address, config_.line_size, 0, granted_writeback);
      if (!plan.allowed) {
        decision.kind = LookupKind::kHold;
        decision.stall = plan.stall;
        return decision;
      }
    }

    decision.kind = LookupKind::kMshrAllocate;
    decision.mshr = free_mshr;
    decision.victim_way = victim;
    return decision;
  }

  CacheStageIntent StageIntent(const CacheRequest *upper_request,
                               const CacheResponse *lower_response,
                               bool response_granted,
                               bool lower_request_granted) const {
    (void)upper_request;
    (void)lower_request_granted;
    CacheStageIntent intent;

    intent.accept_lower_response = false;
    if (lower_response != nullptr) {
      int mshr_idx = FindMshrByLowerId(lower_response->id);
      if (mshr_idx >= 0) {
        const MshrEntry &mshr = current_mshrs_[mshr_idx];
        if (mshr.state == OwnerState::kInFlight &&
            lower_response->type == ResponseType::kReadData) {
          try {
            ValidateEcho(*lower_response, mshr.lower_request,
                         ResponseType::kReadData);
            intent.accept_lower_response = true;
          } catch (...) {
          }
        }
      }

      if (!intent.accept_lower_response) {
        int wb_idx = FindWritebackByLowerId(lower_response->id);
        if (wb_idx >= 0) {
          const WritebackEntry &wb = current_writebacks_[wb_idx];
          if (wb.state == OwnerState::kInFlight &&
              lower_response->type == ResponseType::kWritebackAck) {
            try {
              ValidateEcho(*lower_response, wb.lower_request,
                           ResponseType::kWritebackAck);
              intent.accept_lower_response = true;
            } catch (...) {
            }
          }
        }
      }
    }

    const bool output_drains = OutputWillDrain(response_granted);
    const bool output_has_space = !current_output_.valid || output_drains;

    const bool lookup_can_advance = current_lookup_.valid && output_has_space;
    const bool lookup_is_free = !current_lookup_.valid;

    const bool index_can_advance =
        current_index_.valid && (lookup_is_free || lookup_can_advance);
    const bool index_is_free = !current_index_.valid;

    intent.accept_upper_request = index_is_free || index_can_advance;

    return intent;
  }

  MshrWaiter MakeMshrWaiter(const PipelineEntry &pipeline) const {
    MshrWaiter w;
    w.request_id = pipeline.request.id;
    w.origin_id = pipeline.request.origin_id;
    w.sequence = pipeline.request.sequence;
    w.arrival_order = pipeline.arrival_order;
    w.generation = pipeline.request.generation;
    w.type = pipeline.request.type;
    w.address = pipeline.request.address;
    w.size = pipeline.request.size;
    w.data = pipeline.request.data;
    w.response_ready = false;
    w.response_sent = false;
    w.response_data.clear();
    return w;
  }

  // TODO(A2-CACHE-10)
  WritebackAckWaiter MakeWritebackWaiter(const PipelineEntry &pipeline) const {
    (void)pipeline;
    throw std::runtime_error(
        "CacheController::MakeWritebackWaiter: TODO(A2-CACHE-10)");
  }

  std::uint64_t AllocateArrival() {
    if (next_arrival_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::runtime_error("cache arrival-order counter overflow");
    }
    return next_arrival_++;
  }

  CacheResponse ResponseForHit(const PipelineEntry &pipeline,
                               const CacheLine &line) const {
    CacheResponse response;
    response.id = pipeline.request.id;
    response.origin_id = pipeline.request.origin_id;
    response.sequence = pipeline.request.sequence;
    response.generation = pipeline.request.generation;
    response.address = pipeline.request.address;
    response.size = pipeline.request.size;

    if (pipeline.request.type == RequestType::kRead) {
      response.type = ResponseType::kReadData;
      response.data =
          ReadBytes(line.data, pipeline.address.block, pipeline.request.address,
                    pipeline.request.size);
    } else if (pipeline.request.type == RequestType::kWrite) {
      response.type = ResponseType::kWriteAck;
    } else if (pipeline.request.type == RequestType::kWriteback) {
      response.type = ResponseType::kWritebackAck;
    }
    return response;
  }

  ResponseChoice SelectResponse() const {
    std::optional<ResponseChoice> best;

    auto update_best = [&](const ResponseChoice &candidate) {
      if (!best || ResponseLess(candidate, *best)) {
        best = candidate;
      }
    };

    if (current_output_.valid) {
      ResponseChoice c;
      c.owner = ResponseOwner::kOutput;
      c.response = current_output_.response;
      c.arrival_order = current_output_.arrival_order;
      update_best(c);
    }

    for (std::size_t i = 0; i < current_mshrs_.size(); ++i) {
      const auto &mshr = current_mshrs_[i];
      if (mshr.valid && mshr.state == OwnerState::kResponding) {
        std::optional<std::size_t> oldest_waiter_idx;
        for (std::size_t j = 0; j < mshr.waiters.size(); ++j) {
          const auto &w = mshr.waiters[j];
          if (w.response_ready && !w.response_sent) {
            if (!oldest_waiter_idx) {
              oldest_waiter_idx = j;
            } else {
              const auto &ow = mshr.waiters[*oldest_waiter_idx];
              if (std::tie(w.arrival_order, w.request_id) <
                  std::tie(ow.arrival_order, ow.request_id)) {
                oldest_waiter_idx = j;
              }
            }
          }
        }
        if (oldest_waiter_idx) {
          const auto &w = mshr.waiters[*oldest_waiter_idx];
          ResponseChoice c;
          c.owner = ResponseOwner::kMshr;
          c.entry = i;
          c.waiter = *oldest_waiter_idx;
          c.response.id = w.request_id;
          c.response.origin_id = w.origin_id;
          c.response.sequence = w.sequence;
          c.response.generation = w.generation;
          c.response.address = w.address;
          c.response.size = w.size;
          if (w.type == RequestType::kRead) {
            c.response.type = ResponseType::kReadData;
            c.response.data = w.response_data;
          } else {
            c.response.type = ResponseType::kWriteAck;
          }
          c.arrival_order = w.arrival_order;
          update_best(c);
        }
      }
    }

    for (std::size_t i = 0; i < current_writebacks_.size(); ++i) {
      const auto &wb = current_writebacks_[i];
      if (wb.valid && wb.state == OwnerState::kAckReady) {
        std::optional<std::size_t> oldest_waiter_idx;
        for (std::size_t j = 0; j < wb.waiters.size(); ++j) {
          const auto &w = wb.waiters[j];
          if (!w.response_sent) {
            if (!oldest_waiter_idx) {
              oldest_waiter_idx = j;
            } else {
              const auto &ow = wb.waiters[*oldest_waiter_idx];
              if (std::tie(w.arrival_order, w.request_id) <
                  std::tie(ow.arrival_order, ow.request_id)) {
                oldest_waiter_idx = j;
              }
            }
          }
        }
        if (oldest_waiter_idx) {
          const auto &w = wb.waiters[*oldest_waiter_idx];
          ResponseChoice c;
          c.owner = ResponseOwner::kWriteback;
          c.entry = i;
          c.waiter = *oldest_waiter_idx;
          c.response.id = w.request_id;
          c.response.origin_id = w.origin_id;
          c.response.sequence = w.sequence;
          c.response.generation = w.generation;
          c.response.address = w.address;
          c.response.size = w.size;
          c.response.type = ResponseType::kWritebackAck;
          c.arrival_order = w.arrival_order;
          update_best(c);
        }
      }
    }

    return best ? *best : ResponseChoice{};
  }

  bool
  ConflictsWithLowerIncompleteWriteback(const CacheRequest &request) const {
    for (const auto &wb : current_writebacks_) {
      if (wb.valid && wb.state != OwnerState::kAckReady) {
        if (RangesOverlap(request.address, request.size, wb.address, wb.size)) {
          return true;
        }
      }
    }
    return false;
  }

  RequestChoice SelectLowerRequest() const {
    // Find the oldest eligible UNSENT demand MSHR (not conflicting with
    // an incomplete writeback).
    std::optional<std::size_t> best_demand_idx;
    for (std::size_t i = 0; i < current_mshrs_.size(); ++i) {
      const auto &mshr = current_mshrs_[i];
      if (!mshr.valid || mshr.state != OwnerState::kUnsent)
        continue;
      if (ConflictsWithLowerIncompleteWriteback(mshr.lower_request))
        continue;
      if (!best_demand_idx) {
        best_demand_idx = i;
      } else {
        const auto &best = current_mshrs_[*best_demand_idx];
        if (std::tie(mshr.arrival_order, mshr.lower_request.id) <
            std::tie(best.arrival_order, best.lower_request.id)) {
          best_demand_idx = i;
        }
      }
    }

    // Find the oldest eligible UNSENT writeback.
    std::optional<std::size_t> best_wb_idx;
    for (std::size_t i = 0; i < current_writebacks_.size(); ++i) {
      const auto &wb = current_writebacks_[i];
      if (!wb.valid || wb.state != OwnerState::kUnsent)
        continue;
      if (!best_wb_idx) {
        best_wb_idx = i;
      } else {
        const auto &best = current_writebacks_[*best_wb_idx];
        if (std::tie(wb.arrival_order, wb.lower_request.id) <
            std::tie(best.arrival_order, best.lower_request.id)) {
          best_wb_idx = i;
        }
      }
    }

    RequestChoice result;
    result.oldest_eligible_writeback = best_wb_idx;

    // Determine if a writeback must be forced instead of a demand.
    bool force_writeback = false;
    if (best_demand_idx && best_wb_idx) {
      const auto &wb = current_writebacks_[*best_wb_idx];
      bool no_free_wb = (FindFreeWriteback() < 0);
      bool wb_starved = (wb.demand_bypasses >= 8);
      force_writeback = no_free_wb || wb_starved;
    }

    if (best_wb_idx && (!best_demand_idx || force_writeback)) {
      const auto &wb = current_writebacks_[*best_wb_idx];
      result.owner = RequestOwner::kWriteback;
      result.entry = *best_wb_idx;
      result.request = wb.lower_request;
      result.arrival_order = wb.arrival_order;
      result.demand_blocked_by_writeback = false;
    } else if (best_demand_idx) {
      const auto &mshr = current_mshrs_[*best_demand_idx];
      result.owner = RequestOwner::kMshr;
      result.entry = *best_demand_idx;
      result.request = mshr.lower_request;
      result.arrival_order = mshr.arrival_order;
      result.demand_blocked_by_writeback = false;
    }
    return result;
  }

  std::optional<CacheResponse> ResponseIntent() const {
    ResponseChoice choice = SelectResponse();
    if (choice.owner != ResponseOwner::kNone) {
      return choice.response;
    }
    return std::nullopt;
  }

  std::optional<CacheRequest> LowerRequestIntent() const {
    RequestChoice choice = SelectLowerRequest();
    if (choice.owner != RequestOwner::kNone) {
      return choice.request;
    }
    return std::nullopt;
  }

  // mark the candidate ResponseIntent() offered as sent;
  // free an MSHR (unpinning its way) or writeback entry once every one of
  // its waiters has been sent.
  // Existing helpers: SelectResponse, ResponseIntent.
  void ApplyResponseGrant() {
    ResponseChoice choice = SelectResponse();
    if (choice.owner == ResponseOwner::kNone) {
      throw std::runtime_error("ApplyResponseGrant called with no response");
    }

    if (choice.owner == ResponseOwner::kOutput) {
      next_output_.valid = false;
    } else if (choice.owner == ResponseOwner::kMshr) {
      MshrEntry &mshr = next_mshrs_[choice.entry];

      bool all_sent = true;

      for (const auto &w : wb.waiters) {
        if (!w.response_sent) {
          all_sent = false;
          break;
        }
      }
      if (all_sent) {
        wb.valid = false;
      }
    }
  }

  void ValidateEcho(const CacheResponse &response, const CacheRequest &request,
                    ResponseType expected_type) const {
    if (response.id != request.id || response.origin_id != request.origin_id ||
        response.sequence != request.sequence ||
        response.generation != request.generation ||
        response.address != request.address || response.size != request.size ||
        response.type != expected_type) {
      throw std::runtime_error("lower response metadata does not echo request");
    }
  }

  // TODO(A2-CACHE-09/10): for an owned fill -- install the complete line
  // into its already-reserved way, replay every waiter once in arrival
  // order at byte granularity (capturing each load's bytes at its own
  // replay point, applying each store's bytes), mark dirty if any waiter
  // wrote, call replacement_->OnInsert exactly once. For an owned
  // writeback ack -- transition ACK_READY (or free immediately if it had
  // no waiters).
  // Existing helpers: FindMshrByLowerId, FindWritebackByLowerId, ValidateEcho.
  void ApplyLowerResponse(const CacheResponse &response) {
    (void)response;
    throw std::runtime_error(
        "CacheController::ApplyLowerResponse: TODO(A2-CACHE-09)");
  }

  // TODO(A2-CACHE-10/11): insert a new writeback entry, or merge into the
  // transitive-overlap survivor chosen by a matching PlanWriteback() call
  // -- oldest entry/ID survives, bytes applied in entry-age order with the
  // newly-accepted bytes winning any overlap (this call's data is newest),
  // combined waiters bounded and sorted by (arrival_order, request id).
  void
  InsertWriteback(std::uint32_t address, const std::vector<std::uint8_t> &data,
                  std::uint64_t arrival_order,
                  const std::optional<WritebackAckWaiter> &waiter,
                  int granted_writeback = -1,
                  std::optional<std::uint64_t> preallocated_id = std::nullopt) {
    (void)address;
    (void)data;
    (void)arrival_order;
    (void)waiter;
    (void)granted_writeback;
    (void)preallocated_id;
    throw std::runtime_error(
        "CacheController::InsertWriteback: TODO(A2-CACHE-11)");
  }

  void ApplyHit(const LookupDecision &decision) {
    const auto &pipeline = current_lookup_;
    CacheLine &line = next_lines_[pipeline.address.set][decision.hit_way];

    if (pipeline.request.type == RequestType::kRead) {
      next_output_.valid = true;
      next_output_.arrival_order = AllocateArrival();
      next_output_.response = ResponseForHit(pipeline, line);
    } else {
      WriteBytes(&line.data, pipeline.address.block, pipeline.request.address,
                 pipeline.request.data);
      line.dirty = true;
      next_output_.valid = true;
      next_output_.arrival_order = AllocateArrival();
      next_output_.response = ResponseForHit(pipeline, line);
    }

    replacement_->OnHit(pipeline.address.set, decision.hit_way);

    if (pipeline.request.type == RequestType::kWriteback) {
      CheckedIncrement(&next_stats_.writeback_hits, "writeback_hits");
    } else {
      CheckedIncrement(&next_stats_.hits, "hits");
    }

    next_lookup_.valid = false;
  }

  void ApplyMshrMerge(int mshr_index) {
    next_mshrs_[mshr_index].waiters.push_back(MakeMshrWaiter(current_lookup_));
    CheckedIncrement(&next_stats_.mshr_merges, "mshr_merges");
    next_lookup_.valid = false;
  }

  void ApplyMshrAllocation(const LookupDecision &decision,
                           int granted_writeback) {
    const auto &pipeline = current_lookup_;
    int victim = SelectVictim(pipeline.address.set);
    const CacheLine &victim_line = current_lines_[pipeline.address.set][victim];

    bool victim_valid = victim_line.valid;
    bool victim_dirty = victim_line.dirty;

    int ids_needed = 1;
    if (victim_valid && victim_dirty) {
      ids_needed = 2;
    }

    std::vector<std::uint64_t> ids =
        transaction_ids_->ReserveBatch(SourceForRole(role_), ids_needed);
    std::uint64_t read_id = ids[0];

    if (victim_valid && victim_dirty) {
      std::uint32_t wb_address =
          (victim_line.tag * config_.sets + pipeline.address.set) *
          config_.line_size;
      InsertWriteback(wb_address, victim_line.data, AllocateArrival(),
                      std::nullopt, granted_writeback, ids[1]);
      CheckedIncrement(&next_stats_.dirty_evictions, "dirty_evictions");
    }

    if (victim_valid) {
      CheckedIncrement(&next_stats_.evictions, "evictions");
    }

    MshrEntry &mshr = next_mshrs_[decision.mshr];
    mshr.valid = true;
    mshr.block_address = pipeline.address.block;
    mshr.set = pipeline.address.set;
    mshr.tag = pipeline.address.tag;
    mshr.reserved_way = victim;
    mshr.arrival_order = AllocateArrival();
    mshr.state = OwnerState::kUnsent;

    mshr.lower_request.id = read_id;
    mshr.lower_request.origin_id = kNoOriginId;
    mshr.lower_request.sequence = kNoSequence;
    mshr.lower_request.generation = 0;
    mshr.lower_request.address = pipeline.address.block;
    mshr.lower_request.size = config_.line_size;
    mshr.lower_request.type = RequestType::kRead;
    mshr.lower_request.data.clear();

    mshr.waiters.clear();
    mshr.waiters.push_back(MakeMshrWaiter(pipeline));

    CheckedIncrement(&next_stats_.primary_misses, "primary_misses");
    next_lookup_.valid = false;
  }

  // TODO(A2-CACHE-10): forward an upper writeback into InsertWriteback with
  // its own ack waiter; clear LOOKUP.
  // Existing helpers: MakeWritebackWaiter.
  void ApplyWritebackForward(int granted_writeback) {
    (void)granted_writeback;
    throw std::runtime_error(
        "CacheController::ApplyWritebackForward: TODO(A2-CACHE-10)");
  }

  // increment exactly the one stats counter
  // corresponding to `stall` (kNone/kOutput count nothing). See SPEC.md's
  // stall-precedence chain for which counter fires when several reasons
  // could apply.
  void CountLookupStall(LookupStall stall) {

    switch (stall) {
    case LookupStall ::kNone:
      break;

    case LookupStall::kOutput:
      CheckedIncrement(&next_stats_.output_queue_stalls, "output_queue_stalls");
      break;
    case LookupStall::kMshrWaiterFull:
      CheckedIncrement(&next_stats_.mshr_waiter_full_stalls,
                       "mshr_waiter_full_stalls");
      break;
    case LookupStall::kMshrFull:
      CheckedIncrement(&next_stats_.mshr_full_stalls, "mshr_full_stalls");
      break;
    case LookupStall::kReservedWay:
      CheckedIncrement(&next_stats_.reserved_way_stalls, "reserved_way_stalls");
      break;
    case LookupStall::kWritebackOrder:
      CheckedIncrement(&next_stats_.writeback_order_stalls,
                       "writeback_order_stalls");
      break;
    case LookupStall::kWritebackWaiterFull:
      CheckedIncrement(&next_stats_.writeback_waiter_full_stalls,
                       "writeback_waiter_full_stalls");
      break;
    case LookupStall::kWritebackFull:
      CheckedIncrement(&next_stats_.writeback_full_stalls,
                       "writeback_full_stalls");
      break;
    }
  }

  // transition the granted MSHR/writeback UNSENT ->
  // IN_FLIGHT. Granting a demand bumps the oldest eligible writeback's
  // demand_bypasses (capped at 8); granting a writeback resets
  // demand_bypasses to 0 and counts writebacks_sent.
  void ApplyLowerRequestGrant() {
    RequestChoice choice = SelectLowerRequest();

    if (choice.owner == RequestOwner::kNone) {
      throw std::runtime_error("ApplyLowerRequestGrant called with no request");
    }
    if (choice.owner == RequestOwner::kMshr) {
      next_mshrs_[choice.entry].state = OwnerState::kInFlight;
      std::size_t wb_idx = choice.oldest_eligible_writeback.value();
      if (next_writebacks_[wb_idx].demand_bypasses < 8) {
        next_writebacks_[wb_idx].demand_bypasses++;
      }

    } else if (choice.owner == RequestOwner::kWriteback) {
      next_writebacks_[choice.entry].state = OwnerState::kInFlight;
      next_writebacks_[choice.entry].demand_bypasses = 0;
      CheckedIncrement(&next_stats_, "writebacks_sent");
    }
  }

  // the single mutating call per cycle. Apply only the
  // transfers `grants` authorizes, computed from ONE consistent snapshot of
  // current_* state: upper-response removal (ApplyResponseGrant), owned
  // fill/writeback-ack installation (ApplyLowerResponse), at most one
  // parallel LOOKUP decision or its exact stall (DecideLookup +
  // ApplyHit/ApplyMshrMerge/ApplyMshrAllocation/ApplyWritebackForward/
  // CountLookupStall -- note a fill takes the LOOKUP port, so it must not
  // additionally run), back-to-front INDEX->LOOKUP movement plus admission
  // of upper_request into INDEX, and the granted lower-request owner
  // transition (ApplyLowerRequestGrant). Update every counter exactly
  // once, at its defined event point. Throw for any grant referring to a
  // nonexistent candidate, or any owner/echo mismatch -- see ABI.md's
  // error taxonomy.
  // Existing helpers: ResponseIntent, LowerRequestIntent, StageIntent,
  // ValidateInbound, AllocateArrival, MapAddress.
  void Operate(std::uint64_t current_cycle, const CacheRequest *upper_request,
               const CacheResponse *lower_response,
               const CacheOperateGrants &grants) {
    (void)current_cycle;
    if (!begun_ || operated_) {
      throw std::runtime_error("cache Operate lifecycle violation");
    }
    operated_ = true;

    CacheStageIntent intent =
        StageIntent(upper_request, lower_response, grants.upper_response,
                    grants.lower_request);

    if (grants.upper_response) {
      ApplyResponseGrant();
    }

    bool did_fill = false;
    if (intent.accept_lower_response) {
      ApplyLowerResponse(*lower_response);
      did_fill = true;
    }

    if (!did_fill && current_lookup_.valid) {
      bool output_space =
          !current_output_.valid || OutputWillDrain(grants.upper_response);

      int granted_writeback = -1;
      if (grants.lower_request) {
        RequestChoice lr_choice = SelectLowerRequest();
        if (lr_choice.owner == RequestOwner::kWriteback) {
          granted_writeback = static_cast<int>(lr_choice.entry);
        }
      }

      LookupDecision decision = DecideLookup(output_space, granted_writeback);

      if (decision.kind == LookupKind::kHit) {
        ApplyHit(decision);
      } else if (decision.kind == LookupKind::kMshrMerge) {
        ApplyMshrMerge(decision.mshr);
      } else if (decision.kind == LookupKind::kMshrAllocate) {
        ApplyMshrAllocation(decision, granted_writeback);
      } else if (decision.kind == LookupKind::kWritebackForward) {
        ApplyWritebackForward(granted_writeback);
      } else if (decision.kind == LookupKind::kHold) {
        CountLookupStall(decision.stall);
      }
    }

    if (intent.accept_upper_request) {
      if (current_index_.valid) {
        next_lookup_ = current_index_;
        next_index_.valid = false;
      }

      if (grants.upper_request && upper_request) {
        next_index_.valid = true;
        next_index_.arrival_order = AllocateArrival();
        next_index_.request = *upper_request;
        next_index_.address = MapAddress(upper_request->address);
      }
    }

    if (grants.lower_request) {
      ApplyLowerRequestGrant();
    }
  }

  void Commit() {
    if (!begun_ || !operated_) {
      throw std::runtime_error("cache Commit lifecycle violation");
    }
    replacement_->Commit();
    current_lines_ = std::move(next_lines_);
    current_index_ = std::move(next_index_);
    current_lookup_ = std::move(next_lookup_);
    current_output_ = std::move(next_output_);
    current_mshrs_ = std::move(next_mshrs_);
    current_writebacks_ = std::move(next_writebacks_);
    current_arrival_ = next_arrival_;
    stats_ = next_stats_;
    begun_ = false;
    operated_ = false;
  }

  CacheSnapshot Snapshot() const {
    CacheSnapshot snapshot;
    const PipelineEntry entries[2] = {current_index_, current_lookup_};
    for (int i = 0; i < 2; ++i) {
      snapshot.pipeline[i].valid = entries[i].valid;
      if (entries[i].valid) {
        snapshot.pipeline[i].request_id = entries[i].request.id;
        snapshot.pipeline[i].address = entries[i].request.address;
        snapshot.pipeline[i].state = i == 0 ? "INDEX" : "LOOKUP";
      }
    }
    snapshot.pipeline[2].valid = current_output_.valid;
    if (current_output_.valid) {
      snapshot.pipeline[2].request_id = current_output_.response.id;
      snapshot.pipeline[2].address = current_output_.response.address;
      snapshot.pipeline[2].state = "OUTPUT";
    }
    for (const MshrEntry &entry : current_mshrs_) {
      MshrSnapshot row;
      row.waiter_capacity = config_.mshr_waiters_per_entry;
      if (entry.valid) {
        row.valid = true;
        row.block_address = entry.block_address;
        row.set = entry.set;
        row.tag = entry.tag;
        row.reserved_way = static_cast<int>(entry.reserved_way);
        row.lower_id = entry.lower_request.id;
        row.arrival_order = entry.arrival_order;
        row.state = OwnerStateName(entry.state);
        row.waiters = entry.waiters;
      }
      snapshot.mshrs.push_back(std::move(row));
    }
    for (const WritebackEntry &entry : current_writebacks_) {
      WritebackSnapshot row;
      row.waiter_capacity = config_.writeback_waiters_per_entry;
      if (entry.valid) {
        row.valid = true;
        row.address = entry.address;
        row.size = entry.size;
        row.lower_id = entry.lower_request.id;
        row.arrival_order = entry.arrival_order;
        row.demand_bypasses = entry.demand_bypasses;
        row.state = OwnerStateName(entry.state);
        row.data = entry.data;
        row.waiters = entry.waiters;
      }
      snapshot.writebacks.push_back(std::move(row));
    }
    for (std::uint32_t set = 0; set < config_.sets; ++set) {
      const std::vector<std::uint32_t> replacement =
          replacement_->DebugState(set);
      snapshot.replacement.push_back({set, replacement});
      for (std::uint32_t way = 0; way < config_.ways; ++way) {
        std::uint64_t owner = kNoOriginId;
        for (const MshrEntry &mshr : current_mshrs_) {
          if (mshr.valid && mshr.set == set && mshr.reserved_way == way) {
            owner = mshr.lower_request.id;
          }
        }
        snapshot.lines.push_back({set, way, current_lines_[set][way], owner});
      }
    }
    return snapshot;
  }

  bool HasWork() const {
    if (current_index_.valid || current_lookup_.valid ||
        current_output_.valid) {
      return true;
    }
    return std::any_of(current_mshrs_.begin(), current_mshrs_.end(),
                       [](const MshrEntry &entry) { return entry.valid; }) ||
           std::any_of(current_writebacks_.begin(), current_writebacks_.end(),
                       [](const WritebackEntry &entry) { return entry.valid; });
  }

  CacheRole role_;
  CacheConfig config_;
  TransactionIdService *transaction_ids_;
  std::vector<std::vector<CacheLine>> current_lines_;
  std::vector<std::vector<CacheLine>> next_lines_;
  PipelineEntry current_index_;
  PipelineEntry current_lookup_;
  OutputEntry current_output_;
  PipelineEntry next_index_;
  PipelineEntry next_lookup_;
  OutputEntry next_output_;
  std::vector<MshrEntry> current_mshrs_;
  std::vector<MshrEntry> next_mshrs_;
  std::vector<WritebackEntry> current_writebacks_;
  std::vector<WritebackEntry> next_writebacks_;
  std::uint64_t current_arrival_ = 0;
  std::uint64_t next_arrival_ = 0;
  CacheStats stats_;
  CacheStats next_stats_;
  std::unique_ptr<ReplacementPolicy> replacement_;
  bool begun_ = false;
  bool operated_ = false;
};
} // namespace pipesim::cache
