#pragma once

#include "../../communication/json_include.hpp"

#include <map>
#include <string>
#include <vector>

namespace velix::core::runtime_adapters {

using json = nlohmann::json;

struct RuntimeResolution {
  std::string runtime;
  std::string version;
  std::string run_command;
  std::vector<std::string> run_args;
  struct PrepareStep {
    std::string command;
    std::vector<std::string> args;
    int timeout_ms{0};
  };
  std::vector<PrepareStep> injected_prepare_steps;
  std::map<std::string, std::string> env_overrides;
};

bool select_runtime_adapter(const json &manifest,
                            const std::string &package_path,
                            const std::string &workdir,
                            RuntimeResolution &runtime, std::string &error);

} // namespace velix::core::runtime_adapters
