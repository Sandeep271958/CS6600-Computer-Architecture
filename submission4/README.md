# CS6600 Assignment 4 — Coherent Multicore System

This submission extends the migrated PA3 processor with a configurable
multicore memory system. A 2D mesh connects private MSI data caches,
home-directory/shared-LLC banks, and DRAM controllers. The implementation
includes XY/YX routing, round-robin/custom arbitration, directory coherence,
SC and TSO memory ordering, memory-controller scheduling, and event counters.
The migrated processor instruction-cache hierarchy remains active.

## Requirements

- C++17 compiler (GCC/G++ recommended)
- GNU Make
- Python 3
- GCC's `gcov` tool for the source-coverage run

No third-party C++ libraries are required. Run commands from this directory.

## Build and basic use

```sh
make -j4

# Coherence/memory-system trace
./pa4 trace configs/default.cfg workloads/sharing.trace sharing 5000

# CPU workload (requires the migrated PA3 core)
./pa4 cpu configs/default.cfg configs/pa3.json \
  workloads/arithmetic_programs.txt arithmetic 2000

# Standalone mesh traffic
./pa4-noc configs/default.cfg workloads/network.traffic network 2000
```

The output argument is a prefix; result CSV, state, counter, observer, and trace
files are written using that prefix. The parent directory must already exist.
Generated outputs are not source files and should not be committed.

## Tests

Run the supplied framework fixtures and public functional tests:

```sh
make framework-test
make test
```

Additional tests are kept separate from `make test` so the public target
remains unchanged:

```sh
# Routing, arbitration, TSO decision, and scheduler policy boundaries
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude \
  tests/extended_policy_tests.cc noc/routing.cc memory/ordering.cc \
  memory/scheduling.cc -o /tmp/pa4-extended-policy-tests
/tmp/pa4-extended-policy-tests

# Directed protocol and memory-order integration scenarios
python3 tests/extended_integration.py

# Bounded exhaustive request-timing permutations: 144 simulations
python3 tests/protocol_permutations.py

# Bounded internal message-order exploration through production MSI callbacks
python3 tests/protocol_order_explorer.py
```

The permutation suite exhausts the request-timing assignments defined by its
four two-core scenarios. The callback-level explorer enumerates enabled
queue-head message deliveries across senders (while retaining each sender's
FIFO order) for two fixed two-core, two-operation programs. It invokes the
production `coherence/cache.cc` and `coherence/directory.cc` callbacks, with
injection depth one, and checks MSI exclusivity, per-core completion order,
read values against an allowed sequential ordering, and final data. Its LLC
services requests immediately, and packet delivery, routing, router
arbitration, link timing, DRAM behavior, and other configurations are
abstracted away. This is exhaustive only within that finite callback-level
model; it is not a proof of all implementation executions or every legal
mesh packet schedule.

## Source-line coverage

Run the instrumented test build and gcov report:

```sh
python3 tests/coverage.py
```

This script builds isolated instrumented executables in a temporary directory,
then runs the public tests, extended integration tests, protocol permutations,
and policy tests against them. It reports executable-line coverage and
unexecuted source lines for the six PA4 policy files. The measured coverage
from the documented test set was 583/635 executable lines (91.81%) overall:

| PA4 implementation | Executable-line coverage |
| --- | ---: |
| `coherence/cache.cc` | 84.83% |
| `coherence/directory.cc` | 92.97% |
| `memory/ordering.cc` | 100.00% |
| `memory/scheduling.cc` | 95.45% |
| `noc/routing.cc` | 100.00% |
| `statistics.cc` | 100.00% |

Coverage is a test measurement, not proof of correctness or a branch-coverage
claim. The script requires GCC and `gcov`; it does not change the normal build
or leave coverage artifacts in the repository.

## Configuration and workload formats

The default system is a 3×3 mesh with four cores, four home/LLC banks, and one
memory controller. Configuration files select routing (`XY`/`YX`), arbitration
(`rr`/`custom`), memory model (`SC`/`TSO`), and scheduler (`fcfs`/`custom`),
along with mesh, queue, cache, LLC, and DRAM capacities/timing.

Trace records use one operation per line:

```text
earliest_cycle core LOAD address
earliest_cycle core STORE address value
earliest_cycle core XCHG address value
earliest_cycle core FENCE
```

Per-core request order is retained. Addresses are aligned 32-bit word byte
addresses. A fence has no address/value operands. CPU program lists contain one
program path per core; paths are interpreted relative to the working directory.

## Implementation and reference guides

- [Assignment brief](./Assignment_4.md)
- [Given starter instructions](./README_given.md)
- [Architectural and protocol guide](./pa4_architectural_guide.md)
- [Team workload guide](./PA4_TEAM_WORKLOAD_guide.md)
- [Interface contracts](./docs/INTERFACES.md)
- [Counter definitions](./docs/COUNTERS.md)
