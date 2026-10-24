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

    void sample_occupancy_and_stalls(MulticoreSystem& /* system */) {

        // PA4-COUNT-02: Sample current occupancy and stalls once per cycle.
        // Implement this after checking your PA3 migration with the arithmetic workload.
    }

}

