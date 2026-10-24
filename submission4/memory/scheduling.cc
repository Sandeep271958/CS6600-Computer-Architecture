#include "pa4.h"
namespace multicore {

    std::optional<unsigned> select_memory_request(
        const std::vector<unsigned>& eligible_requests,
        const MemoryController& controller,
        Cycle cycle,
        const SystemConfig& configuration) {

        if (eligible_requests.empty()) {
            return std::nullopt;
        }

        if (configuration.scheduler == "fcfs") {
            // First-Come First-Served: Select earliest controller_arrival time
            unsigned best_idx = eligible_requests[0];
            Cycle best_arrival = controller.queue[best_idx].controller_arrival;

            for (size_t i = 1; i < eligible_requests.size(); ++i) {
                unsigned idx = eligible_requests[i];
                Cycle arr = controller.queue[idx].controller_arrival;
                if (arr < best_arrival) {
                    best_arrival = arr;
                    best_idx = idx;
                } else if (arr == best_arrival && idx < best_idx) {
                    best_idx = idx; // Deterministic tie-breaker: smaller queue index
                }
            }
            return best_idx;
        } else {
            // Custom scheduler: FR-FCFS with Starvation Threshold
            constexpr Cycle StarvationThreshold = 80;

            int best_tier = -1;
            Cycle best_arrival = 0;
            unsigned best_idx = eligible_requests[0];

            for (unsigned idx : eligible_requests) {
                const Packet& pkt = controller.queue[idx];

                // Decode bank and target row
                uint32_t line_idx = pkt.line_address / 64 / configuration.controllers;
                unsigned b_idx = line_idx % configuration.banks;
                unsigned target_row = (line_idx / configuration.banks) / 16;
                int open_row = controller.banks[b_idx].open_row;

                Cycle wait_time = (cycle >= pkt.controller_arrival) ? (cycle - pkt.controller_arrival) : 0;
                bool is_starved = (wait_time >= StarvationThreshold);
                bool is_row_hit = (open_row >= 0 && static_cast<unsigned>(open_row) == target_row);

                // Tier 2: Starved (highest priority to bound maximum wait)
                // Tier 1: Row buffer hits (high DRAM throughput)
                // Tier 0: Row buffer misses/conflicts
                int tier = 0;
                if (is_starved) {
                    tier = 2;
                } else if (is_row_hit) {
                    tier = 1;
                }

                if (tier > best_tier) {
                    best_tier = tier;
                    best_arrival = pkt.controller_arrival;
                    best_idx = idx;
                } else if (tier == best_tier) {
                    if (pkt.controller_arrival < best_arrival) {
                        best_arrival = pkt.controller_arrival;
                        best_idx = idx;
                    } else if (pkt.controller_arrival == best_arrival && idx < best_idx) {
                        best_idx = idx;
                    }
                }
            }
            return best_idx;
        }
    }

}

