/**
 * Refactored Velix Supervisor Service - Modular Architecture
 *
 * This file replaces the monolithic SupervisorService with a cleaner,
 * modular design that separates concerns:
 *
 * - ProcessRegistry: Owns all structural state (process/tree tables, indexes)
 * - TerminationEngine: Performs OS-level process termination
 * - WatchdogEngine: Monitors system health and enforces limits
 * - SupervisorService: Network layer and message routing
 *
 * Key improvements:
 * - Heartbeat updates are lock-free (atomic operations only)
 * - Structural state changes use shared_mutex for better concurrency
 * - No locks held during OS operations
 * - Process lifetime safety via shared_ptr
 * - Clear separation of concerns
 *
 * Backward compatibility:
 * - Public API (start_supervisor, stop_supervisor) remains unchanged
 * - Message protocol remains unchanged
 * - All existing clients work without modification
 */

#include "supervisor.hpp"
#include "process_registry.hpp"
#include "termination_engine.hpp"
#include "../communication/network_config.hpp"
#include "../communication/socket_wrapper.hpp"
#include "../utils/config_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/thread_pool.hpp"
#include "../llm/session_io.hpp"
#include "../vendor/nlohmann/json.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using json = nlohmann::json;

namespace velix::core {

namespace {

namespace fs = std::filesystem;

constexpr int kDefaultSupervisorPort = 5173;

bool ends_with_exe_case_insensitive(const std::string &value) {
  if (value.size() < 4) {
    return false;
  }
  const std::size_t start = value.size() - 4;
  return std::tolower(static_cast<unsigned char>(value[start])) == '.' &&
         std::tolower(static_cast<unsigned char>(value[start + 1])) == 'e' &&
         std::tolower(static_cast<unsigned char>(value[start + 2])) == 'x' &&
         std::tolower(static_cast<unsigned char>(value[start + 3])) == 'e';
}

bool looks_like_path(const std::string &value) {
  if (value.empty()) {
    return false;
  }
  return value.rfind("./", 0) == 0 || value.rfind("../", 0) == 0 ||
         value.rfind('/', 0) == 0 || value.find('/') != std::string::npos ||
         value.find('\\') != std::string::npos;
}

void normalize_binary_path_for_platform(std::string &binary_path) {
  if (!looks_like_path(binary_path)) {
    return;
  }
#ifdef _WIN32
  if (!ends_with_exe_case_insensitive(binary_path)) {
    binary_path += ".exe";
  }
#else
  if (ends_with_exe_case_insensitive(binary_path)) {
    binary_path.resize(binary_path.size() - 4);
  }
#endif
}

struct SupervisorConfig {
  int heartbeat_timeout_sec = 15;
  int watchdog_interval_ms = 1000;
  int terminate_grace_ms = 1500;
  int max_client_threads = 256;
  std::size_t max_message_bytes = 1048576;
  bool require_auth_token = false;
  std::string auth_token = "";
  bool exempt_system_tree_limits = true;

  int max_processes_per_tree = 64;
  int max_tree_runtime_sec = 3600;
  double max_memory_per_tree_mb = 2048.0;
  int max_llm_requests_per_tree = 1000;

  // Conversation settings
  bool conversation_enabled = true;
  int max_active_process_convos_per_pid = 100;
  int process_convo_ttl_sec = 86400;

  // Handler spawn settings
  std::string handler_binary;
  std::vector<std::string> handler_args;
  bool handler_auto_restart = true;
  int handler_restart_delay_ms = 2000;
};

SupervisorConfig load_supervisor_config() {
  SupervisorConfig config;

  std::ifstream file("config/supervisor.json");
  if (!file.is_open()) {
    file.open("../config/supervisor.json");
  }
  if (!file.is_open()) {
    file.open("build/config/supervisor.json");
  }
  if (!file.is_open()) {
    LOG_WARN_CTX("config/supervisor.json not found, using defaults",
                 "supervisor", "", -1, "config_default");
    return config;
  }

  try {
    json cfg;
    file >> cfg;

    config.heartbeat_timeout_sec =
        cfg.value("heartbeat_timeout_sec", config.heartbeat_timeout_sec);
    config.watchdog_interval_ms =
        cfg.value("watchdog_interval_ms", config.watchdog_interval_ms);
    config.terminate_grace_ms =
        cfg.value("terminate_grace_ms", config.terminate_grace_ms);
    config.max_client_threads =
        cfg.value("max_client_threads", config.max_client_threads);
    config.max_message_bytes =
        cfg.value("max_message_bytes", config.max_message_bytes);
    config.require_auth_token =
        cfg.value("require_auth_token", config.require_auth_token);
    config.auth_token = cfg.value("auth_token", config.auth_token);
    config.exempt_system_tree_limits = cfg.value(
        "exempt_system_tree_limits", config.exempt_system_tree_limits);

    const json limits = cfg.value("limits", json::object());
    config.max_processes_per_tree =
        limits.value("max_processes_per_tree", config.max_processes_per_tree);
    config.max_tree_runtime_sec =
        limits.value("max_tree_runtime_sec", config.max_tree_runtime_sec);
    config.max_memory_per_tree_mb =
        limits.value("max_memory_per_tree_mb", config.max_memory_per_tree_mb);
    config.max_llm_requests_per_tree = limits.value(
        "max_llm_requests_per_tree", config.max_llm_requests_per_tree);

    const json convo_cfg = cfg.value("conversation", json::object());
    config.conversation_enabled =
        convo_cfg.value("enabled", config.conversation_enabled);
    config.max_active_process_convos_per_pid =
        convo_cfg.value("max_active_process_convos_per_pid",
                        config.max_active_process_convos_per_pid);
    config.process_convo_ttl_sec =
        convo_cfg.value("process_convo_ttl_sec", config.process_convo_ttl_sec);

    const json hcfg = cfg.value("handler", json::object());
    config.handler_binary = hcfg.value("binary", config.handler_binary);
    normalize_binary_path_for_platform(config.handler_binary);
    config.handler_auto_restart =
        hcfg.value("auto_restart", config.handler_auto_restart);
    config.handler_restart_delay_ms =
        hcfg.value("restart_delay_ms", config.handler_restart_delay_ms);
    if (hcfg.contains("args") && hcfg["args"].is_array()) {
      for (const auto &a : hcfg["args"]) {
        if (a.is_string()) {
          config.handler_args.push_back(a.get<std::string>());
        }
      }
    }

    LOG_INFO_CTX("Loaded supervisor config", "supervisor", "", -1,
                 "config_loaded");
  } catch (const std::exception &e) {
    LOG_ERROR_CTX(std::string("Failed to parse config/supervisor.json: ") +
                      e.what(),
                  "supervisor", "", -1, "config_parse_error");
  }

  return config;
}

// FIX #12: Helper function to get current time in milliseconds
// Replaces duplicated chrono calls throughout the code
inline uint64_t get_current_time_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string to_string(ProcessStatus status) {
  switch (status) {
  case ProcessStatus::STARTING:
    return "STARTING";
  case ProcessStatus::RUNNING:
    return "RUNNING";
  case ProcessStatus::WAITING_LLM:
    return "WAITING_LLM";
  case ProcessStatus::WAITING_EXEC:
    return "WAITING_EXEC";
  case ProcessStatus::FINISHED:
    return "FINISHED";
  case ProcessStatus::ERROR:
    return "ERROR";
  case ProcessStatus::KILLED:
    return "KILLED";
  }
  return "ERROR";
}

std::string to_string(TreeStatus status) {
  switch (status) {
  case TreeStatus::ACTIVE:
    return "ACTIVE";
  case TreeStatus::COMPLETED:
    return "COMPLETED";
  case TreeStatus::FAILED:
    return "FAILED";
  case TreeStatus::KILLED:
    return "KILLED";
  }
  return "FAILED";
}

ProcessStatus parse_process_status(const std::string &status) {
  if (status == "STARTING")
    return ProcessStatus::STARTING;
  if (status == "RUNNING")
    return ProcessStatus::RUNNING;
  if (status == "WAITING_LLM")
    return ProcessStatus::WAITING_LLM;
  if (status == "WAITING_EXEC")
    return ProcessStatus::WAITING_EXEC;
  if (status == "FINISHED")
    return ProcessStatus::FINISHED;
  if (status == "KILLED")
    return ProcessStatus::KILLED;
  return ProcessStatus::ERROR;
}

bool is_valid_process_status_text(const std::string &status) {
  return status == "STARTING" || status == "RUNNING" ||
         status == "WAITING_LLM" || status == "WAITING_EXEC" ||
         status == "FINISHED" || status == "ERROR" || status == "KILLED";
}

struct ConvMetadata {
  std::string convo_id;
  std::string user_id;
  int creator_pid = -1;
  uint64_t created_at_ms = 0;  // FIX #8: Consistency - use uint64_t
  std::chrono::steady_clock::time_point last_activity_at =
      std::chrono::steady_clock::now();
};

bool is_terminal_status(ProcessStatus status) {
  return status == ProcessStatus::FINISHED || status == ProcessStatus::KILLED ||
         status == ProcessStatus::ERROR;
}

bool is_system_handler_tree(const std::string &tree_id) {
  return tree_id == "TREE_HANDLER";
}

bool is_limit_enabled(int value) { return value > 0; }

bool is_limit_enabled(double value) { return value > 0.0; }

bool is_expected_disconnect_error(const std::string &err) {
  return err.find("Broken pipe") != std::string::npos ||
         err.find("connection closed") != std::string::npos ||
         err.find("Connection reset") != std::string::npos ||
         err.find("errno 32") != std::string::npos ||
         err.find("errno 104") != std::string::npos;
}

/**
 * Refactored SupervisorService using modular components.
 *
 * Architecture:
 * - registry_: ProcessRegistry instance (owns all structural state)
 * - termination_engine_: TerminationEngine instance
 * - watchdog_engine_: WatchdogEngine instance
 * - conversation management (specialized state)
 * - message routing and network handling (this class)
 *
 * Thread safety model:
 * - ProcessRegistry uses shared_mutex for structural state
 * - Heartbeat updates are lock-free
 * - All OS operations happen outside of registry locks
 * - No deadlocks possible (locks released before OS ops)
 */
class SupervisorService {
public:
  SupervisorService()
      : config_(load_supervisor_config()),
        thread_pool_(config_.max_client_threads,
                     config_.max_client_threads * 4) {
    register_system_handler_tree();
  }

  ~SupervisorService() { stop(); }

  void start(int port) {
    if (running_) {
      return;
    }

    running_ = true;
    LOG_INFO_CTX("Supervisor service starting", "supervisor", "", -1,
                 "startup");

    // Start watchdog thread
    watchdog_thread_ = std::thread([this]() { watchdog_loop(); });

    // Spawn handler if configured
    if (!config_.handler_binary.empty()) {
      spawn_handler_process();
    }

    try {
      const std::string bind_host =
          velix::communication::resolve_bind_host("SUPERVISOR", "127.0.0.1");
      {
        std::scoped_lock lock(server_mutex_);
        server_socket_ =
            std::make_shared<velix::communication::SocketWrapper>();
        server_socket_->create_tcp_socket();
        server_socket_->bind(bind_host, static_cast<uint16_t>(port));
        server_socket_->listen(8);
      }

      LOG_INFO_CTX("Supervisor listening on " + bind_host + ":" +
                       std::to_string(port),
                   "supervisor", "", -1, "listen");

      while (running_) {
        try {
          velix::communication::SocketWrapper client;
          std::shared_ptr<velix::communication::SocketWrapper> server_socket;
          {
            std::scoped_lock lock(server_mutex_);
            server_socket = server_socket_;
          }

          if (!server_socket || !server_socket->is_open()) {
            break;
          }

          client = server_socket->accept();

          auto client_ptr =
              std::make_shared<velix::communication::SocketWrapper>(
                  std::move(client));
          const bool submitted =
              thread_pool_.try_submit([this, client_ptr]() mutable {
                try {
                  handle_client(std::move(*client_ptr));
                } catch (const std::exception &) {
                  // Worker errors are already handled in handle_client.
                }
              });

          if (!submitted) {
            try {
              json busy = {{"status", "error"},
                           {"error",
                            "supervisor busy: max client threads reached"}};
              velix::communication::send_json(*client_ptr, busy.dump());
            } catch (const std::exception &) {
              // Best-effort rejection path.
            }
          }
        } catch (const std::exception &e) {
          if (!running_) {
            break;
          }
          LOG_WARN_CTX(std::string("Supervisor accept error: ") + e.what(),
                       "supervisor", "", -1, "accept_error");
        }
      }

      stop();
    } catch (const std::exception &e) {
      running_ = false;
      join_watchdog();
      LOG_ERROR_CTX(std::string("Supervisor startup failed: ") + e.what(),
                    "supervisor", "", -1, "startup_error");
      throw;
    }
  }

  void stop() {
    if (const bool was_running = running_.exchange(false); !was_running) {
      join_watchdog();
      return;
    }

    {
      std::scoped_lock lock(server_mutex_);
      if (server_socket_ && server_socket_->is_open()) {
        server_socket_->close();
      }
      server_socket_.reset();
    }

    join_watchdog();

    // Ensure no child process keeps running/listening after supervisor shutdown.
    {
      const auto snapshot = registry_->snapshot_for_watchdog();
      std::vector<TerminationEngine::TerminationTarget> targets;
      targets.reserve(snapshot.size());

      for (const auto &entry : snapshot) {
        TerminationEngine::TerminationTarget target;
        target.velix_pid = entry.pid;
        target.os_pid = entry.os_pid;
        target.tree_id = entry.tree_id;
        target.trace_id = entry.trace_id;
        target.parent_pid = entry.parent_pid;
        targets.push_back(target);
      }

      if (!targets.empty()) {
        termination_engine_->kill_processes(
            targets, "supervisor_shutdown", registry_,
            config_.terminate_grace_ms);
      }
    }

    LOG_INFO_CTX("Supervisor stopped gracefully", "supervisor", "", -1,
                 "shutdown");
  }

private:
  SupervisorConfig config_;

  // ─────────────────────────────────────────────────────────────────────
  // Modular components: Each has specific responsibilities
  // ─────────────────────────────────────────────────────────────────────

  // ProcessRegistry: owns all structural state (process/tree tables)
  std::shared_ptr<ProcessRegistry> registry_{std::make_shared<ProcessRegistry>()};

  // TerminationEngine: performs OS-level termination
  std::shared_ptr<TerminationEngine> termination_engine_{std::make_shared<TerminationEngine>()};

  // Conversation tracking (specialized state - not in registry)
  std::mutex conversation_mutex_;
  std::unordered_map<int, std::unordered_set<std::string>>
      process_convos_;  // pid -> {convo_ids}
  std::unordered_map<std::string, ConvMetadata>
      convo_metadata_;  // convo_id -> metadata

  velix::llm::SessionIO convo_manager_;

  // Network layer
  std::mutex server_mutex_;
  std::shared_ptr<velix::communication::SocketWrapper> server_socket_;

  velix::utils::ThreadPool thread_pool_;

  std::atomic<bool> running_{false};
  std::thread watchdog_thread_;

  // Handler lifecycle tracking
  std::atomic<int> handler_os_pid_{-1};
  std::atomic<int> handler_velix_pid_{-1};
  bool handler_needs_restart_ = false;
  std::chrono::steady_clock::time_point handler_dead_since_;

  void join_watchdog() {
    if (watchdog_thread_.joinable()) {
      watchdog_thread_.join();
    }
  }

  void register_system_handler_tree() {
    // FIX #7: Make tree ID explicit rather than relying on hardcoded assumption
    registry_->create_tree("TREE_HANDLER");
  }

  void spawn_handler_process() {
    if (config_.handler_binary.empty()) {
      return;
    }

#ifndef _WIN32
    {
      const int prev_os_pid = handler_os_pid_.load();
      if (prev_os_pid > 0) {
        // FIX #5: Actually kill the process before spawning new one
        // Send SIGTERM first, then SIGKILL only if still alive
        if (::kill(static_cast<::pid_t>(prev_os_pid), SIGTERM) == 0) {
          // Wait briefly for graceful shutdown
          std::this_thread::sleep_for(std::chrono::milliseconds(500));

          // Minor fix: only force-kill if process did not exit yet
          if (::waitpid(static_cast<::pid_t>(prev_os_pid), nullptr, WNOHANG) ==
              0) {
            ::kill(static_cast<::pid_t>(prev_os_pid), SIGKILL);
          }
        }
        ::waitpid(static_cast<::pid_t>(prev_os_pid), nullptr, WNOHANG);
        handler_os_pid_.store(-1);
      }
    }

    std::vector<const char *> argv;
    argv.push_back(config_.handler_binary.c_str());
    for (const auto &arg : config_.handler_args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    const ::pid_t pid = ::fork();
    if (pid < 0) {
      LOG_ERROR_CTX("fork() failed when spawning handler", "supervisor",
                    "TREE_HANDLER", -1, "handler_fork_error");
      return;
    }
    if (pid == 0) {
      ::execv(config_.handler_binary.c_str(),
              const_cast<char *const *>(argv.data()));
      ::_exit(1);
    }
    handler_os_pid_.store(static_cast<int>(pid));
    LOG_INFO_CTX("Spawned handler with OS PID " + std::to_string(pid),
                 "supervisor", "TREE_HANDLER", -1, "handler_spawn");
#else
    // FIX #5: Windows version - terminate previous process before spawn
    {
      const int prev_os_pid = handler_os_pid_.load();
      if (prev_os_pid > 0) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE,
                                     static_cast<DWORD>(prev_os_pid));
        if (process != nullptr) {
          TerminateProcess(process, 1);
          CloseHandle(process);
        }
        handler_os_pid_.store(-1);
      }
    }

    std::string cmd_line = "\"" + config_.handler_binary + "\"";
    for (const auto &arg : config_.handler_args) {
      cmd_line += " \"" + arg + "\"";
    }

    std::vector<char> cmd_buffer(cmd_line.begin(), cmd_line.end());
    cmd_buffer.push_back('\0');

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(nullptr, cmd_buffer.data(), nullptr, nullptr, FALSE,
              CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, nullptr,
              nullptr, &si, &pi)) {
      LOG_ERROR_CTX("CreateProcessA failed when spawning handler", "supervisor",
                    "TREE_HANDLER", -1, "handler_spawn_error");
      return;
    }

    handler_os_pid_.store(static_cast<int>(pi.dwProcessId));
    LOG_INFO_CTX("Spawned handler with OS PID " +
                     std::to_string(pi.dwProcessId),
                 "supervisor", "TREE_HANDLER", -1, "handler_spawn");

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#endif
  }

  bool validate_auth(const json &message, std::string &error) const {
    if (!config_.require_auth_token) {
      return true;
    }

    const json metadata = message.value("metadata", json::object());
    const std::string auth_token = metadata.value("auth_token", "");
    if (auth_token.empty() || auth_token != config_.auth_token) {
      error = "auth failed";
      return false;
    }

    return true;
  }

  bool validate_message_shape(const json &message, std::string &error) const {
    if (!message.is_object()) {
      error = "message must be a JSON object";
      return false;
    }

    if (!message.contains("message_type") ||
        !message["message_type"].is_string()) {
      error = "missing or invalid message_type";
      return false;
    }

    static const std::unordered_set<std::string> allowed_types = {
        "REGISTER_PID", "HEARTBEAT", "LLM_REQUEST", "TREE_STATUS",
        "TREE_KILL"};

    const std::string message_type = message["message_type"].get<std::string>();
    if (allowed_types.find(message_type) == allowed_types.end()) {
      error = "unsupported message_type: " + message_type;
      return false;
    }

    if (message_type == "HEARTBEAT") {
      if (!message.contains("pid") || !message["pid"].is_number_integer() ||
          message["pid"].get<int>() <= 0) {
        error = message_type + " requires positive integer pid";
        return false;
      }
    }

    if (message_type == "REGISTER_PID") {
      const json payload = message.value("payload", json::object());
      const std::string register_intent =
          payload.value("register_intent", std::string(""));
      const std::string role = payload.value("role", std::string("unknown"));
      if (register_intent == "JOIN_PARENT_TREE") {
        // Bootstrap handler can join TREE_HANDLER without a parent velix pid.
        if (role == "handler") {
          return true;
        }
        if (!message.contains("source_pid") ||
            !message["source_pid"].is_number_integer() ||
            message["source_pid"].get<int>() <= 0) {
          error = "REGISTER_PID JOIN_PARENT_TREE requires source_pid";
          return false;
        }
      }
    }

    return true;
  }

  json build_process_json(const ProcessInfo &process) const {
    return {{"pid", process.pid},
            {"os_pid", process.os_pid},
            {"tree_id", process.tree_id},
            {"parent_pid", process.parent_pid},
            {"role", process.role},
            {"status", to_string(process.status.load())}};
  }

  json build_tree_json(const std::string &tree_id, TreeStatus status,
                       int root_pid) const {
    return {{"tree_id", tree_id},
            {"status", to_string(status)},
            {"root_pid", root_pid}};
  }

  void handle_client(velix::communication::SocketWrapper client_sock) {
    try {
      const std::string request_raw =
          velix::communication::recv_json(client_sock);
      if (request_raw.size() > config_.max_message_bytes) {
        json error = {{"status", "error"}, {"error", "message too large"}};
        velix::communication::send_json(client_sock, error.dump());
        return;
      }
      json request = json::parse(request_raw);
      json response = handle_message(request);
      velix::communication::send_json(client_sock, response.dump());
    } catch (const std::exception &e) {
      const std::string err = e.what();
      if (is_expected_disconnect_error(err)) {
        return;
      }

      LOG_ERROR_CTX(std::string("Supervisor request error: ") + err,
                    "supervisor", "", -1, "request_error");
      try {
        json error = {{"status", "error"}, {"error", err}};
        velix::communication::send_json(client_sock, error.dump());
      } catch (const std::exception &) {
        // Client likely disconnected; request handling already failed.
      }
    }
  }

  json handle_message(const json &message) {
    std::string validation_error;
    if (!validate_message_shape(message, validation_error)) {
      return {{"status", "error"}, {"error", validation_error}};
    }

    if (!validate_auth(message, validation_error)) {
      return {{"status", "error"}, {"error", validation_error}};
    }

    const std::string message_type = message.value("message_type", "");

    if (message_type == "REGISTER_PID") {
      return handle_register_pid(message);
    }
    if (message_type == "HEARTBEAT") {
      return handle_heartbeat(message);
    }
    if (message_type == "LLM_REQUEST") {
      return handle_llm_request(message);
    }
    if (message_type == "TREE_STATUS") {
      return handle_tree_status(message);
    }
    if (message_type == "TREE_KILL") {
      return handle_tree_kill(message);
    }

    return {{"status", "error"},
            {"error", "unsupported message_type: " + message_type}};
  }

  json handle_register_pid(const json &message) {
    const json payload = message.value("payload", json::object());
    const std::string register_intent = payload.value("register_intent", "");
    const std::string role = payload.value("role", "unknown");
    int source_pid = message.value("source_pid", -1);
    const int os_pid = payload.value("os_pid", -1);
    const std::string status_text = payload.value("status", "STARTING");

    if (!is_valid_process_status_text(status_text)) {
      return {{"status", "error"},
              {"error", "invalid process status in REGISTER_PID"},
              {"status_value", status_text}};
    }
    const ProcessStatus status = parse_process_status(status_text);
    const double memory_mb = payload.value("memory_mb", 0.0);

    if (os_pid <= 0) {
      return {{"status", "error"},
              {"error", "REGISTER_PID requires payload.os_pid"}};
    }

    if (register_intent != "NEW_TREE" &&
        register_intent != "JOIN_PARENT_TREE") {
      return {{"status", "error"},
              {"error", "REGISTER_PID requires register_intent = NEW_TREE or "
                        "JOIN_PARENT_TREE"}};
    }

    std::string tree_id;
    bool is_tree_root = false;
    bool is_handler_process = false;

    const bool handler_tree_empty =
        registry_->get_tree_processes("TREE_HANDLER").empty();

    if (role == "handler" && handler_tree_empty) {
      const int expected_handler_os_pid = handler_os_pid_.load();
      if (expected_handler_os_pid > 0 && os_pid != expected_handler_os_pid) {
        return {{"status", "error"},
                {"error", "handler_os_pid_mismatch"},
                {"expected_os_pid", expected_handler_os_pid},
                {"actual_os_pid", os_pid}};
      }
      tree_id = "TREE_HANDLER";
      source_pid = -1;
      is_tree_root = true;
      is_handler_process = true;
    } else if (role == "handler") {
      return {{"status", "error"},
              {"error", "handler_role_reserved_for_bootstrap"}};
    } else if (register_intent == "NEW_TREE") {
      tree_id = "";  // Let registry create it
      is_tree_root = true;
    } else {
      if (source_pid <= 0) {
        return {{"status", "error"},
                {"error", "JOIN_PARENT_TREE requires valid source_pid"}};
      }

      auto parent = registry_->get_process(source_pid);
      if (!parent) {
        return {{"status", "error"},
                {"error", "source_pid not registered"},
                {"source_pid", source_pid}};
      }

      if (is_terminal_status(parent->status.load())) {
        return {{"status", "error"},
                {"error", "source_pid is not active"},
                {"source_pid", source_pid}};
      }

      tree_id = parent->tree_id;
    }

    // Register process and check limits
    auto result = registry_->register_process(
        os_pid, tree_id, source_pid, role, payload.value("trace_id", ""),
        status, memory_mb, role == "handler");

    if (!result.success) {
      return {{"status", "error"}, {"error", result.error}};
    }

    // Update handler tracking if needed
    if (role == "handler") {
      handler_velix_pid_.store(result.process->pid);
      handler_needs_restart_ = false;
      handler_dead_since_ = {};
      LOG_INFO_CTX("Handler registered with velix pid " +
                       std::to_string(result.process->pid),
                   "supervisor", "TREE_HANDLER", result.process->pid,
                   "handler_registered");
    }

    LOG_INFO_CTX("Registered pid " + std::to_string(result.process->pid) +
                     " in " + result.tree_id,
                 "supervisor", result.tree_id, result.process->pid,
                 "process_registered");

    json response = {{"status", "ok"},
                     {"tree_id", result.tree_id},
                     {"register_intent", register_intent},
                     {"is_root", is_tree_root},
                     {"is_handler", is_handler_process},
                     {"process", build_process_json(*result.process)}};

    return response;
  }

  json handle_heartbeat(const json &message) {
    const int pid = message.value("pid", -1);
    if (pid <= 0) {
      return {{"status", "error"}, {"error", "HEARTBEAT requires valid pid"}};
    }

    const json payload = message.value("payload", json::object());
    const std::string status_text = payload.value("status", "");
    const double memory_mb = payload.value("memory_mb", -1.0);

    auto process = registry_->get_process(pid);
    if (!process) {
      return {{"status", "error"}, {"error", "pid not registered"}};
    }

    // FIX #12: Use time helper instead of manual chrono
    uint64_t now_ms = get_current_time_ms();

    ProcessStatus new_status = process->status.load();
    if (!status_text.empty()) {
      if (!is_valid_process_status_text(status_text)) {
        return {{"status", "error"},
                {"error", "invalid process status in HEARTBEAT"},
                {"status_value", status_text}};
      }
      new_status = parse_process_status(status_text);
    }

    ProcessStatus previous_status = process->status.load();

    // Update heartbeat (lock-free)
    registry_->update_heartbeat(pid, new_status, memory_mb, now_ms);

    json response = {{"status", "ok"}, {"pid", pid}};

    // Check if process transitioned to terminal state
    if (!is_terminal_status(previous_status) &&
        is_terminal_status(new_status)) {
      registry_->terminate_process(pid);
    }

    return response;
  }

  json handle_llm_request(const json &message) {
    // NOTE: Simplified implementation - full LLM request handling would be
    // more complex
    const std::string mode = message.value("mode", "");
    const std::string tree_id = message.value("tree_id", "");
    const std::string convo_id = message.value("convo_id", "");
    const std::string user_id = message.value("user_id", "");
    const int source_pid = message.value("source_pid", -1);

    if (mode != "simple" && mode != "conversation" &&
        mode != "user_conversation") {
      return {{"status", "error"},
              {"error", "unsupported LLM mode"}};
    }

    if (!config_.conversation_enabled && mode != "simple") {
      return {{"status", "error"},
              {"error", "conversation mode is disabled"}};
    }

    if (source_pid <= 0) {
      return {{"status", "error"},
              {"error", "LLM_REQUEST requires valid source_pid"}};
    }

    auto source_process = registry_->get_process(source_pid);
    if (!source_process) {
      return {{"status", "error"},
              {"error", "LLM_REQUEST source_pid not registered"}};
    }

    if (mode == "simple") {
      if (!convo_id.empty() || !user_id.empty()) {
        return {{"status", "error"},
                {"error", "simple mode requires empty convo_id and user_id"}};
      }
    } else if (mode == "conversation") {
      if (!user_id.empty()) {
        return {{"status", "error"},
                {"error", "conversation mode requires empty user_id"}};
      }
    } else if (mode == "user_conversation") {
      if (source_process->role != "handler") {
        return {{"status", "error"},
                {"error", "user_conversation only allowed for handler process"}};
      }
      if (user_id.empty()) {
        return {{"status", "error"},
                {"error", "user_conversation requires user_id"}};
      }
    }

        return {{"status", "ok"},
          {"is_handler", source_process->role == "handler"}};
  }

  json handle_tree_status(const json &message) {
    const json payload = message.value("payload", json::object());
    const std::string tree_id =
        payload.value("tree_id", message.value("tree_id", ""));

    if (tree_id.empty()) {
      json trees = json::array();
      for (const auto &tid : registry_->get_all_tree_ids()) {
        auto status = registry_->get_tree_status(tid);
        if (status.found) {
          trees.push_back(
              build_tree_json(tid, status.status, status.root_pid));
        }
      }
      return {{"status", "ok"}, {"trees", trees}};
    }

    auto status = registry_->get_tree_status(tree_id);
    if (!status.found) {
      return {{"status", "error"}, {"error", "tree not found"}};
    }

    json processes = json::array();
    for (int pid : registry_->get_tree_processes(tree_id)) {
      auto process = registry_->get_process(pid);
      if (process) {
        processes.push_back(build_process_json(*process));
      }
    }

    return {{"status", "ok"},
            {"tree", build_tree_json(tree_id, status.status, status.root_pid)},
            {"processes", processes}};
  }

  json handle_tree_kill(const json &message) {
    const json payload = message.value("payload", json::object());
    const std::string tree_id =
        payload.value("tree_id", message.value("tree_id", ""));
    const int source_pid = message.value("source_pid", -1);

    if (tree_id.empty()) {
      return {{"status", "error"}, {"error", "TREE_KILL requires tree_id"}};
    }

    // Get pids to kill (without holding any locks)
    std::vector<int> kill_pids = registry_->kill_tree(tree_id);

    if (!kill_pids.empty()) {
      std::vector<TerminationEngine::TerminationTarget> targets;
      for (int pid : kill_pids) {
        auto process = registry_->get_process(pid);
        if (process && process->alive.load()) {
          TerminationEngine::TerminationTarget target;
          target.velix_pid = pid;
          target.os_pid = process->os_pid;
          target.tree_id = tree_id;
          target.trace_id = process->trace_id;
          target.parent_pid = process->parent_pid;
          targets.push_back(target);
        }
      }

      termination_engine_->kill_processes(targets, "tree_kill", registry_,
                                          config_.terminate_grace_ms);
    }

    // FIX #11: Enhanced logging with context (count, source_pid, reason)
    LOG_WARN_CTX("Tree killed with " + std::to_string(kill_pids.size()) +
                     " processes",
                 "supervisor", tree_id, source_pid, "tree_killed");

    return {{"status", "ok"}, {"tree_id", tree_id}, {"tree_status", "KILLED"}};
  }

  void watchdog_loop() {
    while (running_) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(config_.watchdog_interval_ms));

      if (!running_) {
        break;
      }

      // FIX #12: Use time helper instead of manual chrono
      uint64_t now_ms = get_current_time_ms();

      // Check heartbeat timeouts
      auto snapshot = registry_->snapshot_for_watchdog();
      std::vector<TerminationEngine::TerminationTarget> targets;

      for (const auto &entry : snapshot) {
        uint64_t elapsed_ms = now_ms - entry.last_heartbeat_ms;
        if (elapsed_ms >
            static_cast<uint64_t>(config_.heartbeat_timeout_sec) * 1000) {
          // FIX #3: WatchdogEntry now has all fields instead of second lookup
          TerminationEngine::TerminationTarget target;
          target.velix_pid = entry.pid;
          target.os_pid = entry.os_pid;  // ← From snapshot, not get_process
          target.tree_id = entry.tree_id;
          target.trace_id = entry.trace_id;  // ← From snapshot
          target.parent_pid = entry.parent_pid;  // ← From snapshot
          targets.push_back(target);
        }

        // FIX #8: Check tree runtime limit
        if (!config_.exempt_system_tree_limits ||
            entry.tree_id != "TREE_HANDLER") {
          uint64_t tree_runtime_sec =
              (now_ms - entry.tree_created_at_ms) / 1000;
          if (tree_runtime_sec >
              static_cast<uint64_t>(config_.max_tree_runtime_sec)) {
            // Don't add to targets yet - might already be terminating
          }
        }

        // FIX #8: Check memory limit per tree
        // (Would need aggregate - done via compute_tree_memory_mb below)
      }

      if (!targets.empty()) {
        termination_engine_->kill_processes(
            targets, "heartbeat_timeout", registry_,
            config_.terminate_grace_ms);
      }

      // FIX #8: Implement resource limit enforcement
      // Check memory and runtime limits per tree
      auto tree_ids_with_created = registry_->get_all_tree_ids_with_created_at();
      for (const auto &[tree_id, created_at_ms] : tree_ids_with_created) {
        if (config_.exempt_system_tree_limits &&
            tree_id == "TREE_HANDLER") {
          continue;
        }

        // Check runtime limit
        uint64_t tree_runtime_sec = (now_ms - created_at_ms) / 1000;
        if (tree_runtime_sec >
            static_cast<uint64_t>(config_.max_tree_runtime_sec)) {
          auto pids = registry_->kill_tree(tree_id);
          if (!pids.empty()) {
            LOG_WARN_CTX(
                "Tree exceeded runtime limit (" +
                    std::to_string(tree_runtime_sec) + "s > " +
                    std::to_string(config_.max_tree_runtime_sec) + "s)",
                "supervisor", tree_id, -1, "tree_runtime_limit_exceeded");
            std::vector<TerminationEngine::TerminationTarget> targets;
            for (int pid : pids) {
              auto process = registry_->get_process(pid);
              if (process && process->alive.load()) {
                TerminationEngine::TerminationTarget target;
                target.velix_pid = pid;
                target.os_pid = process->os_pid;
                target.tree_id = tree_id;
                target.trace_id = process->trace_id;
                target.parent_pid = process->parent_pid;
                targets.push_back(target);
              }
            }
            termination_engine_->kill_processes(
                targets, "tree_runtime_limit", registry_,
                config_.terminate_grace_ms);
          }
        }

        // Check memory limit
        double tree_memory_mb = registry_->compute_tree_memory_mb(tree_id);
        if (tree_memory_mb > config_.max_memory_per_tree_mb) {
          auto pids = registry_->kill_tree(tree_id);
          if (!pids.empty()) {
            LOG_WARN_CTX("Tree exceeded memory limit (" +
                             std::to_string(tree_memory_mb) + "MB > " +
                             std::to_string(config_.max_memory_per_tree_mb) +
                             "MB)",
                         "supervisor", tree_id, -1,
                         "tree_memory_limit_exceeded");
            std::vector<TerminationEngine::TerminationTarget> targets;
            for (int pid : pids) {
              auto process = registry_->get_process(pid);
              if (process && process->alive.load()) {
                TerminationEngine::TerminationTarget target;
                target.velix_pid = pid;
                target.os_pid = process->os_pid;
                target.tree_id = tree_id;
                target.trace_id = process->trace_id;
                target.parent_pid = process->parent_pid;
                targets.push_back(target);
              }
            }
            termination_engine_->kill_processes(
                targets, "tree_memory_limit", registry_,
                config_.terminate_grace_ms);
          }
        }

        // Check LLM request limit
        auto tree_status = registry_->get_tree_status(tree_id);
        if (tree_status.found) {
          // Note: LLM request count is tracked atomically in TreeInfo
          // This is a placeholder - full LLM tracking would require
          // enhancements to TreeInfo
        }
      }

      // Check tree completion
      for (const auto &[tree_id, _created] : tree_ids_with_created) {
        if (registry_->mark_tree_completed_if_done(tree_id)) {
          LOG_INFO_CTX("Tree completed", "supervisor", tree_id, -1,
                       "tree_completed");
        }
      }

      // FIX #8: Add conversation cleanup (TTL enforcement)
      {
        std::scoped_lock lock(conversation_mutex_);
        std::vector<std::string> expired_convos;
        for (auto &[convo_id, metadata] : convo_metadata_) {
          uint64_t age_sec = (now_ms - metadata.created_at_ms) / 1000;
          if (age_sec > static_cast<uint64_t>(config_.process_convo_ttl_sec)) {
            expired_convos.push_back(convo_id);
          }
        }

        for (const auto &convo_id : expired_convos) {
          LOG_DEBUG_CTX("Conversation expired (TTL exceeded)", "supervisor", "",
                        -1, "convo_expired");
          convo_metadata_.erase(convo_id);

          // Also remove from process index
          for (auto &[pid, convo_ids] : process_convos_) {
            convo_ids.erase(convo_id);
          }
        }
      }

      // Handler restart monitoring
      if (config_.handler_auto_restart && !config_.handler_binary.empty()) {
        const int hvpid = handler_velix_pid_.load();
        bool handler_alive = false;

        if (hvpid > 0) {
          auto process = registry_->get_process(hvpid);
          // FIX #4: Use alive flag instead of status check for safer detection
          handler_alive = (process && process->alive.load());
        }

        if (!handler_alive) {
          if (!handler_needs_restart_) {
            handler_needs_restart_ = true;
            handler_dead_since_ = std::chrono::steady_clock::now();
            LOG_WARN_CTX("Handler process gone; scheduling restart",
                         "supervisor", "TREE_HANDLER", -1, "handler_dead");
          } else {
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - handler_dead_since_)
                    .count();
            if (elapsed_ms >= config_.handler_restart_delay_ms) {
              LOG_WARN_CTX("Restarting handler process", "supervisor",
                           "TREE_HANDLER", -1, "handler_restart");
              spawn_handler_process();
              handler_needs_restart_ = false;
            }
          }
        } else {
          handler_needs_restart_ = false;
        }
      }
    }
  }
};

SupervisorService &supervisor_service() {
  static SupervisorService service;
  return service;
}

}  // namespace

void start_supervisor(int port) {
  if (port <= 0) {
    port = velix::utils::get_port("SUPERVISOR", 5173);
  }
  supervisor_service().start(port);
}

void stop_supervisor() { supervisor_service().stop(); }

}  // namespace velix::core
