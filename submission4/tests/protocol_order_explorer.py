#!/usr/bin/env python3
"""Compile and run the bounded explorer against the production MSI callbacks."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="pa4-protocol-order-") as temporary:
        executable = pathlib.Path(temporary) / "protocol-order-explorer"
        subprocess.run(
            [
                "g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic",
                "-I", str(ROOT / "include"),
                str(ROOT / "tests" / "protocol_order_explorer.cc"),
                str(ROOT / "coherence" / "cache.cc"),
                str(ROOT / "coherence" / "directory.cc"),
                "-o", str(executable),
            ],
            check=True,
            cwd=ROOT,
        )
        subprocess.run([str(executable)], check=True, cwd=ROOT)


if __name__ == "__main__":
    main()
