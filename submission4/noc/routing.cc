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
        const std::vector<ArbitrationCandidate>& /* candidates */,
        unsigned /* round_robin_cursor */,
        const SystemConfig& /* configuration */) {

        // PA4-NOC-02: Implement round-robin and your policy. Return an eligible input index.
        not_implemented("PA4-NOC-02");

    }

}

