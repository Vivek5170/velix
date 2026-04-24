#pragma once

#include "../../communication/json_include.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
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
  ToolRegistry() { load_from_tools_directory(); }

  json get_tool_schemas() const { return cached_schemas_; }

private:
  std::map<std::string, ToolManifest, std::less<>> manifests_;
  json cached_schemas_ = json::array();

  static std::string get_string_or(const json &obj, const char *key,
                                   const std::string &fallback) {
    if (obj.contains(key) && obj[key].is_string()) {
      return obj[key].get<std::string>();
    }
    return fallback;
  }

  static std::filesystem::path repo_root_from_cwd() {
    std::filesystem::path cur = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
      if (std::filesystem::exists(cur / "tools") &&
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

  static std::vector<std::string>
  list_directory_names(const std::filesystem::path &root) {
    std::vector<std::string> out;
    if (!std::filesystem::exists(root) ||
        !std::filesystem::is_directory(root)) {
      return out;
    }

    for (const auto &entry : std::filesystem::directory_iterator(root)) {
      if (entry.is_directory()) {
        out.push_back(entry.path().filename().string());
      }
    }
    return out;
  }

  void load_from_tools_directory() {
    manifests_.clear();
    const std::filesystem::path repo_root = repo_root_from_cwd();
    const std::filesystem::path tools_root = repo_root / "tools";

    for (const auto &dir_name : list_directory_names(tools_root)) {
      const std::filesystem::path manifest_path =
          tools_root / dir_name / "manifest.json";
      if (!std::filesystem::exists(manifest_path)) {
        continue;
      }

      json manifest_json;
      try {
        std::ifstream in(manifest_path);
        in >> manifest_json;
      } catch (const nlohmann::json::exception &) {
        continue;
      } catch (const std::ios_base::failure &) {
        continue;
      }

      ToolManifest manifest;
      manifest.name = get_string_or(manifest_json, "name", dir_name);
      manifest.type = get_string_or(manifest_json, "type", "tool");
      manifest.description = get_string_or(manifest_json, "description", "");
      manifest.runtime = get_string_or(manifest_json, "runtime", "");
      manifest.workdir = get_string_or(manifest_json, "workdir", "");
      if (manifest_json.contains("parameters") &&
          manifest_json["parameters"].is_object()) {
        manifest.parameters = manifest_json["parameters"];
      } else {
        manifest.parameters = json::object();
      }

      if (manifest_json.contains("run") && manifest_json["run"].is_object()) {
        const json run = manifest_json["run"];
        manifest.command = get_string_or(run, "command", "");
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
    for (const auto &[name, manifest] : manifests_) {
      (void)manifest;
      names.push_back(name);
    }
    return names;
  }

  void recompute_cached_schemas() {
    cached_schemas_ = json::array();
    for (const auto &[name, manifest] : manifests_) {
      cached_schemas_.push_back(
          {{"type", "function"},
           {"function",
            {{"name", name},
             {"description", manifest.description},
             {"parameters", manifest.parameters.is_object()
                                ? manifest.parameters
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
    if (const std::filesystem::path from_repo = repo_root / path;
        std::filesystem::exists(from_repo)) {
      return from_repo.string();
    }

    return (std::filesystem::current_path() / path).lexically_normal().string();
  }
};

} // namespace velix::llm::tools
