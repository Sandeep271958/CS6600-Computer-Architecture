import argparse
import json
from pathlib import Path


BLOCK_COUNT = 640
OUTER_ROUNDS = 3


def build_workload():
    instructions = []
    labels = {}

    def emit(opcode, *operands):
        instructions.append((opcode, operands))

    def mark(label):
        labels[label] = len(instructions) * 4

    emit("ADDI", 1, 0, 0, 0)
    emit("ADDI", 2, 0, 0, 0)
    emit("ADDI", 4, 0, 0, 1)
    emit("ADDI", 7, 0, 0, OUTER_ROUNDS)
    mark("outer")

    total_trip_count = 0
    state = 0x6600
    for block in range(BLOCK_COUNT):
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        trip_count = 1 + state % 31
        total_trip_count += trip_count

        loop_label = f"loop_{block}"
        done_label = f"done_{block}"
        after_count_label = f"after_count_{block}"
        emit("ADDI", 3, 0, 0, trip_count)
        mark(loop_label)
        emit("ADDI", 1, 1, 0, 1)
        emit("ADDI", 3, 3, 0, -1)

        if block % 2 == 0:
            emit("BNE", 0, 3, 0, loop_label)
        else:
            emit("BEQ", 0, 3, 0, done_label)
            emit("J", 0, 0, 0, loop_label)
            mark(done_label)

        emit("BEQ", 0, 7, 4, after_count_label)
        emit("ADDI", 2, 2, 0, 1)
        mark(after_count_label)

    emit("ADDI", 7, 7, 0, -1)
    emit("BNE", 0, 7, 0, "outer")
    emit("HALT", 0, 0, 0, 0)

    lines = ["PIPEISA 1", "ENTRY 0x00000000", ""]
    for index, (opcode, operands) in enumerate(instructions):
        address = index * 4
        resolved = [labels.get(value, value) if isinstance(value, str) else value
                    for value in operands]
        if opcode in {"BEQ", "BNE", "J"}:
            resolved[-1] = f"0x{resolved[-1]:08x}"
        lines.append(f"INST 0x{address:08x} {opcode} " + " ".join(map(str, resolved)))
    lines.extend(["", "END", ""])

    expected_state = [
        "BEGIN STATE",
        "x0 0",
        f"x1 {total_trip_count * OUTER_ROUNDS}",
        f"x2 {BLOCK_COUNT * (OUTER_ROUNDS - 1)}",
        "x3 0",
        "x4 1",
        "x5 0",
        "x6 0",
        "x7 0",
        "status HALTED",
        "END STATE",
        "",
    ]
    return "\n".join(lines), "\n".join(expected_state), total_trip_count


def main():
    parser = argparse.ArgumentParser(description="Generate a large branch-predictor stress workload.")
    parser.add_argument("--output", type=Path, default=Path("tests/branch_predictor_stress.pisa"))
    parser.add_argument("--expected", type=Path, default=Path("tests/branch_predictor_stress.expected.txt"))
    parser.add_argument("--config", type=Path, default=Path("tests/branch_predictor_stress.json"))
    parser.add_argument("--base-config", type=Path, default=Path("default_config.json"))
    args = parser.parse_args()

    program, expected, total_trip_count = build_workload()
    args.output.write_text(program, encoding="ascii")
    args.expected.write_text(expected, encoding="ascii")
    config = json.loads(args.base_config.read_text(encoding="utf-8"))
    instruction_count = program.count("INST ")
    required_image_bytes = ((instruction_count * 4 + 4095) // 4096) * 4096
    config["InstructionMemory"]["image_bytes"] = max(
        config["InstructionMemory"]["image_bytes"], required_image_bytes
    )
    args.config.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {args.output} ({program.count('INST ')} static instructions)")
    print(f"Wrote {args.expected}")
    print(f"Wrote {args.config} (instruction image {config['InstructionMemory']['image_bytes']} bytes)")
    print(f"Static branch PCs: {BLOCK_COUNT * 2 + BLOCK_COUNT // 2 + 1}")
    print(f"Dynamic loop iterations: {total_trip_count * OUTER_ROUNDS}")


if __name__ == "__main__":
    main()