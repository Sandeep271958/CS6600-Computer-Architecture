#!/usr/bin/env python3
"""Build an isolated gcov-instrumented PA4 and measure source-line coverage."""
import os
import pathlib
import re
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def sources():
    pa3 = set(ROOT.glob("*.cc"))
    pa3.update((ROOT / "framework").glob("*.cc"))
    pa3.update((ROOT / "design").glob("*.cc"))
    pa3.update((ROOT / "execution_unit").glob("*.cc"))
    pa3.update((ROOT / "predictors").glob("*.cc"))
    pa3.update((ROOT / "cache").glob("*.cc"))
    pa3.discard(ROOT / "framework/main.cc")
    pa3.discard(ROOT / "statistics.cc")

    pa4 = set((ROOT / "framework/pa4").glob("*.cc"))
    pa4.update((ROOT / "noc").glob("*.cc"))
    pa4.update((ROOT / "coherence").glob("*.cc"))
    pa4.update((ROOT / "memory").glob("*.cc"))
    pa4.discard(ROOT / "framework/pa4/main.cc")
    pa4.discard(ROOT / "framework/pa4/noc_main.cc")
    pa4.add(ROOT / "statistics.cc")

    common = sorted(pa3 | pa4)
    return common, ROOT / "framework/pa4/main.cc", ROOT / "framework/pa4/noc_main.cc"


def compile_binary(compiler, object_paths, main_source, output):
    main_object = output.parent / (main_source.stem + ".o")
    subprocess.run(
        [compiler, "-std=c++17", "-O0", "-g", "--coverage", "-I.",
         "-Iinclude", "-c", str(main_source), "-o", str(main_object)],
        cwd=ROOT, check=True,
    )
    subprocess.run(
        [compiler, "--coverage", *map(str, object_paths), str(main_object),
         "-o", str(output)],
        cwd=ROOT, check=True,
    )


def compile_objects(compiler, sources_list, directory):
    objects = []
    for source in sources_list:
        object_path = directory / source.relative_to(ROOT).with_suffix(".o")
        object_path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [compiler, "-std=c++17", "-O0", "-g", "--coverage", "-I.",
             "-Iinclude", "-c", str(source), "-o", str(object_path)],
            cwd=ROOT, check=True,
        )
        objects.append(object_path)
    return objects


def coverage_rows(gcov, sources_list, object_paths, output_dir):
    rows = []
    gcov_dir = output_dir / "gcov"
    gcov_dir.mkdir()
    for source, object_path in zip(sources_list, object_paths):
        result = subprocess.run(
            [gcov, "-b", "-c", "-o", str(object_path), str(source)],
            cwd=gcov_dir, check=True, capture_output=True, text=True,
        )
        lines = result.stdout.splitlines()
        source_marker = str(source)
        summary = None
        for index, line in enumerate(lines):
            if line.startswith("File '") and source_marker in line:
                summary = next(
                    (candidate for candidate in lines[index + 1:]
                     if candidate.startswith("Lines executed:")),
                    None,
                )
                break
        if summary:
            gcov_path = gcov_dir / f"{source.name}.gcov"
            uncovered = []
            if gcov_path.exists():
                for line in gcov_path.read_text().splitlines():
                    match = re.match(r"\s*(?:#####|=====):\s*(\d+):", line)
                    if match:
                        uncovered.append(int(match.group(1)))
            rows.append((source.relative_to(ROOT), summary, uncovered))
    return rows


def main():
    compiler = os.environ.get("CXX", "g++")
    gcov = shutil.which(os.environ.get("GCOV", "gcov"))
    if not gcov:
        raise SystemExit("gcov was not found; install GCC's gcov tool")

    common, main_source, noc_main_source = sources()
    with tempfile.TemporaryDirectory(prefix="pa4-gcov-") as temporary:
        directory = pathlib.Path(temporary)
        object_paths = compile_objects(compiler, common, directory / "objects")
        pa4 = directory / "pa4"
        pa4_noc = directory / "pa4-noc"
        compile_binary(compiler, object_paths, main_source, pa4)
        compile_binary(compiler, object_paths, noc_main_source, pa4_noc)

        environment = os.environ.copy()
        environment["PA4_EXECUTABLE"] = str(pa4)
        environment["PA4_NOC_EXECUTABLE"] = str(pa4_noc)
        for test in (
            "tests/run_tests.py",
            "tests/extended_integration.py",
            "tests/protocol_permutations.py",
        ):
            subprocess.run(["python3", test], cwd=ROOT, env=environment, check=True)

        policy_test = directory / "extended-policy-tests"
        policy_objects = [
            path for path in object_paths
            if path.name in ("routing.o", "ordering.o", "scheduling.o")
        ]
        policy_main = directory / "extended_policy_tests.o"
        subprocess.run(
            [compiler, "-std=c++17", "-O0", "-g", "--coverage", "-I.",
             "-Iinclude", "-c", "tests/extended_policy_tests.cc",
             "-o", str(policy_main)],
            cwd=ROOT, check=True,
        )
        subprocess.run(
            [compiler, "--coverage", *map(str, policy_objects), str(policy_main),
             "-o", str(policy_test)],
            cwd=ROOT, check=True,
        )
        subprocess.run([str(policy_test)], check=True)

        rows = coverage_rows(gcov, common, object_paths, directory)
        print("\nPA4 production source-line coverage:")
        for path, summary, uncovered in rows:
            if path.parts[0] in ("coherence", "memory", "noc") or path.name == "statistics.cc":
                print(f"{path}: {summary}")
                if uncovered:
                    print("  uncovered source lines:", ", ".join(map(str, uncovered)))


if __name__ == "__main__":
    main()
