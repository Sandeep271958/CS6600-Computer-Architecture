#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>


// Include your custom predictor
#include "../cache/cache_config.h"
#include "../predictors/predictor_one.h"


// Standard ChampSim trace instruction format
struct trace_instr_format {
  unsigned long long ip;
  unsigned char is_branch;
  unsigned char branch_taken;
  unsigned char destination_registers[2];
  unsigned char source_registers[4];
  unsigned long long destination_memory[2];
  unsigned long long source_memory[4];
};

int main(int argc, char *argv[]) {
  uint64_t warmup_instructions = 0;
  uint64_t sim_instructions = UINT64_MAX;
  std::string trace_file = "";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--warmup-instructions" && i + 1 < argc) {
      warmup_instructions = std::stoull(argv[++i]);
    } else if (arg == "--simulation-instructions" && i + 1 < argc) {
      sim_instructions = std::stoull(argv[++i]);
    } else {
      trace_file = arg;
    }
  }

  if (trace_file.empty()) {
    std::cerr << "Usage: " << argv[0] << " [--warmup-instructions N] [--simulation-instructions N] <trace_file.xz>" << std::endl;
    return 1;
  }

  // Use popen to pipe the .xz file through xzcat and read binary stream
  std::string cmd = "xzcat " + trace_file;
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    std::cerr << "Error: Could not open pipe to xzcat. Make sure xz-utils is installed." << std::endl;
    return 1;
  }

  // Initialize predictor
  pipesim::PredictorOne predictor;
  predictor.Reset();

  uint64_t instr_count = 0;
  uint64_t sim_instr_count = 0;
  uint64_t branches_found = 0;
  uint64_t correct_predictions = 0;

  trace_instr_format instr;

  std::cout << "Starting trace simulation for: " << trace_file << "\n"
            << "Warmup Instructions    : " << warmup_instructions << "\n"
            << "Simulation Instructions: " << sim_instructions << std::endl;

  // Read the binary stream struct by struct
  while (fread(&instr, sizeof(trace_instr_format), 1, pipe) == 1) {
    instr_count++;
    
    bool is_warmup = (instr_count <= warmup_instructions);
    
    if (!is_warmup) {
      sim_instr_count++;
      if (sim_instr_count > sim_instructions) {
        break;
      }
    }

    if (instr.is_branch) {
      // Assume 4 bytes instruction size, target can just be modeled simply or extracted if available
      // ChampSim doesn't strictly provide branch target IP in standard trace without execution,
      // but pipesim API needs a target for PredictBranch. We can supply a dummy target for prediction purposes.
      uint32_t pc = static_cast<uint32_t>(instr.ip);
      uint32_t target = pc + 4; // Dummy target
      bool actual_taken = (instr.branch_taken != 0);

      // 1. Predict
      pipesim::BranchPrediction prediction = predictor.Predict(pc);
      bool predicted_taken = prediction.taken;

      // 2. Track accuracy (only during simulation phase)
      if (!is_warmup) {
        branches_found++;
        if (predicted_taken == actual_taken) {
          correct_predictions++;
        }
      }

      // 3. Update
      pipesim::BranchResult result;
      result.pc = pc;
      result.target = target;
      result.taken = actual_taken;
      result.type = pipesim::BranchType::kConditional; // Assume conditional for tracing

      predictor.Update(result, prediction);
    }

    if (instr_count % 10000000 == 0) {
      std::cout << "Processed " << instr_count / 1000000 << "M instructions..." << std::endl;
    }
  }

  pclose(pipe);

  // Calculate metrics
  double accuracy = (branches_found > 0) ? (100.0 * correct_predictions / branches_found) : 0.0;
  double mpki = (sim_instr_count > 0) ? (1000.0 * (branches_found - correct_predictions) / sim_instr_count) : 0.0;

  std::cout << "\n============================================\n";
  std::cout << "Simulation Completed!\n";
  std::cout << "Total Inst Processed : " << (instr_count - 1) << "\n";
  std::cout << "Warmup Instructions  : " << warmup_instructions << "\n";
  std::cout << "Sim Instructions     : " << sim_instr_count << "\n";
  std::cout << "Branches Simulated   : " << branches_found << "\n";
  std::cout << "Correct Predictions  : " << correct_predictions << "\n";
  std::cout << "Mispredictions       : " << (branches_found - correct_predictions) << "\n";
  std::cout << "--------------------------------------------\n";
  std::cout << "Accuracy           : " << std::fixed << std::setprecision(2) << accuracy << "%\n";
  std::cout << "MPKI (Mispredicts per 1k inst): " << std::fixed << std::setprecision(3) << mpki << "\n";
  std::cout << "============================================\n";

  return 0;
}
