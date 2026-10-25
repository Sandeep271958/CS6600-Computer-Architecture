#include "pa4.h"
#include <iostream>
#include <stdexcept>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

using namespace multicore;

void test_routes() {
    SystemConfig config;
    config.x = 3;
    config.y = 3;
    for (const std::string& order : {std::string("XY"), std::string("YX")}) {
        config.routing = order;
        for (int source = 0; source < 9; ++source) {
            for (int destination = 0; destination < 9; ++destination) {
                MeshCoordinate current{source % 3, source / 3};
                const MeshCoordinate target{destination % 3, destination / 3};
                int distance = 0;
                while (current.x != target.x || current.y != target.y) {
                    const int port = select_output_port(
                        {current, target, static_cast<unsigned>(current.y * 3 + current.x)},
                        config);
                    switch (port) {
                        case PositiveX: ++current.x; break;
                        case NegativeX: --current.x; break;
                        case PositiveY: ++current.y; break;
                        case NegativeY: --current.y; break;
                        default: throw std::runtime_error("route terminated before destination");
                    }
                    check(current.x >= 0 && current.x < 3 &&
                              current.y >= 0 && current.y < 3,
                          "route selected a disconnected port");
                    check(++distance <= 4, "route did not converge");
                }
                check(distance == std::abs(source % 3 - destination % 3) +
                                      std::abs(source / 3 - destination / 3),
                      "route is not a minimal dimension-order path");
                check(select_output_port({target, target, static_cast<unsigned>(destination)},
                                         config) == Local,
                      "destination did not select Local");
            }
        }
    }
}

ArbitrationCandidate offer(unsigned input) {
    ArbitrationCandidate candidate;
    candidate.input = input;
    return candidate;
}

void test_arbitration() {
    SystemConfig config;
    const std::vector<ArbitrationCandidate> offers{
        offer(6), offer(14), offer(3)};
    config.arbitration = "rr";
    check(select_router_input(offers, 13, config) == 14,
          "round robin did not wrap around input IDs");
    check(select_router_input(offers, 0, config) == 3,
          "round robin selected a vector index instead of an input ID");
    check(!select_router_input({}, 7, config),
          "empty arbitration set must return no selection");

    config.arbitration = "custom";
    const std::vector<ArbitrationCandidate> classes{
        offer(4), offer(8), offer(11), offer(2), offer(5)};
    check(select_router_input(classes, 0, config) == 2,
          "custom arbitration failed reply priority or tie-breaking");
}

MemoryRequest request(MemoryOperation operation, uint32_t address, uint32_t value = 0) {
    MemoryRequest result;
    result.operation = operation;
    result.address = address;
    result.value = value;
    return result;
}

void test_ordering() {
    SystemConfig config;
    config.store_entries = 2;
    MemoryOrderUnit order;
    check(decide_memory_action(request(MemoryOperation::Fence, 0), order, config).kind ==
              MemoryAction::CompleteFence,
          "empty-buffer fence did not complete");
    check(decide_memory_action(request(MemoryOperation::Exchange, 0), order, config).kind ==
              MemoryAction::Issue,
          "exchange with empty buffer did not issue");
    check(decide_memory_action(request(MemoryOperation::Store, 0, 1), order, config).kind ==
              MemoryAction::BufferStore,
          "available store-buffer capacity was not used");

    MemoryRequest older = request(MemoryOperation::Store, 0, 11);
    MemoryRequest younger = request(MemoryOperation::Store, 0, 22);
    order.stores = {older, younger};
    const auto forwarded =
        decide_memory_action(request(MemoryOperation::Load, 0), order, config);
    check(forwarded.kind == MemoryAction::Forward && forwarded.value == 22,
          "load did not forward the youngest same-word store");
    check(decide_memory_action(request(MemoryOperation::Load, 4), order, config).kind ==
              MemoryAction::Issue,
          "load to a different word did not bypass older stores");
    check(decide_memory_action(request(MemoryOperation::Store, 8, 33), order, config).kind ==
              MemoryAction::Wait,
          "full store buffer did not stall a store");
    check(decide_memory_action(request(MemoryOperation::Fence, 0), order, config).kind ==
              MemoryAction::Wait,
          "fence passed a nonempty store buffer");
    check(decide_memory_action(request(MemoryOperation::Exchange, 0, 33), order, config).kind ==
              MemoryAction::Wait,
          "exchange passed older buffered stores");
    check(decide_memory_action(
              request(static_cast<MemoryOperation>(99), 0), order, config).kind ==
              MemoryAction::Wait,
          "unknown operation did not fail closed to Wait");
}

void test_scheduler() {
    SystemConfig config;
    config.controllers = 1;
    config.banks = 4;
    MemoryController controller;
    controller.banks.resize(config.banks);
    Packet late;
    late.line_address = 0;
    late.controller_arrival = 8;
    Packet early;
    early.line_address = 64;
    early.controller_arrival = 3;
    Packet tied;
    tied.line_address = 128;
    tied.controller_arrival = 3;
    controller.queue = {late, early, tied};

    config.scheduler = "fcfs";
    check(select_memory_request({2, 0, 1}, controller, 10, config) == 1,
          "FCFS ignored controller arrival or queue-index tie-break");
    check(!select_memory_request({}, controller, 10, config),
          "empty eligible set must return no selection");
    check(select_memory_request({2, 1}, controller, 10, config) == 1,
          "FCFS did not break equal-arrival ties by queue index");

    config.scheduler = "custom";
    controller.banks[1].open_row = 0;
    Packet row_hit;
    row_hit.line_address = 64;
    row_hit.controller_arrival = 30;
    controller.queue = {late, row_hit};
    check(select_memory_request({0, 1}, controller, 10, config) == 1,
          "custom scheduler did not prioritize an eligible row hit");
    check(select_memory_request({0, 1}, controller, 100, config) == 0,
          "custom scheduler aging failed to prioritize a starved request");
    Packet first_starved;
    first_starved.line_address = 192;
    first_starved.controller_arrival = 10;
    Packet second_starved;
    second_starved.line_address = 256;
    second_starved.controller_arrival = 20;
    controller.queue = {first_starved, second_starved};
    check(select_memory_request({1, 0}, controller, 100, config) == 0,
          "custom scheduler did not prefer the oldest starved request");
    first_starved.controller_arrival = 100;
    second_starved.controller_arrival = 100;
    controller.queue = {first_starved, second_starved};
    check(select_memory_request({1, 0}, controller, 100, config) == 0,
          "custom scheduler did not break equal-tier ties deterministically");
    first_starved.controller_arrival = 110;
    controller.queue = {first_starved};
    check(select_memory_request({0}, controller, 100, config) == 0,
          "future arrival time underflowed while computing scheduler age");
}

}  // namespace

int main() {
    try {
        test_routes();
        std::cout << "PASS route_pairs_xy_yx\n";
        test_arbitration();
        std::cout << "PASS arbitration_wrap_raw_ids_custom_priority\n";
        test_ordering();
        std::cout << "PASS tso_forwarding_capacity_fence_exchange\n";
        test_scheduler();
        std::cout << "PASS scheduler_fcfs_row_hit_aging\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL extended_policy_tests: " << error.what() << '\n';
        return 1;
    }
}
