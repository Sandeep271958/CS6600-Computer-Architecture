#include "pa4.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace multicore {

MulticoreSystem::MulticoreSystem(SystemConfig config)
    : configuration(std::move(config)) {
    current.caches.resize(configuration.cores);
    for (auto& cache : current.caches) {
        cache.lines.resize(configuration.cache_lines);
    }
    current.network.inject.resize(configuration.tiles());
    current.llc.resize(configuration.homes);
    for (auto& bank : current.llc) {
        bank.entries.resize(configuration.llc_lines);
    }
    next = current;
}

int MulticoreSystem::home(uint32_t line) const {
    return configuration.cores + static_cast<int>((line / 64) % configuration.homes);
}

bool MulticoreSystem::can_send(int source, MessageType, unsigned count) const {
    return source >= 0 &&
           static_cast<size_t>(source) < next.network.inject.size() &&
           next.network.inject[source].size() + count <= configuration.injection_depth;
}

Packet MulticoreSystem::message(MessageType type, int source, int destination,
                                uint32_t line, uint64_t transaction,
                                const CacheLineData& data, int requester) const {
    Packet packet;
    packet.type = type;
    packet.source = source;
    packet.destination = destination;
    packet.line_address = line;
    packet.transaction_id = transaction;
    packet.requester = requester;
    packet.data = data;
    return packet;
}

bool MulticoreSystem::send(Packet packet) {
    if (!can_send(packet.source, packet.type)) {
        return false;
    }
    packet.id = packet_next++;
    packet.injected = cycle;
    next.network.inject[packet.source].push_back(std::move(packet));
    return true;
}

void MulticoreSystem::event(const std::string& component, const std::string& event_name,
                            uint64_t amount) {
    counters.values[component + "." + event_name] += amount;
}

void MulticoreSystem::cache_complete(int core, uint32_t value) {
    auto& cache = next.caches.at(core);
    if (!cache.pending) {
        throw std::runtime_error("cache completion without a pending request");
    }
    next.cache_results.at(core).push_back(
        {cache.pending->id, value, cycle});
    cache.pending.reset();
    cache.phase = "idle";
    observer.values["cache.completed"]++;
}

bool MulticoreSystem::llc_request(int home_tile, const LLCRequest& request) {
    const int bank = home_tile - configuration.cores;
    if (bank < 0 || bank >= configuration.homes) {
        throw std::runtime_error("callback sent LLC request to wrong bank");
    }
    next.llc[bank].requests.push_back(request);
    return true;
}

std::optional<LLCReply> MulticoreSystem::llc_reply(
    int home_tile, uint32_t line, uint64_t transaction, int requester) {
    const int bank = home_tile - configuration.cores;
    for (const auto& reply : current.llc.at(bank).replies) {
        if (reply.line_address != line || reply.transaction_id != transaction ||
            reply.requester != requester) {
            continue;
        }
        auto& staged = next.llc[bank].replies;
        const auto match = std::find_if(staged.begin(), staged.end(),
            [&](const LLCReply& candidate) {
                return candidate.line_address == line &&
                       candidate.transaction_id == transaction &&
                       candidate.requester == requester;
            });
        if (match == staged.end()) {
            return std::nullopt;
        }
        staged.erase(match);
        return reply;
    }
    return std::nullopt;
}

void count_event(MulticoreSystem& system, const std::string& component,
                 const std::string& event_name, uint64_t amount) {
    system.event(component, event_name, amount);
}

}  // namespace multicore

namespace {

using namespace multicore;

struct Scenario {
    std::string name;
    std::array<std::vector<MemoryRequest>, 2> operations;
    unsigned cache_lines;
    unsigned llc_lines;
};

struct SearchState {
    explicit SearchState(const SystemConfig& config) : system(config) {
        system.current.cache_results.resize(config.cores);
        system.next = system.current;
    }

    MulticoreSystem system;
    std::array<size_t, 2> submitted{};
    std::array<size_t, 2> completed{};
    std::map<uint64_t, uint32_t> observed_values;
};

struct SearchStats {
    size_t states = 0;
    size_t terminal_states = 0;
    size_t transitions = 0;
    size_t maximum_depth = 0;
};

void append_packet(std::ostringstream& out, const Packet& packet) {
    out << int(packet.type) << ',' << packet.source << ',' << packet.destination << ','
        << packet.requester << ',' << packet.line_address << ','
        << packet.transaction_id << ':';
    for (uint32_t word : packet.data) {
        out << word << ',';
    }
}

std::string state_key(const SearchState& state) {
    const auto& system = state.system;
    std::ostringstream out;
    out << state.submitted[0] << ',' << state.submitted[1] << ';'
        << state.completed[0] << ',' << state.completed[1] << ';';
    for (const auto& [id, value] : state.observed_values) {
        out << id << '=' << value << ',';
    }
    out << ';';
    for (int core = 0; core < 2; ++core) {
        const auto& cache = system.current.caches[core];
        out << core << ':' << cache.phase << ':' << cache.transaction_id << ':';
        if (cache.pending) {
            out << cache.pending->id << ',' << int(cache.pending->operation) << ','
                << cache.pending->address << ',' << cache.pending->value;
        }
        out << ':';
        if (cache.evicted_line_address) {
            out << *cache.evicted_line_address << ','
                << cache.evicted_line_transaction_id << ':';
            if (cache.evicted_line_data) {
                for (uint32_t word : *cache.evicted_line_data) {
                    out << word << ',';
                }
            }
        }
        for (const auto& line : cache.lines) {
            out << int(line.state) << ',' << line.tag << ',' << line.transient << ':';
            for (uint32_t word : line.data) {
                out << word << ',';
            }
        }
        out << ';';
        for (const auto& result : system.current.cache_results[core]) {
            out << result.id << ',' << result.value << ';';
        }
    }
    for (const auto& [line, entry] : system.current.directory) {
        out << "D" << line << ',' << int(entry.state) << ',' << entry.owner << ','
            << entry.owner_transaction_id << ',' << entry.active << ','
            << (entry.started == system.cycle) << ',' << entry.phase << ':';
        for (uint32_t word : entry.data) out << word << ',';
        out << ':';
        if (entry.active) {
            append_packet(out, entry.request);
        }
        for (int core : entry.sharers) {
            out << 's' << core << ','
                << entry.sharer_transaction_ids.at(core) << ';';
        }
        for (int core : entry.pending_snoops) out << 'p' << core << ';';
        for (int core : entry.pending_acknowledgements) out << 'a' << core << ';';
        for (const auto& packet : entry.waiting) {
            out << 'w';
            append_packet(out, packet);
        }
    }
    for (const auto& queue : system.current.network.inject) {
        out << "Q";
        for (const auto& packet : queue) append_packet(out, packet);
        out << ';';
    }
    for (const auto& bank : system.current.llc) {
        for (const auto& entry : bank.entries) {
            out << "L" << entry.valid << ',' << entry.dirty << ',' << entry.tag << ':';
            for (uint32_t word : entry.data) out << word << ',';
        }
        for (const auto& request : bank.requests) {
            out << "R" << request.line_address << ',' << request.transaction_id << ','
                << request.requester << ',' << request.write << ':';
            for (uint32_t word : request.data) out << word << ',';
        }
        for (const auto& reply : bank.replies) {
            out << "P" << reply.line_address << ',' << reply.transaction_id << ','
                << reply.requester << ':';
            for (uint32_t word : reply.data) out << word << ',';
        }
    }
    return out.str();
}

void check_invariants(const SearchState& state, const Scenario& scenario, bool terminal) {
    const auto& system = state.system;
    for (uint32_t line_address : {0u, 64u}) {
        std::vector<int> modified;
        std::vector<int> shared;
        for (int core = 0; core < 2; ++core) {
            const auto& cache = system.current.caches[core];
            for (const auto& line : cache.lines) {
                if (line.tag != line_address || line.transient ||
                    line.state == CacheState::I) {
                    continue;
                }
                if (line.state == CacheState::M) modified.push_back(core);
                if (line.state == CacheState::S) shared.push_back(core);
            }
        }
        if (modified.size() > 1 || (!modified.empty() && !shared.empty())) {
            throw std::runtime_error(scenario.name + ": MSI exclusivity invariant failed");
        }

        const auto found = system.current.directory.find(line_address);
        if (terminal && found != system.current.directory.end()) {
            const auto& directory = found->second;
            std::set<int> actual_sharers;
            std::vector<int> actual_owners;
            for (int core = 0; core < 2; ++core) {
                const auto& line = system.current.caches[core].lines[
                    system.slot(line_address)];
                if (line.tag != line_address || line.transient) continue;
                if (line.state == CacheState::M) actual_owners.push_back(core);
                if (line.state == CacheState::S) actual_sharers.insert(core);
            }
            if (directory.state == CacheState::M) {
                if (directory.owner < 0 || actual_owners.size() != 1 ||
                    actual_owners.front() != directory.owner ||
                    !actual_sharers.empty() ||
                    system.current.caches[directory.owner].lines[
                        system.slot(line_address)].state != CacheState::M) {
                    throw std::runtime_error(scenario.name + ": final owner mismatch");
                }
            }
            if (directory.state == CacheState::S) {
                if (directory.owner != -1 || actual_owners.size() != 0 ||
                    actual_sharers != directory.sharers) {
                    throw std::runtime_error(scenario.name + ": final sharer set mismatch");
                }
                std::optional<CacheLineData> shared_data;
                for (int sharer : directory.sharers) {
                    const auto& line = system.current.caches[sharer].lines[
                        system.slot(line_address)];
                    if (line.state != CacheState::S ||
                        (shared_data && *shared_data != line.data)) {
                        throw std::runtime_error(scenario.name + ": final sharer mismatch");
                    }
                    shared_data = line.data;
                }
            }
            if (directory.state == CacheState::I &&
                (!actual_owners.empty() || !actual_sharers.empty())) {
                throw std::runtime_error(scenario.name + ": final invalid state mismatch");
            }
        }
    }
}

bool is_terminal(const SearchState& state, const Scenario& scenario) {
    for (int core = 0; core < 2; ++core) {
        if (state.submitted[core] != scenario.operations[core].size() ||
            state.completed[core] != scenario.operations[core].size() ||
            state.system.current.caches[core].pending) {
            return false;
        }
    }
    for (const auto& queue : state.system.current.network.inject) {
        if (!queue.empty()) return false;
    }
    for (const auto& [line, entry] : state.system.current.directory) {
        (void)line;
        if (entry.active || !entry.waiting.empty()) return false;
    }
    for (const auto& bank : state.system.current.llc) {
        if (!bank.requests.empty() || !bank.replies.empty()) return false;
    }
    return true;
}

void process_results(SearchState& state, const Scenario& scenario) {
    for (int core = 0; core < 2; ++core) {
        auto& results = state.system.next.cache_results[core];
        while (!results.empty()) {
            const MemoryResult result = results.front();
            results.pop_front();
            const size_t position = state.completed[core];
            if (position >= scenario.operations[core].size() ||
                scenario.operations[core][position].id != result.id) {
                throw std::runtime_error(scenario.name + ": per-core completion order failed");
            }
            const auto& request = scenario.operations[core][position];
            if (request.operation == MemoryOperation::Store && result.value != 0) {
                 throw std::runtime_error(scenario.name + ": store result was not zero");
            }
            state.observed_values[result.id] = result.value;
            ++state.completed[core];
        }
    }
}

uint32_t authoritative_value(const SearchState& state, uint32_t address) {
    const auto& system = state.system;
    const uint32_t line_address = system.line_address(address);
    const unsigned word = (address % 64) / 4;
    const auto directory = system.current.directory.find(line_address);
    if (directory != system.current.directory.end() &&
        directory->second.state == CacheState::M) {
        return system.current.caches[directory->second.owner]
            .lines[system.slot(line_address)].data[word];
    }
    if (directory != system.current.directory.end() &&
        directory->second.state == CacheState::S &&
        !directory->second.sharers.empty()) {
        const int sharer = *directory->second.sharers.begin();
        return system.current.caches[sharer]
            .lines[system.slot(line_address)].data[word];
    }
    const unsigned slot = (line_address / 64) % system.configuration.llc_lines;
    const auto& entry = system.current.llc[0].entries[slot];
    return entry.valid && entry.tag == line_address ? entry.data[word] : 0;
}

bool has_sequential_order(const SearchState& state, const Scenario& scenario,
                           size_t core0, size_t core1,
                           std::map<uint32_t, uint32_t>& memory) {
    if (core0 == scenario.operations[0].size() &&
        core1 == scenario.operations[1].size()) {
        for (const auto& operations : scenario.operations) {
            for (const auto& request : operations) {
                 const auto found = memory.find(request.address);
                 const uint32_t expected = found == memory.end() ? 0 : found->second;
                 if (authoritative_value(state, request.address) != expected) {
                     return false;
                 }
            }
        }
        return true;
    }

    for (int core = 0; core < 2; ++core) {
        size_t& position = core == 0 ? core0 : core1;
        if (position == scenario.operations[core].size()) continue;
        const auto& request = scenario.operations[core][position];
        const auto old = memory.find(request.address);
        const uint32_t old_value = old == memory.end() ? 0 : old->second;
        const uint32_t observed = state.observed_values.at(request.id);
        if ((request.operation == MemoryOperation::Load ||
             request.operation == MemoryOperation::Exchange) &&
            observed != old_value) {
            continue;
        }

        const auto prior = memory.find(request.address);
        const bool had_prior = prior != memory.end();
        const uint32_t prior_value = had_prior ? prior->second : 0;
        if (request.operation == MemoryOperation::Store ||
            request.operation == MemoryOperation::Exchange) {
            memory[request.address] = request.value;
        }
        ++position;
        const bool valid = has_sequential_order(state, scenario, core0, core1, memory);
        --position;
        if (had_prior) memory[request.address] = prior_value;
        else memory.erase(request.address);
        if (valid) return true;
    }
    return false;
}

void advance(SearchState& state, const Scenario& scenario) {
    auto& system = state.system;
    for (int core = 0; core < 2; ++core) {
        if (state.submitted[core] >= scenario.operations[core].size() ||
            system.next.caches[core].pending) {
            continue;
        }
        const auto& request = scenario.operations[core][state.submitted[core]];
        if (accept_cache_request(system, core, request)) {
            ++state.submitted[core];
        }
    }

    std::vector<uint32_t> lines;
    for (const auto& [line, entry] : system.next.directory) {
        (void)entry;
        lines.push_back(line);
    }
    for (uint32_t line : lines) {
        advance_directory_transaction(system, line);
    }

    for (size_t index = 0; index < system.current.llc.size(); ++index) {
        auto& staged = system.next.llc[index];
        for (const auto& request : system.current.llc[index].requests) {
            if (staged.requests.empty()) {
                throw std::runtime_error("LLC request queue lost staged state");
            }
            staged.requests.pop_front();
            const unsigned slot = (request.line_address / 64) %
                                  system.configuration.llc_lines;
            auto& entry = staged.entries[slot];
            CacheLineData data{};
            if (request.write) {
                entry = {true, true, request.line_address, request.data};
            } else if (entry.valid && entry.tag == request.line_address) {
                data = entry.data;
            } else {
                entry = {true, false, request.line_address, data};
            }
            staged.replies.push_back(
                {request.line_address, request.transaction_id, request.requester, data});
        }
    }
    process_results(state, scenario);
    system.current = system.next;
}

void explore(SearchState state, const Scenario& scenario, SearchStats& stats,
             std::set<std::string>& visited, size_t depth) {
    stats.maximum_depth = std::max(stats.maximum_depth, depth);
    const std::string key = state_key(state);
    if (!visited.insert(key).second) return;
    ++stats.states;
    if (stats.states > 250000) {
        throw std::runtime_error(scenario.name + ": state bound exceeded");
    }
    if (is_terminal(state, scenario)) {
        check_invariants(state, scenario, true);
        std::map<uint32_t, uint32_t> memory;
        if (!has_sequential_order(state, scenario, 0, 0, memory)) {
            throw std::runtime_error(scenario.name +
                                     ": no sequentially consistent completion order");
        }
        ++stats.terminal_states;
        return;
    }
    if (depth > 96) {
        throw std::runtime_error(scenario.name + ": transition bound exceeded");
    }

    std::vector<int> senders;
    for (int source = 0;
         source < static_cast<int>(state.system.current.network.inject.size()); ++source) {
        if (!state.system.current.network.inject[source].empty()) {
            senders.push_back(source);
        }
    }

    bool had_successor = false;
    for (int source : senders) {
        SearchState child = state;
        auto& system = child.system;
        system.cycle = state.system.cycle + 1;
        system.next = system.current;
        Packet packet = system.next.network.inject[source].front();
        system.next.network.inject[source].pop_front();
        const bool consumed = packet.destination < system.configuration.cores
            ? handle_cache_message(system, packet.destination, packet)
            : handle_directory_message(system, packet);
        if (!consumed) continue;
        advance(child, scenario);
        check_invariants(child, scenario, false);
        had_successor = true;
        ++stats.transitions;
        explore(std::move(child), scenario, stats, visited, depth + 1);
    }

    SearchState idle = state;
    idle.system.cycle = state.system.cycle + 1;
    idle.system.next = idle.system.current;
    advance(idle, scenario);
    if (state_key(idle) != key) {
        had_successor = true;
        ++stats.transitions;
        explore(std::move(idle), scenario, stats, visited, depth + 1);
    }

    if (!had_successor) {
        std::ostringstream detail;
        detail << scenario.name << ": reachable protocol dead end";
        for (size_t source = 0;
             source < state.system.current.network.inject.size(); ++source) {
            for (const auto& packet : state.system.current.network.inject[source]) {
                detail << " packet=" << int(packet.type) << ':' << packet.source
                       << "->" << packet.destination << " line=" << packet.line_address
                       << " tx=" << packet.transaction_id;
            }
        }
        for (const auto& [line, entry] : state.system.current.directory) {
            detail << " dir=" << line << ':' << entry.phase << ":active="
                   << entry.active << ":acks=" << entry.pending_acknowledgements.size();
        }
        throw std::runtime_error(detail.str());
    }
}

Scenario make_scenario(std::string name,
                       std::vector<MemoryRequest> core0,
                       std::vector<MemoryRequest> core1,
                       unsigned cache_lines) {
    return {std::move(name), {std::move(core0), std::move(core1)}, cache_lines, 2};
}

MemoryRequest request(uint64_t id, int core, MemoryOperation operation,
                      uint32_t address, uint32_t value = 0) {
    return {id, core, operation, address, value, 0};
}

void run_scenario(const Scenario& scenario) {
    SystemConfig config;
    config.x = 2;
    config.y = 2;
    config.cores = 2;
    config.homes = 1;
    config.controllers = 1;
    config.cache_lines = scenario.cache_lines;
    config.llc_lines = scenario.llc_lines;
    config.injection_depth = 1;
    config.router_depth = 2;
    SearchState initial(config);
    SearchStats stats;
    std::set<std::string> visited;
    explore(std::move(initial), scenario, stats, visited, 0);
    if (stats.terminal_states == 0) {
        throw std::runtime_error(scenario.name + ": no complete execution found");
    }
    std::cout << "PASS " << scenario.name << ": " << stats.states << " unique states, "
              << stats.transitions << " transitions, " << stats.terminal_states
              << " terminal states, depth <= " << stats.maximum_depth << '\n';
}

}  // namespace

int main() {
    try {
        run_scenario(make_scenario(
            "dirty-eviction-recall",
            {request(1, 0, MemoryOperation::Store, 0, 101),
             request(2, 0, MemoryOperation::Store, 64, 202)},
            {request(101, 1, MemoryOperation::Load, 0),
             request(102, 1, MemoryOperation::Store, 0, 303)},
            1));
        run_scenario(make_scenario(
            "shared-upgrade-writers",
            {request(1, 0, MemoryOperation::Load, 0),
             request(2, 0, MemoryOperation::Store, 0, 404)},
            {request(101, 1, MemoryOperation::Load, 0),
             request(102, 1, MemoryOperation::Store, 0, 505)},
            1));
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
