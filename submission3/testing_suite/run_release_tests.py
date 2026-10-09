import os
import re
import subprocess
import csv

def run_release_tests():
    print("Running Release Tests with Metrics...")
    
    results = []
    total_tests = 10
    success_count = 0

    for i in range(1, total_tests + 1):
        pisa_file = f"tests/release_tests/{i}.pisa"
        json_file = f"tests/release_tests/{i}.json"
        expected_file = f"tests/release_tests/{i}.expected.txt"

        if not os.path.exists(pisa_file):
            print(f"Test {i}: MISSING")
            continue

        if os.name == "nt" and os.path.exists(".\\pipesim.exe"):
            exe = ".\\pipesim.exe"
        else:
            exe = "./pipesim"
        proc = subprocess.run(
            [exe, pisa_file, json_file, "0"],
            input="n 500000\np\ns\nq\n",
            text=True, capture_output=True, cwd="."
        )

        output = proc.stdout

        # Pass/fail check
        state_lines = []
        in_state = False
        for line in output.split('\n'):
            if "BEGIN STATE" in line:
                in_state = True
                state_lines.append("BEGIN STATE")
            elif "END STATE" in line and in_state:
                state_lines.append("END STATE")
                break
            elif in_state:
                state_lines.append(line.strip())

        actual_state = '\n'.join(state_lines)
        with open(expected_file, 'r') as f:
            expected_state = f.read()

        passed = actual_state.strip() == expected_state.strip()
        if passed:
            success_count += 1
            status = "PASS"
        else:
            status = "FAIL"
        print(f"Test {i}: {status}")

        # Extract metrics
        def extract(key):
            m = re.search(fr'{key}\s+(\d+)', output, re.MULTILINE)
            return int(m.group(1)) if m else 0

        cycles      = extract(r'cycles')
        retired     = extract(r'retired')
        d_stalls    = extract(r'data_stalls')
        c_stalls    = extract(r'control_stalls')
        b_found     = extract(r'predictor0\.branches_found')
        b_not_found = extract(r'predictor0\.branches_not_found')
        correct     = extract(r'predictor0\.correct_predictions')
        dir_mis     = extract(r'predictor0\.direction_mispredictions')
        tgt_mis     = extract(r'predictor0\.target_mispredictions')
        false_pred  = extract(r'predictor0\.false_branch_predictions')
        missed      = extract(r'predictor0\.missed_branches')

        total_preds = correct + dir_mis + tgt_mis
        accuracy    = f"{(correct / total_preds * 100):.2f}" if total_preds > 0 else "N/A"
        cpi         = f"{(cycles / retired):.2f}" if retired > 0 else "N/A"

        results.append({
            "Test": i,
            "Status": status,
            "Cycles": cycles,
            "Retired": retired,
            "CPI": cpi,
            "Data_Stalls": d_stalls,
            "Control_Stalls": c_stalls,
            "B_Found": b_found,
            "B_Not_Found": b_not_found,
            "Correct": correct,
            "Dir_Mispred": dir_mis,
            "Tgt_Mispred": tgt_mis,
            "False_Pred": false_pred,
            "Missed": missed,
            "Accuracy": accuracy,
        })

    # Write CSV
    out_path = "untracked_tests/release_test_results.csv"
    os.makedirs("untracked_tests", exist_ok=True)
    fieldnames = list(results[0].keys()) if results else []
    with open(out_path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(results)

    print(f"\nScore: {success_count}/{total_tests}")
    print(f"Results saved to: {out_path}")

if __name__ == "__main__":
    run_release_tests()
