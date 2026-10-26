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

        if (victim_needed && entry.state == CacheState::M &&
            cache.evicted_line_address.has_value()) {
            return false;
        }

        // Release the victim first; the acquisition waits for PutAck so even a
        // one-entry injection queue can make progress.
        if (victim_needed) {
            MessageType victim_msg = (entry.state == CacheState::M)
                                         ? MessageType::PutM
                                         : MessageType::PutS;
            if (!system.can_send(core_id, victim_msg, 1)) {
                return false;
            }
        } else {
            if (!system.can_send(core_id, acq_type, 1)) {
                return false;
            }
        }

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
        } else {
            Packet acq_pkt = system.message(acq_type, core_id, home_tile, line_address, request.id, {}, core_id);
            if (!system.send(acq_pkt)) {
                return false;
            }
        }

        // All sends succeeded; only now commit cache state and acceptance events.
        if (victim_needed) {
            // Keep dirty bytes available for recalls until the home confirms release.
            cache.evicted_line_address = victim_address;
            cache.evicted_line_data = entry.state == CacheState::M
                                          ? std::optional<CacheLineData>(entry.data)
                                          : std::nullopt;
            cache.evicted_line_transaction_id = request.id;
            entry.transient = true;
            if (entry.state == CacheState::M) {
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
        cache.phase = victim_needed ? "waiting_putack" : "waiting_data";
        return true;
    }

    return false;
}

bool handle_cache_message(MulticoreSystem& system, int core_id, const Packet& packet) {
    // PA4-COH-02: Handle grants, recalls, invalidations and eviction
    // acknowledgements. A false return must leave staged state unchanged and send
    // no messages.

    PrivateCache& cache = system.next.caches[core_id];
    int slot = system.slot(packet.line_address);
    CacheLine& entry = cache.lines[slot];

    // Case 1: Data Grant Installation (DataS or DataM)
    if (packet.type == MessageType::DataS || packet.type == MessageType::DataM) {
        if (!cache.pending.has_value() || cache.transaction_id != packet.transaction_id) {
            // Has no pending value or the ID being processed is not that packet
            // Must retry next cycle
            return false;
        }
        if (system.line_address(cache.pending->address) != packet.line_address) {
            return false;
        }

        if (!system.can_send(core_id, MessageType::GrantAck, 1))
            return false;

        const CacheLine previous_entry = entry;
        entry.data = packet.data;
        entry.state = packet.type == MessageType::DataM ? CacheState::M : CacheState::S;
        entry.tag = packet.line_address;
        entry.transient = false;

        const MemoryRequest& req = *cache.pending;
        int word_offset = (req.address % 64) / 4;
        uint32_t result_val = 0;

        if (req.operation == MemoryOperation::Load) {
            result_val = packet.data[word_offset];
        } else if (req.operation == MemoryOperation::Store) {
            entry.data[word_offset] = req.value;
        } else if (req.operation == MemoryOperation::Exchange) {
            result_val = packet.data[word_offset];
            entry.data[word_offset] = req.value;
        }

        Packet grant_ack = system.message(MessageType::GrantAck, core_id, packet.source, packet.line_address,
                                          packet.transaction_id, {}, core_id);
        if (!system.send(grant_ack)) {
            entry = previous_entry;
            return false;
        }

        // Complete only after applying the access and successfully sending its ack.
        system.cache_complete(core_id, result_val);
        return true;
    }

    // Case 2: Invalidate Messages (Inv)
    if (packet.type == MessageType::Inv) {
        if (!system.can_send(core_id, MessageType::Ack, 1))
            return false;

        const CacheLine previous_entry = entry;
        if (entry.tag == packet.line_address && entry.state != CacheState::I) {
            entry.state = CacheState::I;
        }

        Packet ack = system.message(MessageType::Ack, core_id, packet.source, packet.line_address,
                                    packet.transaction_id, {}, packet.requester);
        if (!system.send(ack)) {
            entry = previous_entry;
            return false;
        }
        system.event("cache." + std::to_string(core_id), "invalidations_received", 1);
        if (previous_entry.tag == packet.line_address &&
            previous_entry.state != CacheState::I) {
            system.event("cache." + std::to_string(core_id), "invalidations_applied", 1);
        }
        system.event("cache." + std::to_string(core_id), "acks_sent", 1);
        return true;
    }

    // Case 3: Recall Shared (RecallS)
    if (packet.type == MessageType::RecallS) {
        if (!system.can_send(core_id, MessageType::Ack, 1))
            return false;

        const bool matching_line =
            entry.tag == packet.line_address && entry.state != CacheState::I;
        const bool matching_victim =
            cache.evicted_line_address == packet.line_address &&
            cache.evicted_line_data.has_value();
        CacheLineData dirty_data{};
        if (matching_line) {
            dirty_data = entry.data;
        } else if (matching_victim) {
            dirty_data = *cache.evicted_line_data;
        }

        Packet ack = system.message(MessageType::Ack, core_id, packet.source, packet.line_address,
                                    packet.transaction_id, dirty_data, packet.requester);
        const CacheLine previous_entry = entry;
        if (matching_line && entry.state == CacheState::M)
            entry.state = CacheState::S;
        if (!system.send(ack)) {
            entry = previous_entry;
            return false;
        }
        system.event("cache." + std::to_string(core_id), "acks_sent", 1);
        return true;
    }

    // Case 4: Recall Invalid (RecallI)
    if (packet.type == MessageType::RecallI) {
        if (!system.can_send(core_id, MessageType::Ack, 1))
            return false;

        const bool matching_line =
            entry.tag == packet.line_address && entry.state != CacheState::I;
        const bool matching_victim =
            cache.evicted_line_address == packet.line_address &&
            cache.evicted_line_data.has_value();
        CacheLineData dirty_data{};

        if (matching_line && entry.state == CacheState::M) {
            dirty_data = entry.data;
        } else if (matching_victim) {
            dirty_data = *cache.evicted_line_data;
        }

        Packet ack = system.message(MessageType::Ack, core_id, packet.source, packet.line_address,
                                    packet.transaction_id, dirty_data, packet.requester);

        const CacheLine previous_entry = entry;
        if (matching_line)
            entry.state = CacheState::I;
        if (!system.send(ack)) {
            entry = previous_entry;
            return false;
        }
        system.event("cache." + std::to_string(core_id), "acks_sent", 1);
        return true;
    }

    // Case 5: Put Acknowledgement (PutAck)
    if (packet.type == MessageType::PutAck) {
        if (cache.evicted_line_address != packet.line_address ||
            cache.evicted_line_transaction_id != packet.transaction_id) {
            return true;
        }

        if (cache.phase == "waiting_putack" && cache.pending.has_value()) {
            const MemoryRequest& request = *cache.pending;
            const uint32_t line_address = system.line_address(request.address);
            const MessageType acquisition =
                request.operation == MemoryOperation::Load ? MessageType::GetS
                                                          : MessageType::GetM;
            if (!system.can_send(core_id, acquisition, 1)) {
                return false;
            }

            // Do not consume PutAck until the replacement request can be queued.
            Packet acquire = system.message(acquisition, core_id,
                                            system.home(line_address), line_address,
                                            request.id, {}, core_id);
            if (!system.send(acquire)) {
                return false;
            }
        }

        cache.evicted_line_address = std::nullopt;
        cache.evicted_line_data = std::nullopt;
        cache.evicted_line_transaction_id = 0;
        if (entry.tag == packet.line_address && entry.transient) {
            entry.state = CacheState::I;
            entry.transient = false;
        }
        if (cache.phase == "waiting_putack") {
            cache.phase = "waiting_data";
        }
        return true;
    }
    return false;
}

}  // namespace multicore
