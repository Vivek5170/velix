#include "runtime_adapters.hpp"

#include "../../utils/process_spawner.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace velix::core::runtime_adapters {

namespace fs = std::filesystem;

namespace {

bool binary_exists(const std::string &bin) {
  if (bin.empty()) {
    return false;
  }
  try {
    auto res = velix::utils::ProcessSpawner::run_sync("which", {bin});
    return res.success;
  } catch (...) {
    return false;
  }
}

std::string trim(std::string value) {
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
          value.back() == '\t')) {
    value.pop_back();
  }
  std::size_t start = 0;
  while (start < value.size() &&
         (value[start] == ' ' || value[start] == '\t' || value[start] == '\n' ||
          value[start] == '\r')) {
    ++start;
  }
  return value.substr(start);
}

std::string detect_runtime_version(const std::string &command,
                                   const std::vector<std::string> &args) {
  auto res = velix::utils::ProcessSpawner::run_sync(command, args);
  std::string out = trim(res.stdout_content);
  if (out.empty()) {
    out = trim(res.stderr_content);
  }
  if (out.empty()) {
    return "unknown";
  }
  return out;
}

bool file_exists_in(const std::string &workdir, const std::string &entry) {
  const fs::path p = fs::path(workdir) / entry;
  return fs::exists(p) && fs::is_regular_file(p);
}

bool has_prepare_command(const json &manifest, const std::string &command) {
  const json prepare = manifest.value("prepare", json::array());
  if (!prepare.is_array()) {
    return false;
  }
  for (const auto &step : prepare) {
    if (step.is_object() && step.value("command", std::string("")) == command) {
      return true;
    }
  }
  return false;
}

bool path_looks_local_binary(const std::string &command) {
  return !command.empty() &&
         (command.rfind("./", 0) == 0 || command.rfind("../", 0) == 0);
}

} // namespace

bool select_runtime_adapter(const json &manifest,
                            const std::string &package_path,
                            const std::string &workdir,
                            RuntimeResolution &runtime, std::string &error) {
  (void)package_path;

  runtime.runtime = manifest.value("runtime", "");
  std::transform(runtime.runtime.begin(), runtime.runtime.end(),
                 runtime.runtime.begin(), [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });

  runtime.injected_prepare_steps.clear();
  runtime.env_overrides.clear();

  const json run = manifest.value("run", json::object());
  const std::string entry = manifest.value("entry", std::string(""));
  runtime.run_command = run.value("command", std::string(""));

  runtime.run_args.clear();
  if (run.contains("args") && run["args"].is_array()) {
    for (const auto &arg : run["args"]) {
      if (arg.is_string()) {
        runtime.run_args.push_back(arg.get<std::string>());
      }
    }
  }

  if (runtime.runtime == "python") {
    if (!binary_exists("python") && !binary_exists("uv")) {
      error = "python runtime requires python or uv binary";
      return false;
    }

    if (runtime.run_command.empty()) {
      const std::string inferred_entry = entry.empty() ? "main.py" : entry;
      if (!file_exists_in(workdir, inferred_entry)) {
        error = "python entry file not found: " + inferred_entry;
        return false;
      }
      if (binary_exists("uv")) {
        runtime.run_command = "uv";
        runtime.run_args = {"run", inferred_entry};
      } else {
        runtime.run_command = "python";
        runtime.run_args = {inferred_entry};
      }
    }

    if (runtime.run_command != "python" && runtime.run_command != "python3" && runtime.run_command != "uv") {
      error = "python runtime must use python or uv";
      return false;
    }

    if (runtime.run_command == "python" || runtime.run_command == "python3") {
      if (runtime.run_args.empty()) {
        error = "python runtime requires entry script arg";
        return false;
      }
      if (!file_exists_in(workdir, runtime.run_args.front())) {
        error = "python entry file not found: " + runtime.run_args.front();
        return false;
      }
    }

    if (runtime.run_command == "uv") {
      if (runtime.run_args.size() < 2 || runtime.run_args[0] != "run") {
        error = "python uv runtime requires args like: run <entry.py>";
        return false;
      }
      if (!file_exists_in(workdir, runtime.run_args[1])) {
        error = "python entry file not found: " + runtime.run_args[1];
        return false;
      }
    }

    runtime.version = detect_runtime_version("python", {"--version"});
    runtime.env_overrides["PYTHONUNBUFFERED"] = "1";
    runtime.env_overrides["VELIX_RUNTIME"] = "python";
  } else if (runtime.runtime == "node") {
    if (!binary_exists("node")) {
      error = "node runtime requires node binary";
      return false;
    }

    if (runtime.run_command.empty()) {
      const std::string inferred_entry = entry.empty() ? "index.js" : entry;
      if (!file_exists_in(workdir, inferred_entry)) {
        error = "node entry file not found: " + inferred_entry;
        return false;
      }
      runtime.run_command = "node";
      runtime.run_args = {inferred_entry};
    }

    if (runtime.run_command != "node") {
      error = "node runtime requires run.command to be node";
      return false;
    }

    if (runtime.run_args.empty()) {
      error = "node runtime requires entry script arg";
      return false;
    }
    if (!file_exists_in(workdir, runtime.run_args.front())) {
      error = "node entry file not found: " + runtime.run_args.front();
      return false;
    }

    if (fs::exists(fs::path(workdir) / "package.json") &&
        binary_exists("pnpm") && !has_prepare_command(manifest, "pnpm")) {
      runtime.injected_prepare_steps.push_back(
          RuntimeResolution::PrepareStep{"pnpm", {"install", "--frozen-lockfile"},
                                        0});
    }

    runtime.version = detect_runtime_version("node", {"--version"});
    runtime.env_overrides["NODE_ENV"] = "production";
    runtime.env_overrides["VELIX_RUNTIME"] = "node";
  } else if (runtime.runtime == "rust") {
    if (!binary_exists("cargo")) {
      error = "rust runtime requires cargo binary";
      return false;
    }

    if (!path_looks_local_binary(runtime.run_command)) {
      error = "rust runtime expects run.command to be built local binary path";
      return false;
    }

    runtime.version = detect_runtime_version("cargo", {"--version"});
    runtime.env_overrides["CARGO_TARGET_DIR"] =
        (fs::path(workdir) / ".velix" / "cache" / "cargo-target").string();
    runtime.env_overrides["VELIX_RUNTIME"] = "rust";
  } else if (runtime.runtime == "go") {
    if (!binary_exists("go")) {
      error = "go runtime requires go binary";
      return false;
    }

    if (runtime.run_command.empty()) {
      const std::string inferred_entry = entry.empty() ? "main.go" : entry;
      if (!file_exists_in(workdir, inferred_entry)) {
        error = "go entry file not found: " + inferred_entry;
        return false;
      }
      runtime.run_command = "go";
      runtime.run_args = {"run", inferred_entry};
    }

    if (runtime.run_command == "go") {
      if (runtime.run_args.size() < 2 || runtime.run_args[0] != "run") {
        error = "go runtime with go command requires args like: run <entry.go>";
        return false;
      }
      if (!file_exists_in(workdir, runtime.run_args[1])) {
        error = "go entry file not found: " + runtime.run_args[1];
        return false;
      }
    } else if (!path_looks_local_binary(runtime.run_command)) {
      error = "go runtime must use go run <entry.go> or local built binary";
      return false;
    }

    runtime.version = detect_runtime_version("go", {"version"});
    runtime.env_overrides["VELIX_RUNTIME"] = "go";
  } else if (runtime.runtime == "cpp" || runtime.runtime == "c++") {
    if (!path_looks_local_binary(runtime.run_command)) {
      error = "cpp runtime expects run.command to be local binary path";
      return false;
    }

    if (fs::exists(fs::path(workdir) / "CMakeLists.txt") &&
        !has_prepare_command(manifest, "cmake")) {
      runtime.injected_prepare_steps.push_back(
          RuntimeResolution::PrepareStep{"cmake", {"-S", ".", "-B", "build"}, 0});
      runtime.injected_prepare_steps.push_back(
          RuntimeResolution::PrepareStep{"cmake", {"--build", "build", "-j"}, 0});
    }

    runtime.version = "cpp-toolchain";
    runtime.env_overrides["VELIX_RUNTIME"] = "cpp";
  } else {
    error = "unsupported runtime: " + runtime.runtime;
    return false;
  }

  if (runtime.version.empty()) {
    runtime.version = "unknown";
  }

  return true;
}

} // namespace velix::core::runtime_adapters
