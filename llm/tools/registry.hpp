#pragma once

#include "../../vendor/nlohmann/json.hpp"
#include "../../utils/process_spawner.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace velix::llm::tools {

using json = nlohmann::json;

struct ToolManifest {
  std::string name;
  std::string type;
  std::string description;
  std::string runtime;
  std::string workdir;
  std::string command;
  std::vector<std::string> command_args;
  json parameters = json::object();
};

class ToolRegistry {
public:
  ToolRegistry() { load_from_skills_directory(); }

  json get_tool_schemas() const { return cached_schemas_; }

  json execute_tool(const std::string &name, const json &args) const {
    const auto it = manifests_.find(name);
    if (it == manifests_.end()) {
      return {{"status", "error"},
              {"error", "tool_not_found"},
              {"tool", name},
              {"available_tools", list_tool_names()}};
    }

    const ToolManifest &manifest = it->second;
    if (manifest.command.empty()) {
      return {{"status", "error"},
              {"error", "tool_command_missing"},
              {"tool", name}};
    }

    // Velix skills are process-runtime components. For scheduler tool-calling,
    // we execute command-defined tools with JSON args as a positional argument.
    std::vector<std::string> run_args = manifest.command_args;
    run_args.push_back(args.dump());

    const std::string run_cwd = resolve_tool_workdir(manifest);
    velix::utils::ProcessResult result;
    try {
      result = velix::utils::ProcessSpawner::run_sync_with_timeout(
          manifest.command, run_args, {}, 30000, run_cwd);
    } catch (const std::exception &e) {
      return {{"status", "error"},
              {"error", "tool_process_exception"},
              {"tool", name},
              {"message", e.what()}};
    }

    json output = {{"status", result.success ? "success" : "error"},
                   {"tool", name},
                   {"exit_code", result.exit_code},
                   {"timed_out", result.timed_out},
                   {"stdout", result.stdout_content},
                   {"stderr", result.stderr_content}};

    // Best-effort parse structured stdout if tool prints JSON.
    try {
      const json parsed = json::parse(result.stdout_content);
      output["parsed"] = parsed;
    } catch (...) {
    }

    return output;
  }

private:
  std::map<std::string, ToolManifest> manifests_;
  json cached_schemas_ = json::array();

  static std::filesystem::path repo_root_from_cwd() {
    std::filesystem::path cur = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
      if (std::filesystem::exists(cur / "skills") &&
          std::filesystem::exists(cur / "config")) {
        return cur;
      }
      if (!cur.has_parent_path()) {
        break;
      }
      cur = cur.parent_path();
    }
    return std::filesystem::current_path();
  }

  static std::vector<std::string> list_directory_names(
      const std::filesystem::path &root) {
    std::vector<std::string> out;
    if (!std::filesystem::exists(root) || !std::filesystem::is_directory(root)) {
      return out;
    }

    for (const auto &entry : std::filesystem::directory_iterator(root)) {
      if (entry.is_directory()) {
        out.push_back(entry.path().filename().string());
      }
    }
    return out;
  }

  void load_from_skills_directory() {
    manifests_.clear();
    const std::filesystem::path repo_root = repo_root_from_cwd();
    const std::filesystem::path skills_root = repo_root / "skills";

    for (const auto &dir_name : list_directory_names(skills_root)) {
      const std::filesystem::path manifest_path =
          skills_root / dir_name / "manifest.json";
      if (!std::filesystem::exists(manifest_path)) {
        continue;
      }

      json manifest_json;
      try {
        std::ifstream in(manifest_path);
        in >> manifest_json;
      } catch (...) {
        continue;
      }

      ToolManifest manifest;
      manifest.name = (manifest_json.contains("name") && manifest_json["name"].is_string()) ?
          manifest_json["name"].get<std::string>() : dir_name;
      manifest.type = (manifest_json.contains("type") && manifest_json["type"].is_string()) ?
          manifest_json["type"].get<std::string>() : std::string("skill");
      manifest.description = (manifest_json.contains("description") && manifest_json["description"].is_string()) ?
          manifest_json["description"].get<std::string>() : std::string("");
      manifest.runtime = (manifest_json.contains("runtime") && manifest_json["runtime"].is_string()) ?
          manifest_json["runtime"].get<std::string>() : std::string("");
      manifest.workdir = (manifest_json.contains("workdir") && manifest_json["workdir"].is_string()) ?
          manifest_json["workdir"].get<std::string>() : std::string("");
      if (manifest_json.contains("parameters") && manifest_json["parameters"].is_object()) {
        manifest.parameters = manifest_json["parameters"];
      } else {
        manifest.parameters = json::object();
      }

      if (manifest_json.contains("run") && manifest_json["run"].is_object()) {
        const json run = manifest_json["run"];
        manifest.command = (run.contains("command") && run["command"].is_string()) ?
            run["command"].get<std::string>() : std::string("");
        if (run.contains("args") && run["args"].is_array()) {
          for (const auto &arg : run["args"]) {
            if (arg.is_string()) {
              manifest.command_args.push_back(arg.get<std::string>());
            }
          }
        }
      }

      manifests_[manifest.name] = manifest;
    }
    recompute_cached_schemas();
  }

  std::vector<std::string> list_tool_names() const {
    std::vector<std::string> names;
    for (const auto &entry : manifests_) {
      names.push_back(entry.first);
    }
    return names;
  }

  void recompute_cached_schemas() {
    cached_schemas_ = json::array();
    for (const auto &entry : manifests_) {
      const ToolManifest &manifest = entry.second;
      cached_schemas_.push_back(
          {{"type", "function"},
           {"function",
            {{"name", manifest.name},
             {"description", manifest.description},
             {"parameters", manifest.parameters.is_object() ? manifest.parameters
                                                              : json::object()}}}});
    }
  }

  static std::string resolve_tool_workdir(const ToolManifest &manifest) {
    if (manifest.workdir.empty()) {
      return std::filesystem::current_path().string();
    }

    std::filesystem::path path(manifest.workdir);
    if (path.is_absolute()) {
      return path.string();
    }

    const std::filesystem::path repo_root = repo_root_from_cwd();
    const std::filesystem::path from_repo = repo_root / path;
    if (std::filesystem::exists(from_repo)) {
      return from_repo.string();
    }

    return (std::filesystem::current_path() / path).lexically_normal().string();
  }
};

} // namespace velix::llm::tools
