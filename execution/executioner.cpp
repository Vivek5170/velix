#include "../communication/network_config.hpp"
#include "../communication/socket_wrapper.hpp"
#include "prepare_runner.hpp"
#include "run_launcher.hpp"
#include "runtime_adapters/runtime_adapters.hpp"
#include "../utils/config_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/process_spawner.hpp"
#include "../utils/thread_pool.hpp"
#include "../vendor/nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace velix::core {

namespace {

struct ExecutionerConfig {
  int socket_timeout_ms = 5000;
  int max_client_threads = 32;
  int max_queue_size = 128;
  int prepare_timeout_ms = 300000;
  int run_timeout_ms = 0;
  std::string cache_root = ".velix/build_cache";
};

#ifndef _WIN32
class FileLockGuard {
public:
  explicit FileLockGuard(const std::string &lock_file) {
    fd_ = ::open(lock_file.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd_ < 0) {
      throw std::runtime_error("failed to open lock file: " + lock_file);
    }
    if (flock(fd_, LOCK_EX) != 0) {
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error("failed to acquire lock: " + lock_file);
    }
  }

  ~FileLockGuard() {
    if (fd_ >= 0) {
      flock(fd_, LOCK_UN);
      ::close(fd_);
    }
  }

private:
  int fd_{-1};
};
#else
class FileLockGuard {
public:
  explicit FileLockGuard(const std::string &) {}
  ~FileLockGuard() = default;
};
#endif

ExecutionerConfig load_executioner_config() {
  ExecutionerConfig cfg;

  std::ifstream config_file("config/executioner.json");
  if (!config_file.is_open()) {
    config_file.open("../config/executioner.json");
  }
  if (!config_file.is_open()) {
    config_file.open("build/config/executioner.json");
  }
  if (config_file.is_open()) {
    try {
      json config;
      config_file >> config;
      cfg.socket_timeout_ms =
          config.value("socket_timeout_ms", cfg.socket_timeout_ms);
      cfg.max_client_threads =
          config.value("max_client_threads", cfg.max_client_threads);
      cfg.max_queue_size = config.value("max_queue_size", cfg.max_queue_size);
        cfg.prepare_timeout_ms =
          config.value("prepare_timeout_ms", cfg.prepare_timeout_ms);
        cfg.run_timeout_ms = config.value("run_timeout_ms", cfg.run_timeout_ms);
        cfg.cache_root = config.value("cache_root", cfg.cache_root);
    } catch (const std::exception &e) {
      LOG_WARN(
          std::string("Executioner failed to parse config/executioner.json: ") +
          e.what());
    }
  }

  return cfg;
}

class ExecutionerService {
public:
  ExecutionerService()
      : running_(false), config_(load_executioner_config()),
        thread_pool_(config_.max_client_threads, config_.max_queue_size) {}

  ~ExecutionerService() { stop(); }

  void start(int port) {
    if (running_)
      return;
    running_ = true;

    try {
      const std::string bind_host =
          velix::communication::resolve_bind_host("EXECUTIONER", "127.0.0.1");
      {
        std::lock_guard<std::mutex> lock(server_mutex_);
        server_socket_ =
            std::make_unique<velix::communication::SocketWrapper>();
        server_socket_->create_tcp_socket();
        server_socket_->bind(bind_host, static_cast<uint16_t>(port));
        server_socket_->listen(16);
      }

      LOG_INFO("Executioner listening on " + bind_host + ":" +
               std::to_string(port));

      while (running_) {
        try {
#ifndef _WIN32
          reap_dead_children();
#endif

          std::shared_ptr<velix::communication::SocketWrapper> server_socket;
          {
            std::lock_guard<std::mutex> lock(server_mutex_);
            if (!server_socket_ || !server_socket_->is_open())
              break;
            server_socket = std::shared_ptr<velix::communication::SocketWrapper>(
                server_socket_.get(), [](velix::communication::SocketWrapper *) {});
          }

          if (!server_socket->has_data(250)) {
            continue;
          }

          velix::communication::SocketWrapper client;
          client = server_socket->accept();

          auto client_ptr =
              std::make_shared<velix::communication::SocketWrapper>(
                  std::move(client));
          thread_pool_.try_submit([this, client_ptr]() mutable {
            handle_client(std::move(*client_ptr));
          });

        } catch (const std::exception &e) {
          if (!running_)
            break;
          LOG_WARN("Executioner accept error: " + std::string(e.what()));
        }
      }
    } catch (const std::exception &e) {
      running_ = false;
      LOG_ERROR("Executioner startup failed: " + std::string(e.what()));
      throw;
    }
  }

  void stop() {
    running_ = false;
    std::lock_guard<std::mutex> lock(server_mutex_);
    if (server_socket_ && server_socket_->is_open()) {
      server_socket_->close();
    }

#ifndef _WIN32
    reap_dead_children();
#endif
  }

private:
#ifndef _WIN32
  void reap_dead_children() {
    int status = 0;
    while (::waitpid(-1, &status, WNOHANG) > 0) {
    }
  }
#endif

  struct CachedPackage {
    std::string path;
    json manifest;
  };

  bool validate_manifest(const std::string &name,
                         const std::string &folder_name, const json &manifest,
                         std::string &error) {
    // 1. Structural requirements
    if (!manifest.contains("name") || !manifest["name"].is_string()) {
      error = "manifest_missing_name";
      return false;
    }
    if (manifest["name"].get<std::string>() != name) {
      error = "manifest_name_mismatch: " + manifest["name"].get<std::string>() +
              " vs " + name;
      return false;
    }
    if (!manifest.contains("runtime") || !manifest["runtime"].is_string()) {
      error = "manifest_missing_runtime";
      return false;
    }
    if (manifest.contains("prepare") && !manifest["prepare"].is_array()) {
      error = "manifest_prepare_must_be_array";
      return false;
    }
    if (manifest.contains("workdir") && !manifest["workdir"].is_string()) {
      error = "manifest_workdir_must_be_string";
      return false;
    }
    if (manifest.contains("entry") && !manifest["entry"].is_string()) {
      error = "manifest_entry_must_be_string";
      return false;
    }

    if (manifest.contains("run")) {
      if (!manifest["run"].is_object()) {
        error = "manifest_run_must_be_object";
        return false;
      }
      if (manifest["run"].contains("command") &&
          !manifest["run"]["command"].is_string()) {
        error = "manifest_run_command_must_be_string";
        return false;
      }
      if (manifest["run"].contains("args") &&
          !manifest["run"]["args"].is_array()) {
        error = "manifest_run_args_must_be_array";
        return false;
      }
    }

    if (!manifest.contains("run") && !manifest.contains("entry")) {
      error = "manifest_requires_run_or_entry";
      return false;
    }

    (void)folder_name;
    return true;
  }

  bool validate_params(const json &params, const json &schema,
                       std::string &error) {
    if (!schema.is_object())
      return true; // No schema, skip validation

    // Support standard JSON Schema structure
    const json properties = schema.value("properties", schema);
    const json required = schema.value("required", json::array());

    // 1. Check required fields
    for (const auto &req_key : required) {
      if (req_key.is_string()) {
        std::string key = req_key.get<std::string>();
        if (!params.contains(key)) {
          error = "missing_required_param: " + key;
          return false;
        }
      }
    }

    // 2. Validate types for provided properties
    for (auto it = properties.begin(); it != properties.end(); ++it) {
      const std::string &key = it.key();
      const json &spec = it.value();

      if (!params.contains(key))
        continue;
      if (!spec.is_object())
        continue;

      const std::string expected_type = spec.value("type", "string");
      const json &val = params[key];

      bool type_ok = true;
      if (expected_type == "string")
        type_ok = val.is_string();
      else if (expected_type == "number")
        type_ok = val.is_number();
      else if (expected_type == "boolean")
        type_ok = val.is_boolean();
      else if (expected_type == "array")
        type_ok = val.is_array();
      else if (expected_type == "object")
        type_ok = val.is_object();

      if (!type_ok) {
        error = "param_type_mismatch: " + key + " expected " + expected_type;
        return false;
      }
    }
    return true;
  }

  bool should_hash_file(const fs::path &p) {
    if (!fs::is_regular_file(p)) {
      return false;
    }
    const auto rel = p.filename().string();
    if (rel == ".DS_Store") {
      return false;
    }
    if (p.string().find("/.velix/") != std::string::npos) {
      return false;
    }
    return true;
  }

  std::string sha256_of_text(const std::string &text) {
#ifdef _WIN32
    std::size_t h = std::hash<std::string>{}(text);
    std::ostringstream oss;
    oss << std::hex << h;
    return oss.str();
#else
    const auto tmp = fs::temp_directory_path() /
                     ("velix_exec_hash_" + std::to_string(::getpid()) + ".txt");
    {
      std::ofstream out(tmp);
      out << text;
    }
    auto res = velix::utils::ProcessSpawner::run_sync(
        "sha256sum", {tmp.string()});
    fs::remove(tmp);
    if (!res.success || res.stdout_content.empty()) {
      std::size_t h = std::hash<std::string>{}(text);
      std::ostringstream oss;
      oss << std::hex << h;
      return oss.str();
    }
    const auto first_space = res.stdout_content.find(' ');
    return first_space == std::string::npos ? res.stdout_content
                                            : res.stdout_content.substr(0, first_space);
#endif
  }

  std::string compute_cache_key(const json &manifest,
                                const runtime_adapters::RuntimeResolution &runtime,
                                const std::string &pkg_path) {
    std::vector<fs::path> files;
    for (auto const &entry : fs::recursive_directory_iterator(pkg_path)) {
      if (should_hash_file(entry.path())) {
        files.push_back(entry.path());
      }
    }
    std::sort(files.begin(), files.end());

    std::ostringstream source_fingerprint;
    for (const auto &p : files) {
      source_fingerprint << p.lexically_relative(pkg_path).string() << "\n";
      std::ifstream in(p, std::ios::binary);
      source_fingerprint << in.rdbuf() << "\n";
    }

    const std::string canonical =
        manifest.dump() + "\n" + runtime.runtime + "\n" + runtime.version + "\n" +
        source_fingerprint.str();
    return sha256_of_text(canonical);
  }

  std::string resolve_workdir(const std::string &pkg_path,
                              const json &manifest) {
    const std::string rel = manifest.value("workdir", std::string("."));
    fs::path p = fs::path(pkg_path) / rel;
    return p.lexically_normal().string();
  }

  std::string resolve_package_path(const std::string &name,
                                   bool force_refresh) {
    if (name.empty())
      return "";

    // 1. Cache Check
    if (!force_refresh) {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      if (package_cache_.count(name))
        return package_cache_[name].path;
    }

    // 2. Disk Scan
    std::string pkg_path = "";
    std::string folder_name = "";
    if (std::ifstream("skills/" + name + "/manifest.json").good()) {
      pkg_path = "skills/" + name;
      folder_name = name;
    } else if (std::ifstream("agents/" + name + "/manifest.json").good()) {
      pkg_path = "agents/" + name;
      folder_name = name;
    }

    if (!pkg_path.empty()) {
      try {
        std::ifstream f(pkg_path + "/manifest.json");
        json manifest;
        f >> manifest;

        std::string err;
        if (!validate_manifest(name, folder_name, manifest, err)) {
          LOG_ERROR("Manifest Validation Failed for " + name + ": " + err);
          return "";
        }

        std::lock_guard<std::mutex> lock(cache_mutex_);
        package_cache_[name] = {pkg_path, manifest};
      } catch (const std::exception &e) {
        LOG_ERROR("Failed to load manifest for " + name + ": " +
                  std::string(e.what()));
        return "";
      }
    }

    return pkg_path;
  }

  json handle_cache_clean(const std::string &trace_id) {
    try {
      if (fs::exists(config_.cache_root)) {
        fs::remove_all(config_.cache_root);
      }
      fs::create_directories(config_.cache_root);
      return {{"status", "ok"}, {"trace_id", trace_id}, {"phase", "cache"},
              {"action", "clean"}};
    } catch (const std::exception &e) {
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"phase", "cache"},
              {"action", "clean"},
              {"error", std::string("cache_clean_failed: ") + e.what()}};
    }
  }

  json handle_cache_prune(const json &request, const std::string &trace_id) {
    const int older_than_sec = request.value("older_than_sec", 7 * 24 * 3600);
    int removed = 0;
    try {
      if (!fs::exists(config_.cache_root)) {
        return {{"status", "ok"},
                {"trace_id", trace_id},
                {"phase", "cache"},
                {"action", "prune"},
                {"removed", 0}};
      }

      const auto now = fs::file_time_type::clock::now();
      for (const auto &entry : fs::directory_iterator(config_.cache_root)) {
        if (!entry.is_directory()) {
          continue;
        }
        const auto ts = fs::last_write_time(entry.path());
        const auto age = now - ts;
        if (age > std::chrono::seconds(older_than_sec)) {
          fs::remove_all(entry.path());
          ++removed;
        }
      }

      return {{"status", "ok"},
              {"trace_id", trace_id},
              {"phase", "cache"},
              {"action", "prune"},
              {"removed", removed}};
    } catch (const std::exception &e) {
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"phase", "cache"},
              {"action", "prune"},
              {"removed", removed},
              {"error", std::string("cache_prune_failed: ") + e.what()}};
    }
  }

  json handle_exec_request(const json &request) {
    const std::string msg_type = request.value("message_type", "");
    const std::string trace_id = request.value("trace_id", "");
    const std::string tree_id = request.value("tree_id", "");
    const std::string intent = request.value("intent", "JOIN_PARENT_TREE");
    const bool is_handler = request.value("is_handler", false);
    const bool force_refresh = request.value("force_refresh", false);

    if (msg_type == "EXEC_CACHE_CLEAN") {
      return handle_cache_clean(trace_id);
    }
    if (msg_type == "EXEC_CACHE_PRUNE") {
      return handle_cache_prune(request, trace_id);
    }

    if (msg_type != "EXEC_VELIX_PROCESS") {
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"error", "Unsupported message type. Use EXEC_VELIX_PROCESS"}};
    }

    if (intent != "JOIN_PARENT_TREE" && intent != "NEW_TREE") {
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"error", "invalid_intent: must be JOIN_PARENT_TREE or NEW_TREE"}};
    }

    if (intent == "JOIN_PARENT_TREE" && request.value("source_pid", -1) <= 0) {
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"error", "invalid_source_pid_for_join_parent_tree"}};
    }

    if (intent == "NEW_TREE" && !is_handler) {
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"error", "intent_override_new_tree_allowed_only_for_handler"}};
    }

    std::string package_name = request.value("name", "");
    json params = request.value("params", json::object());

    if (package_name.empty()) {
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"error", "empty_package_name"}};
    }

    // 1. Resolve & Validate Package (Cache-Aware)
    std::string pkg_path = resolve_package_path(package_name, force_refresh);
    if (pkg_path.empty()) {
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"error", "package_not_found_or_invalid: " + package_name}};
    }

    json manifest;
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      manifest = package_cache_[package_name].manifest;
    }

    // 2. Structural Param Validation (Standardized OpenAI-style)
    std::string param_err;
    if (!validate_params(params, manifest.value("parameters", json::object()),
                         param_err)) {
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"error", "params_validation_failed: " + param_err}};
    }

        // 3. Resolve entrypoint workdir
        const std::string workdir = resolve_workdir(pkg_path, manifest);
        if (!fs::exists(workdir) || !fs::is_directory(workdir)) {
          return {{"status", "error"},
            {"trace_id", trace_id},
            {"phase", "prepare"},
            {"error", "workdir_not_found: " + workdir}};
        }

        // 4. Select runtime adapter (runtime policy + entry validation)
        runtime_adapters::RuntimeResolution runtime;
        std::string runtime_error;
        if (!runtime_adapters::select_runtime_adapter(manifest, pkg_path, workdir,
                  runtime, runtime_error)) {
          return {{"status", "error"},
            {"trace_id", trace_id},
            {"phase", "adapter"},
            {"error", runtime_error}};
        }

        // 5. Parse prepare pipeline
    std::vector<prepare_runner::PrepareStep> prepare_steps;
    std::string prepare_parse_error;
    if (!prepare_runner::parse_prepare_steps(manifest, config_.prepare_timeout_ms,
                                             prepare_steps, prepare_parse_error)) {
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"phase", "prepare"},
              {"error", prepare_parse_error}};
    }

    if (!runtime.injected_prepare_steps.empty()) {
      std::vector<prepare_runner::PrepareStep> merged;
      merged.reserve(runtime.injected_prepare_steps.size() + prepare_steps.size());
      for (const auto &step : runtime.injected_prepare_steps) {
        merged.push_back(
            prepare_runner::PrepareStep{step.command, step.args, step.timeout_ms});
      }
      for (const auto &step : prepare_steps) {
        merged.push_back(step);
      }
      prepare_steps.swap(merged);
    }

    // 6. Build Environment Context
    const std::string bus_port =
        std::to_string(velix::utils::get_port("BUS", 5174));
    const int requested_source_pid = request.value("source_pid", -1);
    if (requested_source_pid <= 0) {
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"error", "invalid_source_pid"}};
    }
    const bool join_parent_tree = (intent == "JOIN_PARENT_TREE");
    const std::string parent_pid = std::to_string(requested_source_pid);
    const std::string env_tree_id = join_parent_tree ? tree_id : "";

    fs::path workspace_root = fs::current_path();
    {
      const fs::path pkg = fs::path(pkg_path);
      if (!pkg.empty()) {
        const fs::path candidate = pkg.parent_path().parent_path();
        if (!candidate.empty()) {
          workspace_root = fs::absolute(candidate);
        }
      }
    }
  #ifdef _WIN32
    const char py_sep = ';';
  #else
    const char py_sep = ':';
  #endif
    std::string pythonpath = workspace_root.string();
    if (const char *existing_pythonpath = std::getenv("PYTHONPATH");
      existing_pythonpath != nullptr && *existing_pythonpath != '\0') {
      pythonpath += py_sep;
      pythonpath += existing_pythonpath;
    }

    std::map<std::string, std::string> env = {
        {"VELIX_PARENT_PID", parent_pid}, {"VELIX_BUS_PORT", bus_port},
        {"VELIX_INTENT", intent},
        {"VELIX_TRACE_ID", trace_id},     {"VELIX_PROCESS_NAME", package_name},
      {"VELIX_TREE_ID", env_tree_id},   {"VELIX_PARAMS", params.dump()},
        {"PYTHONPATH", pythonpath}
    };
    const std::string user_id = request.value("user_id", "");
    env["VELIX_USER_ID"] = user_id;
    for (const auto &[k, v] : runtime.env_overrides) {
      env[k] = v;
    }

    // 7. Prepare cache with file lock
    const std::string cache_key = compute_cache_key(manifest, runtime, pkg_path);
    const fs::path cache_dir = fs::path(config_.cache_root) / cache_key;
    const fs::path lock_file = cache_dir / ".lock";
    const fs::path status_file = cache_dir / "status.json";
    const fs::path logs_dir = cache_dir / "logs";
    const fs::path prepare_log = logs_dir / "prepare.log";
    try {
      fs::create_directories(cache_dir);
      fs::create_directories(logs_dir);
      FileLockGuard lock(lock_file.string());

      bool cache_ready = false;
      if (!force_refresh && fs::exists(status_file)) {
        try {
          std::ifstream in(status_file.string());
          json status;
          in >> status;
          cache_ready = status.value("status", "") == "ready";
        } catch (...) {
          cache_ready = false;
        }
      }

      if (!cache_ready) {
        json status = {{"status", "preparing"}};
        {
          std::ofstream out(status_file.string());
          out << status.dump(2);
        }

        const json prep = prepare_runner::execute_prepare(
            prepare_steps, env, workdir, trace_id, prepare_log.string());
        if (prep.value("status", "error") != "ok") {
          std::ofstream out(status_file.string());
          out << json({{"status", "failed"},
                       {"error", prep.value("error", "prepare_failed")},
                       {"log_file", prepare_log.string()}})
                     .dump(2);
          return prep;
        }

        std::ofstream out(status_file.string());
        out << json({{"status", "ready"}}).dump(2);
      }
    } catch (const std::exception &e) {
      return {{"status", "error"},
              {"trace_id", trace_id},
              {"phase", "prepare"},
              {"error", std::string("prepare_cache_failed: ") + e.what()}};
    }

    // 8. Atomic Handoff (Non-Blocking) with Diagnostics
    LOG_INFO("Launching Package: " + package_name + " [WD: " + workdir + "]");

    return run_launcher::launch(runtime, env, workdir, trace_id);
  }

  void handle_client(velix::communication::SocketWrapper client_sock) {
    try {
      const std::string request_raw =
          velix::communication::recv_json(client_sock);
      if (request_raw.empty())
        return;
      const json request = json::parse(request_raw);
      const json response = handle_exec_request(request);
      velix::communication::send_json(client_sock, response.dump());
    } catch (const std::exception &e) {
      LOG_WARN("Executioner client handle failed: " + std::string(e.what()));
    }
  }

  std::atomic<bool> running_;
  ExecutionerConfig config_;
  std::mutex server_mutex_;
  std::mutex cache_mutex_;
  std::unique_ptr<velix::communication::SocketWrapper> server_socket_;
  velix::utils::ThreadPool thread_pool_;
  std::unordered_map<std::string, CachedPackage> package_cache_;
};

ExecutionerService &executioner_service() {
  static ExecutionerService service;
  return service;
}

} // namespace

void start_executioner(int port = 5172) { executioner_service().start(port); }
void stop_executioner() { executioner_service().stop(); }

} // namespace velix::core
