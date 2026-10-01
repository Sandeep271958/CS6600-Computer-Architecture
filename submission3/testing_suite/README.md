# TAGE-SC-L Testing Suite

This directory contains the custom testing suite developed to rigorously evaluate the TAGE-SC-L predictor. It includes workload generators, automated configuration sweep scripts, and a standalone ChampSim trace runner for processing massive real-world instruction traces.

## 1. Workload Generation

The standard provided `.pisa` tests were too simple to adequately evaluate the Geometric History Lengths or the Loop Predictor. We wrote two custom assembly generators:

### `generate_workload.py` (Complex Workload)
Generates `complex_workload.pisa`. This workload tests deep repeating patterns through nested loops. It executes an outer loop 100 times, containing three distinct inner loops that execute 4, 6, and 3 times respectively.
**Usage:**
```bash
python3 generate_workload.py
```

### `generate_branch_predictor_stress.py` (Stress Workload)
Generates `branch_predictor_stress.pisa`. This is an aggressive torture test. It creates 640 distinct loop blocks and uses a Linear Congruential Generator to assign a randomized trip count (between 1 and 31 iterations) to each loop. The sequence runs 3 times, creating massive capacity pressure and severely stressing the Loop Predictor.
**Usage:**
```bash
python3 generate_branch_predictor_stress.py
```

## 2. Configuration Sweeping

To tune our storage budget and history lengths, we developed automated Python sweepers that rewrite `predictor_one.h` temporarily, recompile the simulator, run traces, parse the MPKI/Accuracy, and export the results to CSV files.

### `sweep_budget.py`
Sweeps the full PA2 pipeline across Low (~30Kb), Medium (~60Kb), High (~85Kb), and Ultra (~160Kb) configurations using the synthetic `.pisa` workloads.
**Usage:**
```bash
python3 sweep_budget.py
```

## 3. ChampSim Release Testing

The full PA2 processor simulator is too slow to evaluate tens of millions of instructions. We wrote a standalone C++ runner (`champsim_runner.cc`) that strips away the pipeline overhead and directly feeds `.champsimtrace.xz` branch instructions into our `PredictorOne` class.

### `champsim_runner.cc`
A fast, standalone C++ wrapper around `PredictorOne`. It utilizes the `lzma` library to decompress ChampSim traces on the fly.
**Prerequisites:**
```bash
sudo apt-get install liblzma-dev
```
**Manual Compilation:**
```bash
g++ -O3 -I../ -o champsim_runner champsim_runner.cc ../predictors/predictor_one.cc -llzma
```

### `sweep_champsim.py`
Automates compiling `champsim_runner.cc` across the four storage budgets and running it against `410.bwaves` and `433.milc` (assuming the `.champsimtrace.xz` files are located in `../untracked_tests/`).
**Usage:**
```bash
python3 sweep_champsim.py
```
