# Assignment 4: three-person workload and integration guide

Planning document for the team after PA3 migration. It assigns the remaining PA4 implementation without adding that implementation now. The source of requirements is `Assignment_4.md`; the exact callback and counter rules are in `README.md`, `docs/INTERFACES.md`, and `docs/COUNTERS.md`. The assignment states a 28 October deadline. Confirm any later course announcement before planning a final push.

## 1. Ownership and effort

The split below estimates engineering and integration effort, rather than equating effort with marks. Each owner writes and tests their files, documents transient states or policy choices in code review, and explains one complete memory transaction in the viva. All three review the interfaces that cross ownership boundaries.

| Owner | Approximate effort | Primary files | Deliverable |
| --- | ---: | --- | --- |
| Student 1: directory and system integration | 35% | `coherence/directory.cc`; directory-related state helpers if needed | One transaction per line, MSI metadata, recalls/invalidations/acks, conflict queues, LLC interaction, dirty data and atomic grant flow. Coordinates end-to-end coherence tests. |
| Student 2: private cache and ordering | 35% | `coherence/cache.cc`, `memory/ordering.cc`; cache/order state helpers if needed | Private MSI controller, victim retention, request completion, SC/TSO ordering decisions, store buffer forwarding/fences/exchanges. |
| Student 3: network, DRAM policy and counters | 30% | `noc/routing.cc`, `memory/scheduling.cc`, `statistics.cc` | XY/YX and two arbiters, FCFS and custom scheduler, event counting, occupancy and stall sampling. |

Student 1 and Student 2 should review each other's state transitions before implementation: neither controller can be correct alone. Student 3 reviews packet/counter acceptance points with both, because the counter tests compare against the framework's independent observer. No one should edit `framework/pa4/*` to make a test pass. The supplied LLC, DRAM model, adapter, queues, capacities, and timing stay fixed.

### Why this is a 35/35/30 split

The percentages are a planning target, not a measurement or a division of marks. Directory work is one file but carries the most protocol state and dirty-data risk. Private-cache work is another substantial state machine; pairing it with the bounded TSO decision function makes Student 2's share comparable. Student 3 has several smaller, independent policy interfaces plus the cross-cutting counter implementation and performance comparisons. Do not split GetM, recalls, or dirty eviction between additional owners merely to make file counts match: Student 1 owns the home side and Student 2 owns the private-cache side of each complete transaction.

The main balance risk is integration time. If directory conflicts/LLC interactions take longer than planned, Student 3 should take ownership of the directory-focused test scenarios and counter audit while Student 1 retains the protocol code. If counter sampling or NoC policy testing takes longer, Student 1 can take the end-to-end coherence regression suite and Student 2 can take the ordering regression suite, leaving Student 3 focused on policy and measurement. Reassign test and review work first; change source ownership only by explicit team agreement. Review actual effort after the first working GetS/GetM path rather than treating 35/35/30 as a guarantee.

## 2. Shared contracts to agree first

### Addressing and data

- A CPU address is an aligned 32-bit **byte address**. A line address is 64-byte aligned. Word index is `(address % 64) / 4`. `slot(address)` selects the private direct-mapped entry.
- Home and controller mapping use the aligned line address. Core tiles come first, then home/LLC-bank tiles, then controller tiles. Default topology has four cores, four homes, and one controller in a configurable 3 by 3 mesh.
- Directory metadata outlives LLC data eviction; the LLC is non-inclusive. A private M owner can hold data newer than the LLC and DRAM. Directory logic must recall that data before using an LLC copy.
- Each CPU core accepts one local cache access at a time; each home permits one active transaction per line. Conflicting requests queue up to `router_depth` and cannot disappear on retry.

### Message and transaction identity

- `MemoryRequest.id` identifies a cache acquisition or eviction for the run. `Packet.id` identifies one transport packet. Preserve transaction ID, requester, and line in replies. The `requester` is always the original requesting core, even when an owner answers a recall.
- GetS/GetM acquire permission; PutS/PutM release a victim; RecallS/RecallI and Inv revoke or downgrade; Ack confirms; DataS/DataM grant; GrantAck closes the grant; PutAck releases the victim. DataS/DataM and required Acks carry the documented payload size.
- A line's active directory transaction continues through GrantAck. Exclusive permission is granted only after required invalidation acknowledgements. The private cache applies its pending access before sending GrantAck. XCHG reads the old word and installs the new word indivisibly after M permission.
- A pending PutM must retain data until PutAck; a recall during that phase returns retained victim data. A stale queued eviction must never overwrite data from a newer owner.

### Cycle and backpressure rules

- `begin()` copies `current` into `next`. Callbacks select work from `current` but mutate `next`; the next callback in a cycle must see prior changes to `next`. Incoming cache messages precede local accesses. New directory transactions first advance on the next cycle.
- A callback returning `false` consumes nothing and leaves queues, counters, state, and outgoing messages unchanged. Preflight all needed `can_send` capacities, grouped by message class, before consuming an input. `send` may still fail and must be checked. A blocked transaction remains in its current phase.
- Network routing order is global per run. The return from arbitration is a router **input ID**, not a candidate-vector position. Output links, injection queues, router buffers, response channels, and LLC/DRAM queues are finite. Avoid protocol deadlock by keeping response traffic serviceable.

## 3. Student 1: home directory and LLC integration

**Own:** `handle_directory_message` and `advance_directory_transaction` in `coherence/directory.cc` (PA4-COH-03/04). Add private helper functions or transient fields only when needed; preserve public signatures, capacities, and timing.

| Work package | Required behavior | Evidence to produce |
| --- | --- | --- |
| MSI metadata | Track I/S/M, owner, sharers, and one active transaction per line; queue conflicting requests up to the stated capacity. | Read sharing, two writers to one line, queue-full retry. |
| Acquisition | GetS can read the current owner or LLC; GetM invalidates sharers, recalls an owner as needed, then sends DataM. Keep the transaction through GrantAck. | Remote read of M line; many readers then writer; no M plus other readable copy. |
| Release | PutS/PutM and PutAck; retain dirty data through completion; prevent an old queued PutM from replacing newer data. | Dirty eviction; recall during PutM; stale eviction ordering. |
| LLC | Use `llc_request` and `llc_reply` once per phase. Dirty data updates the LLC via full-line write; never read backing memory directly. | Forced LLC eviction, dirty owner newer than LLC, completion without duplicated requests. |
| Progress and accounting | Service active transactions without consuming on blocked output; classify accepted requests, starts/completions, recalls, invalidations, ownership grants, acknowledgements, latency. | Directory start = complete + active; traffic counters agree with messages. |

**Review handoff:** publish a short state table for each transaction phase: accepted message, required permission/data, outgoing packet or LLC operation, expected reply, and next phase. Have Student 2 review it before either controller lands. Pair on the first GetM test and on dirty owner recall. The directory owner is the integration reviewer for the complete GetS/GetM cycle.

## 4. Student 2: private cache and memory ordering

**Own:** `accept_cache_request` and `handle_cache_message` in `coherence/cache.cc` (PA4-COH-01/02), and `decide_memory_action` in `memory/ordering.cc` (PA4-ORDER-01).

### Private cache

| Work package | Required behavior | Evidence to produce |
| --- | --- | --- |
| Local hits/misses | Classify load/store/XCHG at acceptance; serve a readable load and an M store/exchange locally; GetS/GetM or upgrade for missing permission. | Same-line hits, S-to-M upgrade, atomic old value. |
| Miss and grant path | Hold one pending access, install DataS/DataM, apply the pending operation, complete it once, then send GrantAck. | No duplicate result under blocked reply path. |
| Snoop path | Process Inv and RecallS/RecallI even while the core is stalled; acknowledge with correct data and update `next` before a same-cycle local access. | Writer after readers; recalled M data; incoming Inv before access. |
| Victims | Send PutS/PutM, hold M victim bytes until PutAck, and answer a recall of retained victim data. | Dirty conflict eviction; recall during pending PutM. |
| Counters | Emit accepted-access and permission classification once; emit invalidation, Ack, dirty eviction/writeback events at the exact acceptance point. | `docs/COUNTERS.md` event totals against trace and observer. |

### SC and TSO

The framework retains SC and calls `decide_memory_action` only in TSO mode. Implement decisions over the bounded per-core FIFO: enqueue non-speculative stores, drain them in order, forward a load from the **youngest older store to the same word**, and let a load to another word bypass buffered stores without reordering loads. Stall a new store when full. FENCE waits for older accesses and an empty store buffer before younger memory operations proceed. XCHG observes the same barrier and remains indivisible. Halted cores continue to service coherence and drain stores.

**Evidence:** same-word forwarding, different-word bypass, full-buffer stall, FENCE drain, SC and TSO store-buffer litmus with x/y on separate lines, and atomic lock counter. The both-zero outcome is forbidden under SC and after FENCE; it is allowed but not required on any one TSO run. Check that the result is architectural, not just that counters balance.

**Review handoff:** publish a cache state table including `I/S/M`, pending acquisition/upgrade, retained victim, and expected packet. Student 1 reviews every point at which an Ack or GrantAck is sent. Student 3 reviews the store-buffer and cache accepted-event counting points.

## 5. Student 3: mesh, scheduling, and measurements

**Own:** `select_output_port`, `select_router_input` (PA4-NOC-01/02); `select_memory_request` (PA4-MC-01); `count_event`, `sample_occupancy_and_stalls` (PA4-COUNT-01 plus sampling).

### Mesh routing and arbitration

Implement XY and YX using `current_router` and `destination_router`. Return Local only at destination and never route through an unconnected port. For round-robin, compare eligible input IDs cyclically from `round_robin_cursor` modulo 15; with nonempty candidates choose one. Define one custom policy and its deterministic tie breaker. The framework already handles timed links, packet sizes, ready times, bandwidth, and buffer eligibility; the policy must choose among eligible candidates without bypassing those limits. Test identical traffic under both route orders/arbiters, unloaded increasing-distance latency, contention, and full downstream buffers. An unloaded longer path must take more time when link timings match.

### Memory scheduler

FCFS selects the oldest eligible request. Define a custom policy that uses only supplied eligible indices, a deterministic tie breaker, and a starvation bound. Do not edit the DRAM model or issue commands directly. Compare FCFS/custom with identical configurations on same-row, alternating-row, independent-bank, and mixed read/write traffic. Record completion cycles, latency, row hit/miss/conflict classification, and maximum per-core waiting time.

### Counters and sampling

`count_event` stores supplied events as `component + "." + event`, adds ordinary counts, and takes a maximum for names ending `_max`. The framework emits transport, LLC, DRAM, and order events; do not duplicate them in controllers. Student 1/2 emit the coherence classifications specified in `docs/COUNTERS.md`. At each cycle start, sample `current` queue lengths for every router input queue, router total, controller queue, LLC queue, and store buffer. Count buffer-full, fence-wait, and ordering stalls at most once per core per cycle per category. Never copy from or change `observer`; it is the independent cross-check.

**Evidence:** reset clears all counters; injected = delivered + in-network; directory starts = completions + active; stores enqueued = drained + buffered; memory accepted = completed + outstanding. At drain the in-flight terms are zero. Row hit + miss + conflict = service starts. Check values against the independent observer and an architectural trace.

## 6. Integration order and review gates

| Gate | Team action | Exit condition |
| --- | --- | --- |
| 0. Migrated CPU | Review `PA3_MIGRATION.md`; compile; run `make framework-test` and the arithmetic CPU warm-up. | Register 3 is 12 on cores 0 and 1. PA4 TODO tests may still fail. |
| 1. Protocol contract | Students 1/2 agree on phase tables, packet identity, retained victims, and queue-full behavior. Student 3 confirms message class and counter points. | Both controllers can be implemented without incompatible assumptions. |
| 2. Isolated components | Student 3 checks NoC and scheduler; Students 1/2 test local transitions and retry behavior. | No new packet or counter on a rejected callback; finite capacities respected. |
| 3. Shared-memory path | Run private-line, sharing, modified-owner read, upgrade, competing writers, dirty eviction, false sharing, and atomic tests. | Architectural values and MSI exclusivity hold; no deadlock. |
| 4. Ordering and statistics | Run SC/TSO litmus, forwarding, FENCE, lock counter, and counter invariants. | Ordering and counter checks match independent observations. |
| 5. Final review | Run full `make test`, inspect warnings and Git diff, and have each member explain one complete transaction. | All public tests pass, previous submissions unchanged, contributions attributable in Git. |

The public test runner in `tests/run_tests.py` covers sharing, dirty eviction, LLC eviction, false sharing, SC/TSO litmus, forwarding, full buffers, atomics, reduced queue capacities, and YX/custom-policy combinations. `tests/framework.py` checks the supplied framework. Add focused tests where a state transition or starvation case is not covered, but keep generated outputs out of the submission.

## 7. Calendar and submission boundary

- **21–22 October:** confirm migrated CPU and shared protocol/state tables; each student establishes focused test inputs for their owner functions.
- **23–25 October:** implement isolated owner work and review cross-controller message formats daily.
- **26 October:** integrate coherence, ordering, NoC, scheduler, and counters; run reduced-capacity and dirty-owner cases.
- **27 October:** run the full suite, fix regressions, inspect Git attribution, and practice viva traces.
- **28 October:** final source review, commit and push by the announced deadline. Recheck the course deadline announcement.

The assignment requests **source code only** inside `submission4/`: no reports, plots, logs, generated counter files, or reproduction documents as submission artifacts. These two internal planning guides are for team understanding; remove them from a source-only hand-in if the submission mechanism treats every file under `submission4/` as submitted. Keep previous submissions unchanged.