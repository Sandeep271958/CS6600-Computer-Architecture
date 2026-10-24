#include "pa4.h"
namespace multicore {

bool accept_cache_request(MulticoreSystem& system, int core_id, const MemoryRequest& request) {
    // PA4-COH-01: Implement permission checks, victim release, acquisition and
    // local completion. Return true only when this CPU request is accepted.

    PrivateCache& cache = system.next.caches[core_id];

    // Check for Active Pending Requests
    if (cache.pending.has_value()) {
        return false;
    }

    // Check Operation
    if (request.operation == MemoryOperation::Fence) {
        return false;
    }

    // Address Decomposition
    // Calculate Indices
    uint32_t line_address = system.line_address(request.address);
    int word_offset = (request.address % 64) / 4;
    int slot = system.slot(request.address);
    CacheLine& entry = cache.lines[slot];

    // Tag Match and Permissions
    bool tag_match = (entry.tag == line_address);
    bool has_read_permission = tag_match && (entry.state == CacheState::S || entry.state == CacheState::M);
    bool has_write_permission = tag_match && (entry.state == CacheState::M);

    // Load Hit: Read Permission (S or M)
    if (request.operation == MemoryOperation::Load && has_read_permission) {
        cache.pending = request;
        system.cache_complete(core_id, entry.data[word_offset]);
        system.event("cache." + std::to_string(core_id), "loads_accepted", 1);
        system.event("cache." + std::to_string(core_id), "load_permission_hits", 1);
        return true;
    }
    // Store Hit: Write Permission (M)
    else if (request.operation == MemoryOperation::Store && has_write_permission) {
        entry.data[word_offset] = request.value;
        cache.pending = request;
        system.cache_complete(core_id, 0);  // Stores return value 0
        system.event("cache." + std::to_string(core_id), "stores_accepted", 1);
        system.event("cache." + std::to_string(core_id), "store_permission_hits", 1);
        return true;
    }
    // Exchange Hit: Write Permission (M)
    else if (request.operation == MemoryOperation::Exchange && has_write_permission) {
        uint32_t old_value = entry.data[word_offset];
        entry.data[word_offset] = request.value;
        cache.pending = request;
        system.cache_complete(core_id, old_value);  // Atomic XCHG returns old value
        system.event("cache." + std::to_string(core_id), "atomics_accepted", 1);
        system.event("cache." + std::to_string(core_id), "atomic_permission_hits", 1);
        return true;
    }

    // Miss/Upgrade Path
    else {
        int home_tile = system.home(line_address);
        MessageType acq_type = MessageType::GetS;

        if (request.operation == MemoryOperation::Load) {
            acq_type = MessageType::GetS;
        } else if (request.operation == MemoryOperation::Store || request.operation == MemoryOperation::Exchange) {
            acq_type = MessageType::GetM;
        }

        // Victim Eviction Check
        bool victim_needed = (entry.state != CacheState::I && !tag_match);
        uint32_t victim_address = entry.tag;
        int victim_home = system.home(victim_address);

        // Preflight capacity checks before mutating state or emitting events
        if (victim_needed) {
            MessageType victim_msg = (entry.state == CacheState::M) ? MessageType::PutM : MessageType::PutS;
            if (!system.can_send(core_id, victim_msg, 2)) {
                return false;
            }
        } else {
            if (!system.can_send(core_id, acq_type, 1)) {
                return false;
            }
        }

        // Preflight accounts for both packets in the same request channel, so
        // neither send can fail for capacity during this callback.
        if (victim_needed) {
            if (entry.state == CacheState::M) {
                Packet put_pkt = system.message(MessageType::PutM, core_id, victim_home, victim_address, request.id,
                                                entry.data, core_id);
                if (!system.send(put_pkt)) {
                    return false;
                }
            } else if (entry.state == CacheState::S) {
                Packet put_pkt = system.message(MessageType::PutS, core_id, victim_home, victim_address, request.id, {},
                                                core_id);
                if (!system.send(put_pkt)) {
                    return false;
                }
            }
        }

        Packet acq_pkt = system.message(acq_type, core_id, home_tile, line_address, request.id, {}, core_id);
        if (!system.send(acq_pkt)) {
            return false;
        }

        // All sends succeeded; only now commit cache state and acceptance events.
        if (victim_needed) {
            if (entry.state == CacheState::M) {
                cache.evicted_line_address = victim_address;
                entry.transient = true;
                system.event("cache." + std::to_string(core_id), "dirty_evictions", 1);
                system.event("cache." + std::to_string(core_id), "writebacks_sent", 1);
            } else if (entry.state == CacheState::S) {
                entry.state = CacheState::I;
            }
        }

        if (request.operation == MemoryOperation::Load) {
            system.event("cache." + std::to_string(core_id), "loads_accepted", 1);
            system.event("cache." + std::to_string(core_id), "read_misses", 1);
        } else if (request.operation == MemoryOperation::Store) {
            system.event("cache." + std::to_string(core_id), "stores_accepted", 1);
            if (tag_match && entry.state == CacheState::S) {
                system.event("cache." + std::to_string(core_id), "upgrades", 1);
            } else {
                system.event("cache." + std::to_string(core_id), "write_misses", 1);
            }
        } else if (request.operation == MemoryOperation::Exchange) {
            system.event("cache." + std::to_string(core_id), "atomics_accepted", 1);
            system.event("cache." + std::to_string(core_id), "atomic_misses", 1);
        }

        // Move Request to Pending
        cache.pending = request;
        cache.transaction_id = request.id;
        cache.phase = "waiting_data";
        return true;
    }

    return false;
}

bool handle_cache_message(MulticoreSystem& /* system */, int /* core_id */, const Packet& /* packet */) {
    // PA4-COH-02: Handle grants, recalls, invalidations and eviction
    // acknowledgements. A false return must leave staged state unchanged and send
    // no messages.
    not_implemented("PA4-COH-02");
}

}  // namespace multicore
