#!/bin/bash

# Configuration Search Script for PredictorOne
# This script iterates through different combinations of history lengths, table sizes,
# and threshold parameters for the TAGE-SC-L predictor. It builds the simulator
# and runs the test suite for each configuration, saving the metrics to a CSV file.

# Output CSV for results
RESULTS_FILE="search_results.csv"
echo "Config,History_Lengths,SC_Threshold,Loop_Conf,B_Found,B_Not_Found,P_Taken,P_Not_Taken,Correct,Dir_Mispred,Tgt_Mispred,False_Pred,Missed,Accuracy,Total_Cycles,CPI" > $RESULTS_FILE

# Base source files
HEADER_FILE="submission3/predictors/predictor_one.h"
TMP_HEADER="submission3/predictors/predictor_one.h.tmp"

# Helper to restore original header
restore_header() {
    if [ -f "$TMP_HEADER" ]; then
        mv "$TMP_HEADER" "$HEADER_FILE"
    fi
}
trap restore_header EXIT INT TERM

# Save original header
cp "$HEADER_FILE" "$TMP_HEADER"

# --- Define Configurations to Search ---

# 1. History Length configurations (Default: 0, 6, 11, 20, 37, 68, 130)
# Format: "L0,L1,L2,L3,L4,L5,L6"
declare -a HISTORY_CONFIGS=(
    "0, 6, 11, 20, 37, 68, 130"    # Baseline (Geometric, max 130)
    "0, 4, 8, 16, 32, 64, 128"     # Power of 2 (max 128)
    "0, 8, 16, 32, 64, 100, 150"   # Extended history (max 150 - requires changing bitset size if >130, skipping for now, keeping max 130)
    "0, 5, 10, 20, 40, 80, 130"    # Aggressive geometric
)

# 2. SC Threshold configurations (Default: 18)
declare -a SC_THRESHOLDS=(
    14
    18
    22
)

# 3. Loop Confidence Thresholds (Default: 3)
declare -a LOOP_THRESHOLDS=(
    2
    3
    4
)

run_test() {
    local config_name=$1
    local hist_arr=$2
    local sc_thresh=$3
    local loop_thresh=$4

    echo "========================================================="
    echo "Running Configuration: $config_name"
    echo "History: [$hist_arr] | SC Thresh: $sc_thresh | Loop Conf: $loop_thresh"

    # Modify header file using sed
    # Replace history lengths array
    sed -i "s/static constexpr int kHistoryLengths\[7\] = .*/static constexpr int kHistoryLengths[7] = {$hist_arr};/" "$HEADER_FILE"
    
    # Replace SC Threshold
    sed -i "s/static constexpr int kScThreshold = .*/static constexpr int kScThreshold = $sc_thresh;/" "$HEADER_FILE"

    # Replace Loop Confidence Threshold
    # Since it's on two lines in the original source, we can use perl to do a multiline replace
    perl -0777 -pi -e "s/static constexpr std::uint32_t kLoopConfidenceThreshold =\s+[0-9]+;/static constexpr std::uint32_t kLoopConfidenceThreshold =\n      $loop_thresh;/" "$HEADER_FILE"

    # Compile the project
    echo "Compiling..."
    cd submission3
    make clean > /dev/null
    make pipesim > /dev/null
    local build_status=$?
    
    if [ $build_status -ne 0 ]; then
        echo "Build failed for config $config_name"
        cd ..
        return
    fi

    # Run the integration tests
    # Using one_hundred.pisa as a representative workload for branch prediction stats
    echo "Running test..."
    test_out=$(mktemp)
    printf 'n 100000\np\ns\nq\n' | ./pipesim untracked_tests/complex_workload.pisa default_config.json 0 > "$test_out"
    cd ..

    # Extract metrics using awk/grep
    local b_found=$(grep -E 'predictor0.branches_found' "$test_out" | awk '{print $2}')
    local b_not_found=$(grep -E 'predictor0.branches_not_found' "$test_out" | awk '{print $2}')
    local p_taken=$(grep -E 'predictor0.predicted_taken' "$test_out" | awk '{print $2}')
    local p_not_taken=$(grep -E 'predictor0.predicted_not_taken' "$test_out" | awk '{print $2}')
    local correct=$(grep -E 'predictor0.correct_predictions' "$test_out" | awk '{print $2}')
    local dir_mispred=$(grep -E 'predictor0.direction_mispredictions' "$test_out" | awk '{print $2}')
    local tgt_mispred=$(grep -E 'predictor0.target_mispredictions' "$test_out" | awk '{print $2}')
    local false_pred=$(grep -E 'predictor0.false_branch_predictions' "$test_out" | awk '{print $2}')
    local missed=$(grep -E 'predictor0.missed_branches' "$test_out" | awk '{print $2}')
    
    local cycles=$(grep -E '^cycles' "$test_out" | awk '{print $2}')
    local retired=$(grep -E '^retired' "$test_out" | awk '{print $2}')
    
    local cpi="0.00"
    if [ ! -z "$cycles" ] && [ ! -z "$retired" ] && [ "$retired" -gt 0 ]; then
        cpi=$(awk "BEGIN {printf \"%.2f\", $cycles / $retired}")
    fi

    # Calculate accuracy if total predictions > 0
    local total_preds=$((correct + dir_mispred + tgt_mispred))
    local accuracy="0.00"
    if [ "$total_preds" -gt 0 ]; then
        accuracy=$(awk "BEGIN {printf \"%.2f\", ($correct / $total_preds) * 100}")
    fi

    echo "Accuracy: ${accuracy}%, CPI: ${cpi}"

    # Append to CSV
    # Re-wrap hist_arr in quotes so CSV parsing doesn't split on commas
    echo "$config_name,\"$hist_arr\",$sc_thresh,$loop_thresh,$b_found,$b_not_found,$p_taken,$p_not_taken,$correct,$dir_mispred,$tgt_mispred,$false_pred,$missed,$accuracy,$cycles,$cpi" >> $RESULTS_FILE

    rm "$test_out"
}

# Run the parameter sweep
config_count=1
for hist in "${HISTORY_CONFIGS[@]}"; do
    for sc in "${SC_THRESHOLDS[@]}"; do
        for lc in "${LOOP_THRESHOLDS[@]}"; do
            run_test "Config_$config_count" "$hist" "$sc" "$lc"
            config_count=$((config_count + 1))
        done
    done
done

# Restore original header file immediately after tests finish
restore_header

echo "========================================================="
echo "Search completed. Results saved to $RESULTS_FILE"
