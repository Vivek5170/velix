#include "run_launcher.hpp"

#include "../utils/process_spawner.hpp"

namespace velix::core::run_launcher {

json launch(const runtime_adapters::RuntimeResolution &runtime,
            const std::map<std::string, std::string> &env,
            const std::string &workdir, const std::string &trace_id) {
  auto spawn_res = velix::utils::ProcessSpawner::spawn(
      runtime.run_command, runtime.run_args, env, workdir);

  if (spawn_res.first <= 0) {
    return {{"status", "error"},
            {"trace_id", trace_id},
            {"phase", "run"},
            {"error", "spawn_failure: " + spawn_res.second}};
  }

  return {{"status", "ok"},
          {"trace_id", trace_id},
          {"os_pid", spawn_res.first}};
}

} // namespace velix::core::run_launcher
