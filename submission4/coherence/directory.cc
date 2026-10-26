#include "pa4.h"
namespace multicore {

    namespace {

        std::string directory_component(const MulticoreSystem& system,
                                         uint32_t line_address) {
            return "directory." +
                   std::to_string(system.home(line_address) - system.configuration.cores);
        }

        void start_transaction(MulticoreSystem& system, DirectoryEntry& entry,
                               const Packet& request, uint32_t line_address) {
            entry.active = true;
            entry.request = request;
            entry.started = system.cycle;
            entry.phase = "start";
            entry.pending_snoops.clear();
            entry.pending_acknowledgements.clear();
            count_event(system, directory_component(system, line_address),
                        "transactions_started", 1);
        }

        void finish_transaction(MulticoreSystem& system, DirectoryEntry& entry,
                                uint32_t line_address) {
            const Cycle latency = system.cycle - entry.started;
            const std::string component = directory_component(system, line_address);
            count_event(system, component, "transactions_completed", 1);
            count_event(system, component, "completed_latency_sum", latency);

            entry.active = false;
            entry.phase = "idle";
            entry.pending_snoops.clear();
            entry.pending_acknowledgements.clear();
            if (!entry.waiting.empty()) {
                const Packet next = entry.waiting.front();
                entry.waiting.pop_front();
                start_transaction(system, entry, next, line_address);
            }
        }

        bool send_request_reply(MulticoreSystem& system, MessageType type, int home,
                                const Packet& request,
                                const CacheLineData& data = {}) {
            const int destination = type == MessageType::PutAck
                                        ? request.source
                                        : request.requester;
            return system.send(system.message(type, home, destination,
                                               request.line_address,
                                               request.transaction_id, data,
                                               request.requester));
        }

        bool is_directory_request(MessageType type) {
            return type == MessageType::GetS || type == MessageType::GetM ||
                   type == MessageType::PutS || type == MessageType::PutM;
        }

    }

    bool handle_directory_message(MulticoreSystem& system, const Packet& packet) {
        const uint32_t line_address = packet.line_address;
        const std::string component = directory_component(system, line_address);

        if (is_directory_request(packet.type)) {
            DirectoryEntry& entry = system.next.directory[line_address];
            if (entry.active && entry.waiting.size() >= system.configuration.router_depth) {
                return false;
            }

            count_event(system, component, "requests_accepted", 1);
            if (entry.active) {
                entry.waiting.push_back(packet);
                count_event(system, component, "conflicts_queued", 1);
            } else {
                start_transaction(system, entry, packet, line_address);
            }
            return true;
        }

        auto found = system.next.directory.find(line_address);
        if (found == system.next.directory.end()) {
            return false;
        }
        DirectoryEntry& entry = found->second;

        if (packet.type == MessageType::Ack) {
            if (!entry.active ||
                packet.transaction_id != entry.request.transaction_id ||
                entry.pending_acknowledgements.count(packet.source) == 0) {
                return false;
            }
            if (entry.phase != "gets_recall_wait" &&
                entry.phase != "getm_recall_wait" &&
                entry.phase != "getm_invalidate_send" &&
                entry.phase != "getm_invalidate_wait") {
                return false;
            }

            entry.pending_acknowledgements.erase(packet.source);
            count_event(system, component, "acks_received", 1);

            if (entry.phase == "gets_recall_wait") {
                entry.data = packet.data;
                entry.sharer_transaction_ids[packet.source] =
                    entry.owner_transaction_id;
                entry.owner = -1;
                entry.owner_transaction_id = 0;
                entry.state = CacheState::S;
                entry.sharers.insert(packet.source);
                if (entry.pending_acknowledgements.empty()) {
                    entry.phase = "gets_dirty_write_start";
                }
            } else if (entry.phase == "getm_recall_wait") {
                entry.data = packet.data;
                entry.owner = -1;
                entry.owner_transaction_id = 0;
                entry.sharers.erase(packet.source);
                entry.sharer_transaction_ids.erase(packet.source);
                entry.state = entry.sharers.empty() ? CacheState::I : CacheState::S;
                if (entry.pending_acknowledgements.empty()) {
                    entry.phase = "getm_grant";
                }
            } else {
                entry.sharers.erase(packet.source);
                entry.sharer_transaction_ids.erase(packet.source);
                if (entry.pending_snoops.empty() &&
                    entry.pending_acknowledgements.empty()) {
                    entry.state = entry.sharers.empty() ? CacheState::I : CacheState::S;
                    entry.phase = "getm_llc_start";
                } else if (entry.pending_snoops.empty()) {
                    entry.phase = "getm_invalidate_wait";
                }
            }
            return true;
        }

        if (packet.type == MessageType::GrantAck) {
            if (!entry.active ||
                packet.transaction_id != entry.request.transaction_id ||
                packet.source != entry.request.requester ||
                entry.phase != "grant_ack_wait") {
                return false;
            }
            finish_transaction(system, entry, line_address);
            return true;
        }

        return false;
    }

    void advance_directory_transaction(MulticoreSystem& system, uint32_t line_address) {
        auto found = system.next.directory.find(line_address);
        if (found == system.next.directory.end()) {
            return;
        }
        DirectoryEntry& entry = found->second;
        if (!entry.active || entry.started >= system.cycle) {
            return;
        }

        const Packet& request = entry.request;
        const int home = system.home(line_address);
        const int requester = request.requester;
        const std::string component = directory_component(system, line_address);

        if (entry.phase == "start") {
            if (request.type == MessageType::GetS) {
                if (entry.state == CacheState::M && entry.owner >= 0 &&
                    entry.owner != requester) {
                    entry.pending_snoops.insert(entry.owner);
                    entry.phase = "gets_recall_send";
                } else {
                    entry.phase = "gets_llc_start";
                }
            } else if (request.type == MessageType::GetM) {
                if (entry.state == CacheState::M && entry.owner >= 0 &&
                    entry.owner != requester) {
                    entry.pending_snoops.insert(entry.owner);
                    entry.phase = "getm_recall_send";
                } else {
                    for (int sharer : entry.sharers) {
                        if (sharer != requester) {
                            entry.pending_snoops.insert(sharer);
                        }
                    }
                    entry.phase = entry.pending_snoops.empty()
                                      ? "getm_llc_start"
                                      : "getm_invalidate_send";
                }
            } else if (request.type == MessageType::PutM) {
                // A delayed release is acknowledged but must not replace a newer owner.
                if (entry.state == CacheState::M && entry.owner == request.source &&
                    request.transaction_id >= entry.owner_transaction_id) {
                    entry.data = request.data;
                    entry.phase = "putm_llc_start";
                } else {
                    entry.phase = "put_ack_send";
                }
            } else if (request.type == MessageType::PutS) {
                entry.phase = "put_ack_send";
            }
            return;
        }

        if (entry.phase == "gets_recall_send" ||
            entry.phase == "getm_recall_send" ||
            entry.phase == "getm_invalidate_send") {
            if (entry.pending_snoops.empty()) {
                if (entry.phase == "gets_recall_send") {
                    entry.phase = "gets_recall_wait";
                } else if (entry.phase == "getm_recall_send") {
                    entry.phase = "getm_recall_wait";
                } else {
                    entry.phase = entry.pending_acknowledgements.empty()
                                      ? "getm_llc_start"
                                      : "getm_invalidate_wait";
                }
                return;
            }

            const int target = *entry.pending_snoops.begin();
            const MessageType type = entry.phase == "gets_recall_send"
                                         ? MessageType::RecallS
                                     : entry.phase == "getm_recall_send"
                                         ? MessageType::RecallI
                                         : MessageType::Inv;
            if (!system.can_send(home, type)) {
                return;
            }
            Packet snoop = system.message(type, home, target, line_address,
                                          request.transaction_id, {}, requester);
            if (!system.send(std::move(snoop))) {
                return;
            }
            entry.pending_snoops.erase(target);
            entry.pending_acknowledgements.insert(target);
            count_event(system, component,
                        type == MessageType::Inv ? "invalidations_sent" : "recalls_sent", 1);
            if (entry.pending_snoops.empty()) {
                if (type == MessageType::RecallS) {
                    entry.phase = "gets_recall_wait";
                } else if (type == MessageType::RecallI) {
                    entry.phase = "getm_recall_wait";
                } else if (entry.pending_acknowledgements.empty()) {
                    entry.phase = "getm_llc_start";
                } else {
                    entry.phase = "getm_invalidate_wait";
                }
            }
            return;
        }

        if (entry.phase == "gets_dirty_write_start" ||
            entry.phase == "putm_llc_start") {
            LLCRequest llc_request{
                line_address, request.transaction_id, requester, true, entry.data,
                system.cycle
            };
            if (system.llc_request(home, llc_request)) {
                entry.phase = entry.phase == "gets_dirty_write_start"
                                  ? "gets_dirty_write_wait"
                                  : "putm_llc_wait";
            }
            return;
        }

        if (entry.phase == "gets_llc_start" || entry.phase == "getm_llc_start") {
            LLCRequest llc_request{
                line_address, request.transaction_id, requester, false, {}, system.cycle
            };
            if (system.llc_request(home, llc_request)) {
                entry.phase = entry.phase == "gets_llc_start"
                                  ? "gets_llc_wait"
                                  : "getm_llc_wait";
            }
            return;
        }

        if (entry.phase == "gets_dirty_write_wait") {
            auto reply = system.llc_reply(home, line_address,
                                          request.transaction_id, requester);
            if (reply) {
                entry.data = reply->data;
                entry.phase = "gets_grant";
            }
            return;
        }

        if (entry.phase == "putm_llc_wait") {
            if (system.llc_reply(home, line_address, request.transaction_id,
                                 requester)) {
                entry.phase = "put_ack_send";
            }
            return;
        }

        if (entry.phase == "gets_llc_wait" || entry.phase == "getm_llc_wait") {
            auto reply = system.llc_reply(home, line_address,
                                          request.transaction_id, requester);
            if (reply) {
                entry.data = reply->data;
                entry.phase = entry.phase == "gets_llc_wait"
                                  ? "gets_grant"
                                  : "getm_grant";
            }
            return;
        }

        if (entry.phase == "gets_grant" || entry.phase == "getm_grant") {
            const bool exclusive = entry.phase == "getm_grant";
            const MessageType type = exclusive ? MessageType::DataM : MessageType::DataS;
            if (!system.can_send(home, type)) {
                return;
            }
            if (!send_request_reply(system, type, home, request, entry.data)) {
                return;
            }
            if (exclusive) {
                entry.state = CacheState::M;
                entry.owner = requester;
                entry.owner_transaction_id = request.transaction_id;
                entry.sharers.clear();
                entry.sharer_transaction_ids.clear();
                count_event(system, component, "ownership_grants", 1);
            } else {
                entry.state = CacheState::S;
                entry.owner = -1;
                entry.owner_transaction_id = 0;
                entry.sharers.insert(requester);
                entry.sharer_transaction_ids[requester] = request.transaction_id;
            }
            entry.phase = "grant_ack_wait";
            return;
        }

        if (entry.phase == "put_ack_send") {
            if (!system.can_send(home, MessageType::PutAck)) {
                return;
            }
            if (!send_request_reply(system, MessageType::PutAck, home, request)) {
                return;
            }
            if (request.type == MessageType::PutM &&
                entry.state == CacheState::M && entry.owner == request.source &&
                request.transaction_id >= entry.owner_transaction_id) {
                entry.owner = -1;
                entry.owner_transaction_id = 0;
                entry.sharers.clear();
                entry.sharer_transaction_ids.clear();
                entry.state = CacheState::I;
            } else if (request.type == MessageType::PutS) {
                auto sharer = entry.sharer_transaction_ids.find(request.source);
                if (sharer != entry.sharer_transaction_ids.end() &&
                    request.transaction_id >= sharer->second) {
                    entry.sharers.erase(request.source);
                    entry.sharer_transaction_ids.erase(sharer);
                }
                if (entry.sharers.empty() && entry.state == CacheState::S) {
                    entry.state = CacheState::I;
                }
            }
            finish_transaction(system, entry, line_address);
        }

    }

}
