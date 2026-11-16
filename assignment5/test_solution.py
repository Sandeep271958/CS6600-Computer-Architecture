import subprocess
import glob
import os
import sys

def run_tests(bin_path="./outbin"):
    if not os.path.exists(bin_path):
        print(f"Error: binary '{bin_path}' not found. Please compile your CUDA code first:")
        print(f"  nvcc -O2 -std=c++17 starter.cu -o outbin")
        return False

    test_inputs = sorted(glob.glob("public_tests/input_*.txt"))
    if not test_inputs:
        # Check relative path
        test_inputs = sorted(glob.glob("assignment5/public_tests/input_*.txt"))
    
    all_passed = True
    print(f"Running tests with binary '{bin_path}'...\n" + "="*50)

    for in_file in test_inputs:
        base_name = os.path.basename(in_file).replace("input_", "expected_")
        exp_file = os.path.join(os.path.dirname(in_file), base_name)
        out_file = "/tmp/test_out.txt"

        if not os.path.exists(exp_file):
            print(f"[SKIP] Expected file not found for {in_file}")
            continue

        cmd = [bin_path, in_file, out_file]
        try:
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            if res.returncode != 0:
                print(f"[FAIL] {in_file} -> non-zero return code {res.returncode}")
                if res.stderr:
                    print(res.stderr)
                all_passed = False
                continue

            with open(out_file, "r") as f_out, open(exp_file, "r") as f_exp:
                out_content = f_out.read().strip()
                exp_content = f_exp.read().strip()

            if out_content == exp_content:
                print(f"[PASS] {os.path.basename(in_file)}")
            else:
                print(f"[FAIL] {os.path.basename(in_file)}: Output mismatch!")
                print("--- Expected ---")
                print(exp_content)
                print("--- Actual ---")
                print(out_content)
                all_passed = False
        except Exception as e:
            print(f"[ERROR] {in_file}: {e}")
            all_passed = False

    print("="*50)
    if all_passed:
        print("ALL PUBLIC TESTS PASSED!")
    else:
        print("SOME TESTS FAILED.")
    return all_passed

if __name__ == "__main__":
    binary = sys.argv[1] if len(sys.argv) > 1 else "./outbin"
    success = run_tests(binary)
    sys.exit(0 if success else 1)

