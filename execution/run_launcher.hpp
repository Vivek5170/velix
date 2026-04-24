#pragma once

#include "runtime_adapters/runtime_adapters.hpp"

#include "../communication/json_include.hpp"

#include <map>
#include <string>

namespace velix::core::run_launcher {

using json = nlohmann::json;

json launch(const runtime_adapters::RuntimeResolution &runtime,
            const std::map<std::string, std::string> &env,
            const std::string &workdir, const std::string &trace_id);

} // namespace velix::core::run_launcher
