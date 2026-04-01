#pragma once

#include "../vendor/nlohmann/json.hpp"

#include <map>
#include <string>
#include <vector>

namespace velix::core::prepare_runner {

using json = nlohmann::json;

struct PrepareStep {
  std::string command;
  std::vector<std::string> args;
  int timeout_ms{0};
};

bool parse_prepare_steps(const json &manifest, int default_timeout_ms,
                         std::vector<PrepareStep> &steps, std::string &error);

json execute_prepare(const std::vector<PrepareStep> &steps,
                     const std::map<std::string, std::string> &env,
                     const std::string &workdir, const std::string &trace_id,
                     const std::string &log_file);

} // namespace velix::core::prepare_runner
