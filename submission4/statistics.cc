#include "pa4.h"
namespace multicore {

    void count_event(
        MulticoreSystem& system,
        const std::string& component,
        const std::string& event,
        uint64_t amount) {

        std::string key = component + "." + event;
        if (event.size() >= 4 && event.rfind("_max") == event.size() - 4) {
            system.counters.maximum(key, amount);
        } else {
            system.counters.add(key, amount);
        }
    }

    void sample_occupancy_and_stalls(MulticoreSystem& system) {
        // 1. Router Input Queues (noc.T.queue.Q and noc.T)
        for (int t = 0; t < system.configuration.tiles(); ++t) {
            uint64_t tile_sum = 0;
            std::string t_prefix = "noc." + std::to_string(t);

            for (unsigned q = 0; q < 15; ++q) {
                uint64_t q_len = system.current.network.buffers[t][q].size();
                std::string q_key = t_prefix + ".queue." + std::to_string(q);

                system.counters.add(q_key + ".occupancy_sum", q_len);
                system.counters.maximum(q_key + ".occupancy_max", q_len);
                system.counters.add(q_key + ".samples", 1);
                tile_sum += q_len;
            }

            system.counters.add(t_prefix + ".queue_occupancy_sum", tile_sum);
            system.counters.maximum(t_prefix + ".queue_occupancy_max", tile_sum);
            system.counters.add(t_prefix + ".samples", 1);
        }

        // 2. Memory Controllers (mc.M)
        for (int m = 0; m < system.configuration.controllers; ++m) {
            uint64_t mc_len = system.current.controllers[m].queue.size();
            std::string mc_key = "mc." + std::to_string(m);

            system.counters.add(mc_key + ".queue_occupancy_sum", mc_len);
            system.counters.maximum(mc_key + ".queue_occupancy_max", mc_len);
            system.counters.add(mc_key + ".samples", 1);
        }

        // 3. Shared LLC Banks (llc.H)
        for (int h = 0; h < system.configuration.homes; ++h) {
            uint64_t llc_len = system.current.llc[h].requests.size();
            std::string llc_key = "llc." + std::to_string(h);

            system.counters.add(llc_key + ".queue_occupancy_sum", llc_len);
            system.counters.maximum(llc_key + ".queue_occupancy_max", llc_len);
            system.counters.add(llc_key + ".samples", 1);
        }

        // 4. Store Buffers & Core Stalls (order.C)
        for (int c = 0; c < system.configuration.cores; ++c) {
            const auto& order = system.current.order[c];
            uint64_t sb_len = order.stores.size();
            std::string ord_key = "order." + std::to_string(c);

            system.counters.add(ord_key + ".queue_occupancy_sum", sb_len);
            system.counters.maximum(ord_key + ".queue_occupancy_max", sb_len);
            system.counters.add(ord_key + ".samples", 1);

            const auto& cpu_req = system.current.cpu[c];
            if (cpu_req.has_value()) {
                // Buffer full stall: store waiting and store buffer is full
                if (cpu_req->operation == MemoryOperation::Store &&
                    sb_len >= system.configuration.store_entries) {
                    system.counters.add(ord_key + ".buffer_full_cycles", 1);
                }

                // Fence wait stall: fence waiting with non-empty buffer or in-flight access
                if (cpu_req->operation == MemoryOperation::Fence &&
                    (sb_len > 0 || order.inflight.has_value())) {
                    system.counters.add(ord_key + ".fence_wait_cycles", 1);
                }

                // Ordering stall: access waiting while earlier request in flight OR fence/xchg waiting for drain
                if (order.inflight.has_value() ||
                    (cpu_req->operation == MemoryOperation::Fence && sb_len > 0) ||
                    (cpu_req->operation == MemoryOperation::Exchange && sb_len > 0)) {
                    system.counters.add(ord_key + ".ordering_stall_cycles", 1);
                }
            }
        }
    }

}

