#include "../../runtime/sdk/cpp/velix_process.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace velix::core;

class StressTestExecutorTool : public VelixProcess {
public:
  StressTestExecutorTool() : VelixProcess("stress_test_executor", "tool") {}

  void run() override {
    int calls = params.value("calls", 10);
    calls = std::clamp(calls, 1, 300);

    const std::string cmd = params.value("cmd", std::string("echo stress_test_executor"));
    const int timeout_sec = std::clamp(params.value("timeout_sec", 30), 1, 600);

    struct CallResult {
      int index = 0;
      long long start_ms = 0;
      long long end_ms = 0;
      long long latency_ms = 0;
      bool ok = false;
      std::string error;
      json terminal_result = json::object();
    };

    auto now_ms = []() -> long long {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    };

    std::vector<std::future<CallResult>> futures;
    futures.reserve(static_cast<std::size_t>(calls));

    for (int i = 0; i < calls; ++i) {
      futures.push_back(std::async(std::launch::async, [this, i, cmd, timeout_sec, now_ms]() {
        CallResult out;
        out.index = i + 1;
        out.start_ms = now_ms();

        try {
          // Split cmd string into binary and args for non-PTY execution
          std::string binary;
          std::vector<std::string> args;
          std::stringstream ss(cmd);
          std::string word;
          if (ss >> word) {
            binary = word;
            while (ss >> word) {
              args.push_back(word);
            }
          }

          json terminal_params = {
              {"cmd", binary.empty() ? cmd : binary},
              {"args", args},
              {"timeout_sec", timeout_sec},
              {"cwd", "."},
              {"pty", false}
          };

          out.terminal_result = execute_tool("terminal", terminal_params);
          out.ok = out.terminal_result.value("status", "") == "ok";
          if (!out.ok) {
            out.error = out.terminal_result.value("error", "terminal_call_failed");
          }
        } catch (const std::exception &e) {
          out.ok = false;
          out.error = e.what();
        }

        out.end_ms = now_ms();
        out.latency_ms = out.end_ms - out.start_ms;
        return out;
      }));
    }

    json calls_json = json::array();
    int failures = 0;
    long long min_latency = std::numeric_limits<long long>::max();
    long long max_latency = 0;
    long long total_latency = 0;

    for (auto &fut : futures) {
      CallResult r = fut.get();
      if (!r.ok) {
        ++failures;
      }
      min_latency = std::min(min_latency, r.latency_ms);
      max_latency = std::max(max_latency, r.latency_ms);
      total_latency += r.latency_ms;

      calls_json.push_back({
          {"index", r.index},
          {"start_ms", r.start_ms},
          {"end_ms", r.end_ms},
          {"latency_ms", r.latency_ms},
          {"ok", r.ok},
          {"error", r.error},
          {"terminal_result", r.terminal_result},
      });
    }

    const long long avg_latency = calls > 0 ? (total_latency / calls) : 0;
    if (calls == 0) {
      min_latency = 0;
    }

    json result = {
        {"status", failures == 0 ? "ok" : "error"},
        {"calls", calls},
        {"failures", failures},
        {"successes", calls - failures},
        {"latency", {
            {"min_ms", min_latency},
            {"max_ms", max_latency},
            {"avg_ms", avg_latency},
            {"total_ms", total_latency}
        }},
        {"results", calls_json},
    };

    report_result(parent_pid, result, entry_trace_id);
  }
};

int main() {
  StressTestExecutorTool tool;
  try {
    tool.start();
  } catch (...) {
    return 1;
  }
  return 0;
}
