import os
import subprocess
import re
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SUBMISSION_DIR = os.path.dirname(SCRIPT_DIR)

RESULTS_FILE = os.path.join(SCRIPT_DIR, "../untracked_tests/search_results_complex.csv")
HEADER_FILE = os.path.join(SUBMISSION_DIR, "predictors/predictor_one.h")
TMP_HEADER = os.path.join(SUBMISSION_DIR, "predictors/predictor_one.h.tmp")

HISTORY_CONFIGS = [
    "0, 6, 11, 20, 37, 68, 130",
    "0, 4, 8, 16, 32, 64, 128",
    "0, 8, 16, 32, 64, 100, 150",
    "0, 5, 10, 20, 40, 80, 130"
]

def restore_header():
    if os.path.exists(TMP_HEADER):
        import shutil
        shutil.move(TMP_HEADER, HEADER_FILE)

def setup():
    os.makedirs(os.path.dirname(RESULTS_FILE), exist_ok=True)
    with open(RESULTS_FILE, 'w') as f:
        f.write("Config,History_Lengths,B_Found,B_Not_Found,P_Taken,P_Not_Taken,Correct,Dir_Mispred,Tgt_Mispred,False_Pred,Missed,Accuracy,Total_Cycles,CPI\n")
    import shutil
    shutil.copy2(HEADER_FILE, TMP_HEADER)

def run_test(config_name, hist_arr):
    print(f"=========================================================")
    print(f"Running Configuration: {config_name}")
    
    with open(HEADER_FILE, 'r') as f:
        content = f.read()
    
    content = re.sub(r'static constexpr int kHistoryLengths\[7\] = .*?;', f'static constexpr int kHistoryLengths[7] = {{{hist_arr}}};', content)
    
    with open(HEADER_FILE, 'w') as f:
        f.write(content)

    print("Compiling...")
    subprocess.run(["make", "clean"], cwd=SUBMISSION_DIR, stdout=subprocess.DEVNULL)
    build_result = subprocess.run(["make", "pipesim"], cwd=SUBMISSION_DIR, stdout=subprocess.DEVNULL)
    if build_result.returncode != 0:
        print(f"Build failed for config {config_name}")
        return

    print("Running test...")
    with tempfile.NamedTemporaryFile(mode='w+', delete=False) as tmp_out:
        tmp_name = tmp_out.name
    
    try:
        # Run pipesim with 'n 200000', 'p', 's', 'q'
        proc = subprocess.run(["./pipesim", "untracked_tests/complex_workload.pisa", "default_config.json", "0"], 
                              input="n 200000\np\ns\nq\n", text=True, cwd=SUBMISSION_DIR, capture_output=True)
        
        output = proc.stdout
        
        def extract(key):
            m = re.search(fr'{key}\s+(\d+)', output, re.MULTILINE)
            return int(m.group(1)) if m else 0

        b_found = extract(r'predictor0\.branches_found')
        b_not_found = extract(r'predictor0\.branches_not_found')
        p_taken = extract(r'predictor0\.predicted_taken')
        p_not_taken = extract(r'predictor0\.predicted_not_taken')
        correct = extract(r'predictor0\.correct_predictions')
        dir_mispred = extract(r'predictor0\.direction_mispredictions')
        tgt_mispred = extract(r'predictor0\.target_mispredictions')
        false_pred = extract(r'predictor0\.false_branch_predictions')
        missed = extract(r'predictor0\.missed_branches')
        
        cycles = extract(r'^cycles')
        retired = extract(r'^retired')
        cpi = f"{(cycles / retired):.2f}" if retired > 0 else "0.00"
        
        total_preds = correct + dir_mispred + tgt_mispred
        accuracy = f"{(correct / total_preds * 100):.2f}" if total_preds > 0 else "0.00"
        
        print(f"Accuracy: {accuracy}%, CPI: {cpi}")
        
        with open(RESULTS_FILE, 'a') as f:
            f.write(f'{config_name},"{hist_arr}",{b_found},{b_not_found},{p_taken},{p_not_taken},{correct},{dir_mispred},{tgt_mispred},{false_pred},{missed},{accuracy},{cycles},{cpi}\n')
            
    finally:
        if os.path.exists(tmp_name):
            os.remove(tmp_name)

try:
    setup()
    config_count = 1
    for hist in HISTORY_CONFIGS:
        run_test(f"Config_{config_count}", hist)
        config_count += 1
finally:
    restore_header()
    print(f"=========================================================")
    print(f"Search completed. Results saved to {RESULTS_FILE}")
