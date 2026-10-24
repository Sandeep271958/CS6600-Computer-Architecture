#include "pa4.h"
namespace multicore {

    int select_output_port(
        const RoutingContext& context,
        const SystemConfig& configuration) {

        const MeshCoordinate& cur = context.current_router;
        const MeshCoordinate& dst = context.destination_router;

        // Arrived at destination endpoint tile
        if (cur.x == dst.x && cur.y == dst.y) {
            return Local;
        }

        if (configuration.routing == "XY") {
            // Dimension-Order Routing: Resolve X dimension first
            if (cur.x < dst.x) {
                return PositiveX;
            }
            if (cur.x > dst.x) {
                return NegativeX;
            }
            // X aligned; resolve Y dimension
            if (cur.y < dst.y) {
                return PositiveY;
            }
            return NegativeY;
        } else {
            // YX routing: Resolve Y dimension first
            if (cur.y < dst.y) {
                return PositiveY;
            }
            if (cur.y > dst.y) {
                return NegativeY;
            }
            // Y aligned; resolve X dimension
            if (cur.x < dst.x) {
                return PositiveX;
            }
            return NegativeX;
        }
    }

    std::optional<unsigned> select_router_input(
        const std::vector<ArbitrationCandidate>& candidates,
        unsigned round_robin_cursor,
        const SystemConfig& configuration) {

        if (candidates.empty()) {
            return std::nullopt;
        }

        if (configuration.arbitration == "rr") {
            // Round-robin selection: Find candidate with minimum cyclic distance from cursor modulo 15
            unsigned best_input = candidates[0].input;
            unsigned best_distance = (candidates[0].input - round_robin_cursor + 15) % 15;

            for (size_t i = 1; i < candidates.size(); ++i) {
                unsigned dist = (candidates[i].input - round_robin_cursor + 15) % 15;
                if (dist < best_distance) {
                    best_distance = dist;
                    best_input = candidates[i].input;
                }
            }
            return best_input;
        } else {
            // Custom arbitration: Prioritize message classes to prevent protocol deadlock
            // Channel priority: Replies (channel 2) > Snoops (channel 1) > Requests (channel 0)
            // Tie-breaker: Lowest input ID
            int best_channel = -1;
            unsigned best_input = candidates[0].input;

            for (const auto& candidate : candidates) {
                int ch = static_cast<int>(candidate.input % 3);
                if (ch > best_channel) {
                    best_channel = ch;
                    best_input = candidate.input;
                } else if (ch == best_channel && candidate.input < best_input) {
                    best_input = candidate.input;
                }
            }
            return best_input;
        }
    }

}

