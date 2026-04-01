#include "../../runtime/sdk/cpp/velix_process.hpp"
#include <array>
#include <cstdio>
#include <sstream>
#include <vector>

/**
 * @brief Simple C++ Skill to execute commandline tasks.
 * Inherits from VelixProcess to gain OS connectivity and IPC.
 */
class CommandLineSkill : public velix::core::VelixProcess {
public:
  CommandLineSkill() : velix::core::VelixProcess("commandline", "skill") {}

  /**
   * @brief The core execution logic.
   * Injected parameters (params) are used to determine which command to run.
   */
  void run() override {
    // 1. Extract Validated Parameters
    if (!params.contains("cmd")) {
      report_error("Missing required parameter: cmd");
      return;
    }

    std::string cmd = params["cmd"].get<std::string>();
    std::vector<std::string> args;
    if (params.contains("args") && params["args"].is_array()) {
      for (const auto &arg : params["args"]) {
        if (arg.is_string())
          args.push_back(arg.get<std::string>());
      }
    }

    // 2. Safe Command Construction
    std::stringstream ss;
    ss << cmd;
    for (const auto &arg : args) {
      // Simple space-escaping for basic correctness
      ss << " \"" << arg << "\"";
    }
    std::string full_command =
        ss.str() + " 2>&1"; // Redirect stderr to stdout for capture

    // 3. Piped Execution to Capture Output
    std::string output;
    std::array<char, 128> buffer;

#ifdef _WIN32
    auto pipe = _popen(full_command.c_str(), "r");
#else
    auto pipe = popen(full_command.c_str(), "r");
#endif

    if (!pipe) {
      report_error("Failed to spawn command process pipe.");
      return;
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
      output += buffer.data();
    }

#ifdef _WIN32
    int exit_code = _pclose(pipe);
#else
    int exit_code = pclose(pipe);
#endif

    // 4. Report Final Result back to Parent
    json result = {{"status", exit_code == 0 ? "success" : "error"},
                   {"exit_code", exit_code},
                   {"output", output},
                   {"command", full_command}};

    // Dispatch correctly to the parent requester via the Velix Bus
    report_result(parent_pid, result, entry_trace_id);
    result_reported = true; // Prevents the SDK ResultGuard from sending a
                            // duplicate generic 'completed'
  }

private:
  void report_error(const std::string &message) {
    json err = {{"status", "error"}, {"error", message}};
    report_result(parent_pid, err, entry_trace_id);
    result_reported = true;
  }
};

int main() {
  CommandLineSkill skill;
  // start() performs:
  // 1. REGISTER_PID with Supervisor
  // 2. Spawns heartbeat & Bus listeners
  // 3. Blocks until run() completes
  skill.start();
  return 0;
}
