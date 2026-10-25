#!/usr/bin/env python3
"""Exhaustively enumerate small two-core request-timing interleavings."""
import csv
import itertools
import json
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PA4 = pathlib.Path(os.environ.get("PA4_EXECUTABLE", ROOT / "pa4"))
SLOTS = tuple(itertools.combinations(range(4), 2))

SCENARIOS = (
    (
        "dirty_eviction_recall",
        (("STORE", 0, 101), ("STORE", 64, 202)),
        (("LOAD", 0, None), ("STORE", 0, 303)),
        "\ncache_lines 1\nrouter_depth 1\ninjection_depth 1\n",
    ),
    (
        "shared_upgrade_reader",
        (("LOAD", 0, None), ("STORE", 0, 404)),
        (("LOAD", 0, None), ("LOAD", 0, None)),
        "",
    ),
    (
        "competing_writers",
        (("STORE", 128, 505), ("LOAD", 128, None)),
        (("STORE", 128, 606), ("LOAD", 128, None)),
        "\ninjection_depth 1\n",
    ),
    (
        "false_sharing_adjacent_words",
        (("STORE", 0, 707), ("LOAD", 4, None)),
        (("STORE", 4, 808), ("LOAD", 0, None)),
        "\ninjection_depth 1\n",
    ),
)


def write_operation(core, operation, address, value):
    suffix = "" if value is None else f" {value}"
    return f"{core} {operation} {address}{suffix}"


def check_sequential_order(trace_path, input_by_core, results_path, state_path):
    current = {}
    next_operation = {core: 0 for core in input_by_core}
    rows = list(csv.DictReader(open(results_path, newline="")))
    values = {int(row["id"]): int(row["value"]) for row in rows}

    for line in trace_path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[1] == "memory_event":
            core = int(fields[2])
            request_id = int(fields[3])
            operation = fields[4].upper()
            address, written, returned = map(int, fields[5:8])
            sequence = input_by_core[core]
            position = next_operation[core]
            assert position < len(sequence), ("unexpected operation", core, operation)
            expected_operation, expected_address, expected_written, expected_id = sequence[position]
            assert (operation, address, written, request_id) == (
                expected_operation, expected_address, expected_written, expected_id
            ), ("per-core order mismatch", core, position, fields)
            next_operation[core] += 1

            old_value = current.get(address, 0)
            if operation in ("LOAD", "XCHG"):
                assert returned == old_value, (
                    "SC read mismatch", core, operation, address, returned, old_value
                )
            if operation in ("STORE", "XCHG"):
                current[address] = written

    assert all(next_operation[core] == len(sequence)
               for core, sequence in input_by_core.items())
    for sequence in input_by_core.values():
        for operation, _, _, request_id in sequence:
            if operation == "STORE":
                assert values[request_id] == 0, ("store returned nonzero", request_id)

    state = json.loads(pathlib.Path(state_path).read_text())
    for address, value in current.items():
        assert state["memory"].get(str(address), 0) == value, (
            "final memory mismatch", address, value
        )


def run_one(directory, scenario, slot_pair_0, slot_pair_1):
    name, operations_0, operations_1, overrides = scenario
    schedule = {0: slot_pair_0, 1: slot_pair_1}
    operations = {0: operations_0, 1: operations_1}
    records = []
    per_core = {0: [], 1: []}
    for core in (0, 1):
        for sequence_number, (operation, address, value) in enumerate(operations[core]):
            records.append(
                (schedule[core][sequence_number] * 10, core, sequence_number,
                 operation, address, value)
            )

    workload = directory / "permutation.trace"
    lines = []
    for request_id, record in enumerate(
        sorted(records, key=lambda item: (item[0], item[1])), 1
    ):
        cycle, core, sequence_number, operation, address, value = record
        lines.append(f"{cycle} {write_operation(core, operation, address, value)}\n")
        per_core[core].append((sequence_number, operation, address, value, request_id))
    per_core = {
        core: [(operation, address, 0 if value is None else value, request_id)
               for _, operation, address, value, request_id in
               sorted(sequence, key=lambda item: item[0])]
        for core, sequence in per_core.items()
    }
    workload.write_text("".join(lines))
    config = directory / "permutation.cfg"
    config.write_text(
        (ROOT / "configs/default.cfg").read_text()
        .replace("memory_model SC", "memory_model SC") + overrides
    )
    output = directory / "permutation"
    result = subprocess.run(
        [str(PA4), "trace", str(config), str(workload), str(output), "12000"],
        cwd=ROOT, text=True, capture_output=True,
    )
    if result.returncode:
        raise AssertionError(
            f"{name} slots={slot_pair_0}/{slot_pair_1}: {result.stderr.strip()}"
        )
    check_sequential_order(
        pathlib.Path(f"{output}.trace"), per_core,
        f"{output}.results.csv", f"{output}.state.json"
    )


def main():
    count = 0
    with tempfile.TemporaryDirectory(prefix="pa4-protocol-permutations-") as temporary:
        directory = pathlib.Path(temporary)
        for scenario in SCENARIOS:
            for slots_0, slots_1 in itertools.product(SLOTS, repeat=2):
                run_one(directory, scenario, slots_0, slots_1)
                count += 1
            print("PASS exhaustive timing interleavings:", scenario[0], len(SLOTS) ** 2)
    print(f"PASS {count} bounded protocol/permutation simulations")


if __name__ == "__main__":
    main()
