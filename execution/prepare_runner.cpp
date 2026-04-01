#include "prepare_runner.hpp"

#include "../utils/process_spawner.hpp"

#include <fstream>

namespace velix::core::prepare_runner {

namespace {

void append_prepare_log(const std::string &log_file, const std::string &text) {
  std::ofstream out(log_file, std::ios::app);
  if (!out.is_open()) {
    return;
  }
  out << text << '\n';
}

} // namespace

bool parse_prepare_steps(const json &manifest, int default_timeout_ms,
                         std::vector<PrepareStep> &steps, std::string &error) {
  const json prepare = manifest.value("prepare", json::array());
  if (!prepare.is_array()) {
    error = "prepare must be an array";
    return false;
  }

  for (std::size_t i = 0; i < prepare.size(); ++i) {
    const auto &entry = prepare[i];
    if (!entry.is_object()) {
      error = "prepare step must be object at index " + std::to_string(i);
      return false;
    }
    if (!entry.contains("command") || !entry["command"].is_string()) {
      error = "prepare step missing command at index " + std::to_string(i);
      return false;
    }

    PrepareStep step;
    step.command = entry["command"].get<std::string>();
    step.timeout_ms = entry.value("timeout_ms", default_timeout_ms);

    if (entry.contains("args")) {
      if (!entry["args"].is_array()) {
        error = "prepare step args must be array at index " + std::to_string(i);
        return false;
      }
      for (const auto &arg : entry["args"]) {
        if (!arg.is_string()) {
          error = "prepare step arg must be string at index " + std::to_string(i);
          return false;
        }
        step.args.push_back(arg.get<std::string>());
      }
    }

    steps.push_back(std::move(step));
  }

  return true;
}

json execute_prepare(const std::vector<PrepareStep> &steps,
                    const std::map<std::string, std::string> &env,
                    const std::string &workdir, const std::string &trace_id,
                    const std::string &log_file) {
  for (std::size_t i = 0; i < steps.size(); ++i) {
    const auto &step = steps[i];
    append_prepare_log(log_file, "[prepare] step=" + std::to_string(i) +
                                     " cmd=" + step.command);

    auto res = velix::utils::ProcessSpawner::run_sync_with_timeout(
        step.command, step.args, env, step.timeout_ms, workdir);

    if (!res.stdout_content.empty()) {
      append_prepare_log(log_file, res.stdout_content);
    }

    if (!res.success) {
      append_prepare_log(log_file,
                         "[prepare] failed exit=" + std::to_string(res.exit_code));
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"phase", "prepare"},
              {"step_index", static_cast<int>(i)},
              {"command", step.command},
              {"exit_code", res.exit_code},
              {"timed_out", res.timed_out},
              {"stdout", res.stdout_content},
              {"error", "prepare_step_failed"}};
    }

    append_prepare_log(log_file, "[prepare] step=" + std::to_string(i) +
                                     " status=ok");
  }

  return {{"status", "ok"}};
}

} // namespace velix::core::prepare_runner
