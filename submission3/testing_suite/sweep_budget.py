import re
import subprocess
import os
import shutil
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SUBMISSION_DIR = os.path.dirname(SCRIPT_DIR)
RESULTS_FILE = os.path.join(SUBMISSION_DIR, "untracked_tests/sweep_budget_results_ph.csv")
H_PATH = os.path.join(SUBMISSION_DIR, "predictors/predictor_one.h")
TMP_HEADER = os.path.join(SUBMISSION_DIR, "predictors/predictor_one.h.tmp")

CONFIGS = [
    {
        "name": "Low Budget (~30Kb)",
        "kBaseSize": 2048,
        "t_sizes": [256, 256, 256, 256, 128, 64],
        "kIndexWidths": [11, 8, 8, 8, 8, 7, 6],
        "kTagWidths": [0, 8, 8, 9, 9, 10, 10],
        "kHistoryLengths": [0, 4, 8, 16, 32, 64, 128],
        "kScNumTables": 2,
        "kScTableSize": 256,
        "kLoopTableSize": 32,
    },
    {
        "name": "Medium Budget (~60Kb)",
        "kBaseSize": 4096,
        "t_sizes": [512, 256, 256, 128, 128, 128],
        "kIndexWidths": [12, 9, 8, 8, 7, 7, 7],
        "kTagWidths": [0, 8, 8, 9, 10, 10, 11],
        "kHistoryLengths": [0, 5, 10, 20, 40, 80, 130],
        "kScNumTables": 4,
        "kScTableSize": 256,
        "kLoopTableSize": 64,
    },
    {
        "name": "High Budget (~85Kb) [Current]",
        "kBaseSize": 4096,
        "t_sizes": [512, 512, 512, 512, 256, 128],
        "kIndexWidths": [12, 9, 9, 9, 9, 8, 7],
        "kTagWidths": [0, 8, 8, 9, 10, 10, 11],
        "kHistoryLengths": [0, 5, 10, 20, 40, 80, 130],
        "kScNumTables": 4,
        "kScTableSize": 512,
        "kLoopTableSize": 64,
    },
    {
        "name": "Ultra Budget (~160Kb)",
        "kBaseSize": 8192,
        "t_sizes": [1024, 1024, 1024, 512, 512, 256],
        "kIndexWidths": [13, 10, 10, 10, 9, 9, 8],
        "kTagWidths": [0, 8, 9, 10, 11, 12, 13],
        "kHistoryLengths": [0, 8, 16, 32, 64, 128, 200],
        "kScNumTables": 6,
        "kScTableSize": 512,
        "kLoopTableSize": 128,
    }
]

def apply_config(cfg):
    with open(H_PATH, "r") as f:
        content = f.read()

    content = re.sub(r"kBaseSize = \d+;", f"kBaseSize = {cfg['kBaseSize']};", content)
    for i in range(6):
        content = re.sub(r"kT" + str(i+1) + r"Size = \d+;", f"kT{i+1}Size = {cfg['t_sizes'][i]};", content)
    
    hl_str = ", ".join(map(str, cfg['kHistoryLengths']))
    content = re.sub(r"kHistoryLengths\[7\] = \{.*?\};", f"kHistoryLengths[7] = {{{hl_str}}};", content)
    
    iw_str = ", ".join(map(str, cfg['kIndexWidths']))
    content = re.sub(r"kIndexWidths\[7\] = \{.*?\};", f"kIndexWidths[7] = {{{iw_str}}};", content)
    
    tw_str = ", ".join(map(str, cfg['kTagWidths']))
    content = re.sub(r"kTagWidths\[7\] = \{.*?\};", f"kTagWidths[7] = {{{tw_str}}};", content)
    
    content = re.sub(r"kScNumTables = \d+;", f"kScNumTables = {cfg['kScNumTables']};", content)
    content = re.sub(r"kScTableSize = \d+;", f"kScTableSize = {cfg['kScTableSize']};", content)
    content = re.sub(r"kLoopTableSize = \d+;", f"kLoopTableSize = {cfg['kLoopTableSize']};", content)
    
    with open(H_PATH, "w") as f:
        f.write(content)

def run_test(config_name, pisa_file, json_file, test_label):
    cmd = ["./pipesim", pisa_file, json_file, "0"]
    try:
        proc = subprocess.run(cmd, input="n 200000\np\ns\nq\n", text=True, cwd=SUBMISSION_DIR, capture_output=True)
        output = proc.stdout
        
        def extract(key):
            m = re.search(fr'{key}\s+(\d+)', output, re.MULTILINE)
            return int(m.group(1)) if m else 0

        bits = extract(r'predictor0\.storage_bits')
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
        
        with open(RESULTS_FILE, 'a') as f:
            f.write(f'"{config_name}",{test_label},{bits},{b_found},{b_not_found},{p_taken},{p_not_taken},{correct},{dir_mispred},{tgt_mispred},{false_pred},{missed},{accuracy},{cycles},{cpi}\n')
            
        return accuracy, bits
    except Exception as e:
        print(e)
        return "ERROR", "ERROR"

def main():
    os.makedirs(os.path.dirname(RESULTS_FILE), exist_ok=True)
    with open(RESULTS_FILE, 'w') as f:
        f.write("Config,Test,Storage_Bits,B_Found,B_Not_Found,P_Taken,P_Not_Taken,Correct,Dir_Mispred,Tgt_Mispred,False_Pred,Missed,Accuracy,Total_Cycles,CPI\n")
        
    shutil.copy2(H_PATH, TMP_HEADER)
    
    try:
        for cfg in CONFIGS:
            print(f"--- Sweeping {cfg['name']} ---")
            apply_config(cfg)
            subprocess.run(["make"], cwd=SUBMISSION_DIR, stdout=subprocess.DEVNULL, check=True)
            
            acc_c, bits_c = run_test(cfg['name'], "untracked_tests/complex_workload.pisa", "tests/release_tests/1.json", "Complex")
            acc_s, bits_s = run_test(cfg['name'], "untracked_scripts/branch_predictor_stress.pisa", "untracked_scripts/branch_predictor_stress.json", "Stress")
            
            print(f"Storage Bits: {bits_s}")
            print(f"Complex Acc:  {acc_c}")
            print(f"Stress Acc:   {acc_s}\n")
    finally:
        if os.path.exists(TMP_HEADER):
            shutil.move(TMP_HEADER, H_PATH)
        subprocess.run(["make"], cwd=SUBMISSION_DIR, stdout=subprocess.DEVNULL)
        print(f"Search completed. Results saved to {RESULTS_FILE}")

if __name__ == "__main__":
    main()
