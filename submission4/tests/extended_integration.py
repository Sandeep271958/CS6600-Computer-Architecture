#!/usr/bin/env python3
"""Directed integration regressions not covered by tests/run_tests.py."""
import csv
import json
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PA4 = pathlib.Path(os.environ.get("PA4_EXECUTABLE", ROOT / "pa4"))


def run_case(directory, name, workload, model="SC", overrides="", max_cycles=12000):
    config = directory / f"{name}.cfg"
    config.write_text(
        (ROOT / "configs/default.cfg").read_text()
        .replace("memory_model SC", f"memory_model {model}")
        + overrides
    )
    trace = directory / f"{name}.trace"
    trace.write_text(workload)
    output = directory / name
    result = subprocess.run(
        [str(PA4), "trace", str(config), str(trace), str(output),
         str(max_cycles)],
        cwd=ROOT, text=True, capture_output=True,
    )
    if result.returncode:
        raise AssertionError(result.stderr.strip())
    values = {
        int(row["id"]): int(row["value"])
        for row in csv.DictReader(open(f"{output}.results.csv", newline=""))
    }
    state = json.loads(pathlib.Path(f"{output}.state.json").read_text())
    counters = json.loads(pathlib.Path(f"{output}.counters.json").read_text())
    observer = json.loads(pathlib.Path(f"{output}.observer.json").read_text())
    return values, state, counters, observer


def check_accounting(counters, observer):
    for component, event, observed in [
        ("noc", "packets_injected", "noc.injected"),
        ("noc", "packets_delivered", "noc.delivered"),
        ("directory", "transactions_started", "directory.started"),
        ("directory", "transactions_completed", "directory.completed"),
    ]:
        total = sum(
            value for key, value in counters.items()
            if key.startswith(component + ".") and key.count(".") == 2
            and key.endswith("." + event)
        )
        assert total == observer.get(observed, 0), (event, total, observer.get(observed, 0))
    assert observer["noc.injected"] == observer["noc.delivered"]
    assert observer["directory.started"] == observer["directory.completed"]


def main():
    cases = [
        (
            "retained_dirty_victim_recall",
            "0 0 STORE 0 111\n"
            "80 0 STORE 64 222\n"
            "80 1 LOAD 0\n"
            "160 1 STORE 0 333\n"
            "240 0 LOAD 64\n"
            "320 0 LOAD 0\n",
            "SC",
            "\ncache_lines 1\nrouter_depth 1\ninjection_depth 1\n",
            {3: 111, 5: 222, 6: 333},
        ),
        (
            "multiple_sharer_writer_handover",
            "0 0 STORE 128 51\n"
            "80 1 LOAD 128\n"
            "80 2 LOAD 128\n"
            "160 3 STORE 128 73\n"
            "240 1 LOAD 128\n"
            "320 2 LOAD 128\n",
            "SC",
            "",
            {2: 51, 3: 51, 5: 73, 6: 73},
        ),
        (
            "retained_dirty_victim_writer_race",
            "0 0 STORE 0 111\n"
            "80 0 STORE 64 222\n"
            "80 1 STORE 0 333\n"
            "240 1 LOAD 0\n"
            "320 0 LOAD 0\n",
            "SC",
            "\ncache_lines 1\nrouter_depth 1\ninjection_depth 1\n",
            {4: 333, 5: 333},
        ),
        (
            "upgrade_with_concurrent_reader",
            "0 0 LOAD 0\n"
            "80 1 LOAD 0\n"
            "160 0 STORE 0 707\n"
            "160 1 LOAD 0\n"
            "320 0 LOAD 0\n"
            "320 1 LOAD 0\n",
            "SC",
            "",
            {5: 707, 6: 707},
        ),
        (
            "tso_youngest_forward_fence_and_exchange",
            "0 0 STORE 0 11\n"
            "0 0 STORE 0 22\n"
            "0 0 LOAD 0\n"
            "0 0 STORE 4 44\n"
            "0 0 LOAD 8\n"
            "0 0 FENCE\n"
            "0 0 LOAD 4\n"
            "0 0 XCHG 0 33\n"
            "0 0 LOAD 0\n",
            "TSO",
            "\nstore_entries 8\n",
            {3: 22, 5: 0, 7: 44, 8: 22, 9: 33},
        ),
        (
            "last_valid_word_and_line_boundary",
            "0 0 STORE 65532 90210\n"
            "80 1 LOAD 65532\n"
            "160 1 STORE 64 123\n"
            "240 0 LOAD 64\n"
            "320 2 LOAD 60\n",
            "SC",
            "",
            {2: 90210, 4: 123, 5: 0},
        ),
    ]
    with tempfile.TemporaryDirectory(prefix="pa4-extended-") as temporary:
        directory = pathlib.Path(temporary)
        for name, workload, model, overrides, expected in cases:
            values, _, counters, observer = run_case(
                directory, name, workload, model, overrides)
            for request_id, value in expected.items():
                assert values.get(request_id) == value, (
                    name, request_id, values.get(request_id), value
                )
            check_accounting(counters, observer)
            if name.startswith("retained_dirty_victim"):
                recalls = sum(
                    value for key, value in counters.items()
                    if key.startswith("directory.") and key.endswith(".recalls_sent")
                )
                evictions = sum(
                    value for key, value in counters.items()
                    if key.startswith("cache.") and key.endswith(".dirty_evictions")
                )
                assert recalls > 0 and evictions > 0, (name, recalls, evictions)
            print("PASS", name)
    print(f"{len(cases)} integration scenarios passed")


if __name__ == "__main__":
    main()
