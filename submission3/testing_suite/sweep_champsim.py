import re
import subprocess
import os
import shutil

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SUBMISSION_DIR = os.path.dirname(SCRIPT_DIR)
RESULTS_FILE = os.path.join(SUBMISSION_DIR, "untracked_tests/sweep_champsim_results_no_ph.csv")
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

def build_runner():
    cmd = ["g++", "-std=c++17", "-O3", "-I.", "testing_suite/champsim_runner.cc", "predictors/predictor_one.cc", "-o", "champsim_runner"]
    subprocess.run(cmd, cwd=SUBMISSION_DIR, check=True)

def run_test(config_name, trace_file, test_label):
    cmd = ["./champsim_runner", "--warmup-instructions", "20000000", "--simulation-instructions", "50000000", trace_file]
    try:
        proc = subprocess.run(cmd, text=True, cwd=SUBMISSION_DIR, capture_output=True)
        output = proc.stdout
        
        def extract_float(key):
            m = re.search(fr'{key}\s*:\s*([\d\.]+)', output, re.MULTILINE)
            return float(m.group(1)) if m else 0.0

        accuracy = extract_float(r'Accuracy')
        mpki = extract_float(r'MPKI \(Mispredicts per 1k inst\)')
        
        with open(RESULTS_FILE, 'a') as f:
            f.write(f'"{config_name}",{test_label},{accuracy},{mpki}\n')
            
        return accuracy, mpki
    except Exception as e:
        print(e)
        return 0.0, 0.0

def main():
    os.makedirs(os.path.dirname(RESULTS_FILE), exist_ok=True)
    with open(RESULTS_FILE, 'w') as f:
        f.write("Config,Trace,Accuracy,MPKI\n")
        
    shutil.copy2(H_PATH, TMP_HEADER)
    
    try:
        for cfg in CONFIGS:
            print(f"--- Sweeping {cfg['name']} ---")
            apply_config(cfg)
            build_runner()
            
            acc_b, mpki_b = run_test(cfg['name'], "untracked_tests/410.bwaves-945B.champsimtrace.xz", "410.bwaves")
            acc_m, mpki_m = run_test(cfg['name'], "untracked_tests/433.milc-337B.champsimtrace.xz", "433.milc")
            
            print(f"bwaves Acc: {acc_b} (MPKI {mpki_b})")
            print(f"milc Acc:   {acc_m} (MPKI {mpki_m})\n")
    finally:
        if os.path.exists(TMP_HEADER):
            shutil.move(TMP_HEADER, H_PATH)
        print(f"Search completed. Results saved to {RESULTS_FILE}")

if __name__ == "__main__":
    main()
