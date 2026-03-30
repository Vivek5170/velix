#include "supervisor.hpp"
#include "../communication/socket_wrapper.hpp"
#include "../utils/logger.hpp"
#include "../vendor/nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
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

constexpr int kDefaultSupervisorPort = 5173;

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
  int max_active_process_convos_per_pid = 100; // only process-type convos count
  int process_convo_ttl_sec = 86400;

  // Handler spawn settings
  std::string handler_binary; // empty = don't auto-spawn
  std::vector<std::string> handler_args;
  bool handler_auto_restart = true;
  int handler_restart_delay_ms = 2000;
};

SupervisorConfig load_supervisor_config() {
  SupervisorConfig config;

  std::ifstream file("config/supervisor.json");
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

enum class ProcessStatus {
  STARTING,
  RUNNING,
  WAITING_LLM,
  WAITING_EXEC,
  FINISHED,
  ERROR,
  KILLED
};

enum class TreeStatus { ACTIVE, COMPLETED, FAILED, KILLED };

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

struct ProcessInfo {
  int pid = -1;
  int os_pid = -1;
  std::string tree_id;
  int parent_pid = -1;
  std::string role = "unknown";
  ProcessStatus status = ProcessStatus::STARTING;
  double memory_mb = 0.0;
  std::chrono::steady_clock::time_point last_heartbeat =
      std::chrono::steady_clock::now();
};

struct TreeInfo {
  std::string tree_id;
  TreeStatus status = TreeStatus::ACTIVE;
  int root_pid = -1;
  int llm_request_count = 0;
  std::chrono::steady_clock::time_point created_at =
      std::chrono::steady_clock::now();
};

struct ConvMetadata {
  std::string convo_id;
  std::string convo_type; // "user" | "process"
  std::string user_id;    // only for type="user"
  int creator_pid = -1;   // for type="process": the only pid allowed to use it
  long created_at_ms = 0;
  std::chrono::steady_clock::time_point last_activity_at =
      std::chrono::steady_clock::now();
};

struct TerminationTarget {
  int velix_pid = -1;
  int os_pid = -1;
  std::string tree_id;
};

bool is_terminal_status(ProcessStatus status) {
  return status == ProcessStatus::FINISHED || status == ProcessStatus::KILLED;
}

bool is_system_handler_tree(const std::string &tree_id) {
  return tree_id == "TREE_HANDLER";
}

bool is_limit_enabled(int value) { return value > 0; }

bool is_limit_enabled(double value) { return value > 0.0; }

bool terminate_os_process(int pid) {
  if (pid <= 0) {
    return false;
  }

#ifdef _WIN32
  HANDLE process =
      OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) {
    return false;
  }
  const BOOL terminated = TerminateProcess(process, 1);
  CloseHandle(process);
  return terminated == TRUE;
#else
  if (::kill(pid, SIGTERM) == -1) {
    if (errno == ESRCH) {
      return true;
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  if (::kill(pid, 0) == 0) {
    if (::kill(pid, SIGKILL) == -1) {
      return errno == ESRCH;
    }
  }

  return true;
#endif
}

/**
 * Fixed-size thread pool for handling supervisor client connections.
 * Uses only standard C++11 primitives — works on Linux, macOS, and Windows.
 * Worker threads are created once at construction and reused for every
 * accepted connection, eliminating per-connection thread create/destroy cost.
 */
class ClientThreadPool {
public:
  explicit ClientThreadPool(int thread_count, int max_queued)
      : stop_(false), pending_count_(0),
        capacity_(static_cast<std::size_t>(max_queued > 0 ? max_queued : 512)) {
    workers_.reserve(static_cast<std::size_t>(thread_count));
    for (int i = 0; i < thread_count; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  ~ClientThreadPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto &w : workers_) {
      if (w.joinable()) {
        w.join();
      }
    }
  }

  // Non-copyable, non-movable
  ClientThreadPool(const ClientThreadPool &) = delete;
  ClientThreadPool &operator=(const ClientThreadPool &) = delete;

  /**
   * Try to submit a task. Returns false if the pool is at capacity
   * (back-pressure). Safe to call from any thread.
   */
  bool try_submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_ || pending_count_ >= capacity_) {
        return false;
      }
      tasks_.push(std::move(task));
      ++pending_count_;
    }
    cv_.notify_one();
    return true;
  }

private:
  void worker_loop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
        if (stop_ && tasks_.empty()) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop();
        --pending_count_;
      }
      task();
    }
  }

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_;
  std::size_t pending_count_;
  const std::size_t capacity_;
};

class SupervisorService {
public:
  SupervisorService()
      : config_(load_supervisor_config()),
        thread_pool_(config_.max_client_threads,
                     config_.max_client_threads * 4),
        running_(false), tree_counter_(1), process_counter_(1000) {
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

    watchdog_thread_ = std::thread([this]() { watchdog_loop(); });

    // Spawn handler if configured
    if (!config_.handler_binary.empty()) {
      spawn_handler_process();
    }

    try {
      {
        std::lock_guard<std::mutex> lock(server_mutex_);
        server_socket_ =
            std::make_shared<velix::communication::SocketWrapper>();
        server_socket_->create_tcp_socket();
        server_socket_->bind("127.0.0.1", static_cast<uint16_t>(port));
        server_socket_->listen(8);
      }

      LOG_INFO_CTX("Supervisor listening on 127.0.0.1:" + std::to_string(port),
                   "supervisor", "", -1, "listen");

      while (running_) {
        try {
          velix::communication::SocketWrapper client;
          std::shared_ptr<velix::communication::SocketWrapper> server_socket;
          {
            std::lock_guard<std::mutex> lock(server_mutex_);
            server_socket = server_socket_;
          }

          if (!server_socket || !server_socket->is_open()) {
            break;
          }

          client = server_socket->accept();

          // Wrap in shared_ptr so the lambda is copy-constructible
          // (std::function requires copy-constructibility; SocketWrapper is
          // move-only).
          auto client_ptr =
              std::make_shared<velix::communication::SocketWrapper>(
                  std::move(client));
          const bool submitted =
              thread_pool_.try_submit([this, client_ptr]() mutable {
                try {
                  handle_client(std::move(*client_ptr));
                } catch (...) {
                }
              });

          if (!submitted) {
            // Pool at capacity: reject with back-pressure error
            try {
              json busy = {
                  {"status", "error"},
                  {"error", "supervisor busy: max client threads reached"}};
              velix::communication::send_json(client, busy.dump());
            } catch (...) {
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
    const bool was_running = running_.exchange(false);
    if (!was_running) {
      join_watchdog();
      return;
    }

    {
      std::lock_guard<std::mutex> lock(server_mutex_);
      if (server_socket_ && server_socket_->is_open()) {
        server_socket_->close();
      }
      server_socket_.reset();
    }

    join_watchdog();
    LOG_INFO_CTX("Supervisor stopped gracefully", "supervisor", "", -1,
                 "shutdown");
  }

private:
  SupervisorConfig config_;

  std::mutex state_mutex_;
  std::unordered_map<int, ProcessInfo> process_table_;
  std::unordered_map<std::string, TreeInfo> tree_table_;
  std::unordered_map<std::string, std::unordered_set<int>> tree_process_index_;
  std::unordered_map<int, std::unordered_set<int>> process_children_;

  // Conversation tracking
  std::unordered_map<int, std::unordered_set<std::string>>
      process_convos_; // pid -> {convo_ids}
  std::unordered_map<std::string, ConvMetadata>
      convo_metadata_; // convo_id -> metadata

  std::mutex server_mutex_;
  std::shared_ptr<velix::communication::SocketWrapper> server_socket_;

  // Thread pool must be declared after config_ so it initialises after it
  ClientThreadPool thread_pool_;

  std::atomic<bool> running_;
  std::atomic<std::uint64_t> tree_counter_;
  std::atomic<std::uint64_t> process_counter_;
  std::thread watchdog_thread_;

  // Handler lifecycle tracking (for auto-spawn and restart)
  std::atomic<int> handler_os_pid_{-1}; // OS-level PID of the spawned handler
  std::atomic<int> handler_velix_pid_{
      -1}; // velix PID of the registered handler
  bool handler_needs_restart_{false};
  std::chrono::steady_clock::time_point handler_dead_since_;

  void on_process_registered_unlocked(const ProcessInfo &process) {
    tree_process_index_[process.tree_id].insert(process.pid);
    if (process.parent_pid > 0) {
      process_children_[process.parent_pid].insert(process.pid);
    }
  }

  void on_process_terminal_unlocked(int pid) {
    auto process_it = process_table_.find(pid);
    if (process_it == process_table_.end()) {
      return;
    }

    const std::string tree_id = process_it->second.tree_id;
    auto tree_index_it = tree_process_index_.find(tree_id);
    if (tree_index_it != tree_process_index_.end()) {
      tree_index_it->second.erase(pid);
      if (tree_index_it->second.empty()) {
        tree_process_index_.erase(tree_index_it);
      }
    }

    const int parent_pid = process_it->second.parent_pid;
    if (parent_pid > 0) {
      auto children_it = process_children_.find(parent_pid);
      if (children_it != process_children_.end()) {
        children_it->second.erase(pid);
        if (children_it->second.empty()) {
          process_children_.erase(children_it);
        }
      }
    }

    auto own_children_it = process_children_.find(pid);
    if (own_children_it != process_children_.end()) {
      process_children_.erase(own_children_it);
    }

    // Clean up process-type conversation tracking for this pid.
    // User convos are not in process_convos_ and are unaffected.
    auto pc_it = process_convos_.find(pid);
    if (pc_it != process_convos_.end()) {
      for (const auto &cid : pc_it->second) {
        convo_metadata_.erase(cid);
      }
      process_convos_.erase(pc_it);
    }

    // Remove terminal process from the main registry.
    process_table_.erase(pid);
  }

  void join_watchdog() {
    if (watchdog_thread_.joinable()) {
      watchdog_thread_.join();
    }
  }

  void spawn_handler_process() {
    if (config_.handler_binary.empty()) {
      return;
    }

#ifndef _WIN32
    // Reap the previous handler child to avoid zombie accumulation.
    {
      const int prev_os_pid = handler_os_pid_.load();
      if (prev_os_pid > 0) {
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
    std::string cmd_line = "\"" + config_.handler_binary + "\"";
    for (const auto &arg : config_.handler_args) {
      cmd_line += " \"" + arg + "\"";
    }

    // CreateProcessA requires a mutable char buffer for the command line string
    std::vector<char> cmd_buffer(cmd_line.begin(), cmd_line.end());
    cmd_buffer.push_back('\0');

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL,              // Application Name
                        cmd_buffer.data(), // Command Line (mutable)
                        NULL, NULL, FALSE, // Inherit Handles
                        CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, // Flags
                        NULL, NULL, // Env and CWD
                        &si, &pi)) {
      LOG_ERROR_CTX(
          "CreateProcessA failed when spawning handler. Error code: " +
              std::to_string(GetLastError()),
          "supervisor", "TREE_HANDLER", -1, "handler_spawn_error");
      return;
    }

    handler_os_pid_.store(static_cast<int>(pi.dwProcessId));
    LOG_INFO_CTX("Spawned handler with OS PID " +
                     std::to_string(pi.dwProcessId),
                 "supervisor", "TREE_HANDLER", -1, "handler_spawn");

    // We do not need to hold the kernel handles open for the detached daemon
    // execution
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
        "REGISTER_PID", "HEARTBEAT", "LLM_REQUEST", "TREE_STATUS", "TREE_KILL"};

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
      if (!message.contains("source_pid") ||
          !message["source_pid"].is_number_integer()) {
        error = "REGISTER_PID requires integer source_pid";
        return false;
      }
    }

    return true;
  }

  void mark_tree_failed_unlocked(const std::string &tree_id) {
    auto tree_it = tree_table_.find(tree_id);
    if (tree_it != tree_table_.end() &&
        tree_it->second.status != TreeStatus::KILLED &&
        tree_it->second.status != TreeStatus::COMPLETED) {
      tree_it->second.status = TreeStatus::FAILED;
    }
  }

  bool mark_tree_completed_if_done_unlocked(const std::string &tree_id) {
    if (is_system_handler_tree(tree_id)) {
      return false;
    }

    auto tree_it = tree_table_.find(tree_id);
    if (tree_it == tree_table_.end()) {
      return false;
    }

    if (tree_it->second.status == TreeStatus::KILLED ||
        tree_it->second.status == TreeStatus::FAILED ||
        tree_it->second.status == TreeStatus::COMPLETED) {
      return false;
    }

    auto index_it = tree_process_index_.find(tree_id);
    if (index_it != tree_process_index_.end() && !index_it->second.empty()) {
      return false;
    }

    for (const auto &[pid, process] : process_table_) {
      if (process.tree_id == tree_id && !is_terminal_status(process.status)) {
        return false;
      }
    }

    if (tree_it->second.root_pid > 0) {
      tree_it->second.status = TreeStatus::COMPLETED;
      return true;
    }

    return false;
  }

  std::vector<int>
  collect_active_tree_pids_unlocked(const std::string &tree_id) {
    std::vector<int> pids;
    auto index_it = tree_process_index_.find(tree_id);
    if (index_it == tree_process_index_.end()) {
      return pids;
    }

    for (int pid : index_it->second) {
      auto process_it = process_table_.find(pid);
      if (process_it != process_table_.end() &&
          !is_terminal_status(process_it->second.status)) {
        pids.push_back(pid);
      }
    }
    return pids;
  }

  double compute_tree_memory_mb_unlocked(const std::string &tree_id) {
    double total = 0.0;
    auto index_it = tree_process_index_.find(tree_id);
    if (index_it == tree_process_index_.end()) {
      return total;
    }

    for (int pid : index_it->second) {
      auto process_it = process_table_.find(pid);
      if (process_it != process_table_.end() &&
          !is_terminal_status(process_it->second.status)) {
        total += process_it->second.memory_mb;
      }
    }
    return total;
  }

  void terminate_processes(const std::vector<TerminationTarget> &targets,
                           const std::string &reason) {

    // Removed legacy `control_port` network pinging.
    // We now execute pure stateless OS-level PID SIGKILLs exclusively
    // to cleanly murder rogue agents and children without port exhaustion
    // vulnerabilities.

    for (const auto &target : targets) {
      bool still_active = false;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto process_it = process_table_.find(target.velix_pid);
        still_active = process_it != process_table_.end() &&
                       process_it->second.status != ProcessStatus::FINISHED &&
                       process_it->second.status != ProcessStatus::KILLED;
      }

      if (!still_active) {
        continue;
      }

      if (target.os_pid > 0) {
        const bool ok = terminate_os_process(target.os_pid);
        if (!ok) {
          LOG_WARN_CTX("Failed forced OS kill for velix pid " +
                           std::to_string(target.velix_pid),
                       "supervisor", target.tree_id, target.velix_pid,
                       reason + "_force_kill_failed");
        }
      } else {
        LOG_WARN_CTX("Missing os_pid for forced termination", "supervisor",
                     target.tree_id, target.velix_pid,
                     reason + "_missing_os_pid");
      }

      std::lock_guard<std::mutex> lock(state_mutex_);
      auto process_it = process_table_.find(target.velix_pid);
      if (process_it != process_table_.end() &&
          process_it->second.status != ProcessStatus::FINISHED &&
          process_it->second.status != ProcessStatus::KILLED) {
        process_it->second.status = ProcessStatus::KILLED;
        on_process_terminal_unlocked(target.velix_pid);
      }
    }
  }

  std::vector<TerminationTarget>
  build_termination_targets_unlocked(const std::vector<int> &pids) {
    std::vector<TerminationTarget> targets;
    targets.reserve(pids.size());

    for (int pid : pids) {
      auto process_it = process_table_.find(pid);
      if (process_it == process_table_.end()) {
        continue;
      }

      TerminationTarget target;
      target.velix_pid = process_it->second.pid;
      target.os_pid = process_it->second.os_pid;
      target.tree_id = process_it->second.tree_id;
      targets.push_back(target);
    }

    return targets;
  }

  void register_system_handler_tree() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    TreeInfo handler_tree;
    handler_tree.tree_id = "TREE_HANDLER";
    handler_tree.status = TreeStatus::ACTIVE;
    tree_table_[handler_tree.tree_id] = handler_tree;
  }

  std::string create_tree_unlocked() {
    std::ostringstream oss;
    oss << "TREE_" << tree_counter_++;

    TreeInfo tree_info;
    tree_info.tree_id = oss.str();
    tree_info.status = TreeStatus::ACTIVE;
    tree_info.created_at = std::chrono::steady_clock::now();

    tree_table_[tree_info.tree_id] = tree_info;
    return tree_info.tree_id;
  }

  json build_process_json(const ProcessInfo &process) const {
    return {
        {"pid", process.pid},         {"os_pid", process.os_pid},
        {"tree_id", process.tree_id}, {"parent_pid", process.parent_pid},
        {"role", process.role},       {"status", to_string(process.status)}};
  }

  json build_tree_json(const TreeInfo &tree) const {
    return {{"tree_id", tree.tree_id},
            {"status", to_string(tree.status)},
            {"root_pid", tree.root_pid}};
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
      LOG_ERROR_CTX(std::string("Supervisor request error: ") + e.what(),
                    "supervisor", "", -1, "request_error");
      try {
        json error = {{"status", "error"}, {"error", e.what()}};
        velix::communication::send_json(client_sock, error.dump());
      } catch (...) {
        LOG_ERROR_CTX("Failed to send supervisor error response", "supervisor",
                      "", -1, "response_error");
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
              {"error", "REGISTER_PID requires payload.register_intent = "
                        "NEW_TREE or JOIN_PARENT_TREE"}};
    }

    std::lock_guard<std::mutex> lock(state_mutex_);

    int pid = -1;
    std::uint64_t pid_candidate = process_counter_++;
    while (true) {
      if (pid_candidate >
          static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return {{"status", "error"}, {"error", "pid space exhausted"}};
      }

      pid = static_cast<int>(pid_candidate);
      if (process_table_.count(pid) == 0) {
        break;
      }

      pid_candidate = process_counter_++;
    }

    std::string tree_id;
    bool is_tree_root_registration = false;
    if (role == "handler") {
      tree_id = "TREE_HANDLER";
      source_pid = -1;
      is_tree_root_registration = true;
      // Track the new handler velix pid for restart monitoring
      handler_velix_pid_.store(pid);
      handler_needs_restart_ = false;
      handler_dead_since_ = {};
      LOG_INFO_CTX("Handler registered with velix pid " + std::to_string(pid),
                   "supervisor", "TREE_HANDLER", pid, "handler_registered");
    } else if (register_intent == "NEW_TREE") {
      tree_id = create_tree_unlocked();
      source_pid = -1;
      is_tree_root_registration = true;
    } else {
      if (source_pid <= 0) {
        return {{"status", "error"},
                {"error", "JOIN_PARENT_TREE requires valid source_pid"}};
      }

      auto parent_it = process_table_.find(source_pid);
      if (parent_it == process_table_.end()) {
        return {{"status", "error"},
                {"error", "source_pid is not registered"},
                {"source_pid", source_pid}};
      }

      if (is_terminal_status(parent_it->second.status)) {
        return {{"status", "error"},
                {"error", "source_pid is not active"},
                {"source_pid", source_pid}};
      }

      tree_id = parent_it->second.tree_id;
    }

    std::size_t active_count = 0;
    auto tree_index_it = tree_process_index_.find(tree_id);
    if (tree_index_it != tree_process_index_.end()) {
      active_count = tree_index_it->second.size();
    }

    if (is_limit_enabled(config_.max_processes_per_tree) &&
        active_count >=
            static_cast<std::size_t>(config_.max_processes_per_tree)) {
      if (config_.exempt_system_tree_limits &&
          is_system_handler_tree(tree_id)) {
        // system tree intentionally exempt from per-tree limits
      } else {
        return {{"status", "error"},
                {"error", "max_processes_per_tree exceeded"},
                {"tree_id", tree_id}};
      }
    }

    auto tree_it = tree_table_.find(tree_id);
    if (tree_it != tree_table_.end() &&
        is_limit_enabled(config_.max_tree_runtime_sec)) {
      const auto age =
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::steady_clock::now() - tree_it->second.created_at)
              .count();
      if (age > config_.max_tree_runtime_sec &&
          !(config_.exempt_system_tree_limits &&
            is_system_handler_tree(tree_id))) {
        return {{"status", "error"},
                {"error", "max_tree_runtime_sec exceeded"},
                {"tree_id", tree_id}};
      }
    }

    ProcessInfo process;
    process.pid = pid;
    process.os_pid = os_pid;
    process.tree_id = tree_id;
    process.parent_pid = source_pid;
    process.role = role;
    process.status = status;
    process.memory_mb = memory_mb;
    process.last_heartbeat = std::chrono::steady_clock::now();

    process_table_[pid] = process;
    on_process_registered_unlocked(process);

    TreeInfo &tree = tree_table_[tree_id];
    if (is_tree_root_registration) {
      tree.root_pid = pid;
    }

    LOG_INFO_CTX("Registered pid " + std::to_string(pid) + " in " + tree_id,
                 "supervisor", tree_id, pid, "register_pid");

    return {{"status", "ok"},
            {"tree_id", tree_id},
            {"register_intent", register_intent},
            {"process", build_process_json(process)}};
  }

  json handle_heartbeat(const json &message) {
    const int pid = message.value("pid", -1);
    if (pid <= 0) {
      return {{"status", "error"}, {"error", "HEARTBEAT requires valid pid"}};
    }

    const json payload = message.value("payload", json::object());
    const std::string status_text = payload.value("status", "");
    const double memory_mb = payload.value("memory_mb", -1.0);
    std::vector<int> kill_pids;
    std::string tree_id_for_kill;
    bool tree_completed = false;

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      auto it = process_table_.find(pid);
      if (it == process_table_.end()) {
        return {{"status", "error"}, {"error", "pid not registered"}};
      }

      const ProcessStatus previous_status = it->second.status;
      it->second.last_heartbeat = std::chrono::steady_clock::now();
      if (!status_text.empty()) {
        if (!is_valid_process_status_text(status_text)) {
          return {{"status", "error"},
                  {"error", "invalid process status in HEARTBEAT"},
                  {"status_value", status_text}};
        }
        it->second.status = parse_process_status(status_text);
      }
      if (!is_terminal_status(previous_status) &&
          is_terminal_status(it->second.status)) {
        on_process_terminal_unlocked(pid);
      }
      if (memory_mb >= 0.0) {
        it->second.memory_mb = memory_mb;
      }

      tree_id_for_kill = it->second.tree_id;
      if (is_limit_enabled(config_.max_memory_per_tree_mb)) {
        if (!(config_.exempt_system_tree_limits &&
              is_system_handler_tree(tree_id_for_kill))) {
          const double tree_memory =
              compute_tree_memory_mb_unlocked(tree_id_for_kill);
          if (tree_memory > config_.max_memory_per_tree_mb) {
            mark_tree_failed_unlocked(tree_id_for_kill);
            kill_pids = collect_active_tree_pids_unlocked(tree_id_for_kill);

            for (int kill_pid : kill_pids) {
              auto proc_it = process_table_.find(kill_pid);
              if (proc_it != process_table_.end()) {
                proc_it->second.status = ProcessStatus::ERROR;
              }
            }
          }
        }
      }

      tree_completed = mark_tree_completed_if_done_unlocked(tree_id_for_kill);
    }

    json response = {{"status", "ok"}, {"pid", pid}};

    if (!kill_pids.empty()) {
      LOG_ERROR_CTX("Tree memory limit exceeded; terminating tree",
                    "supervisor", tree_id_for_kill, pid,
                    "memory_limit_exceeded");

      std::vector<TerminationTarget> targets;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        targets = build_termination_targets_unlocked(kill_pids);
      }

      response["warning"] = "memory limit exceeded; tree processes terminated";
      terminate_processes(targets, "memory_limit");
    }

    if (tree_completed) {
      response["tree_status"] = "COMPLETED";
      LOG_INFO_CTX("Tree completed", "supervisor", tree_id_for_kill, pid,
                   "tree_completed");
    }

    return response;
  }

  json handle_llm_request(const json &message) {
    const std::string tree_id = message.value("tree_id", "");
    const std::string convo_id = message.value("convo_id", "");
    const std::string convo_type = message.value("convo_type", "process");
    const std::string user_id = message.value("user_id", "");
    const int source_pid = message.value("source_pid", -1);

    const bool is_conversation_mode = !convo_id.empty();

    std::lock_guard<std::mutex> lock(state_mutex_);

    if (is_conversation_mode) {
      if (!config_.conversation_enabled) {
        return {{"status", "error"},
                {"error", "conversation mode is disabled"}};
      }

      // Source process must be alive and registered
      if (source_pid <= 0) {
        return {{"status", "error"},
                {"error", "conversation mode requires valid source_pid"}};
      }

      auto source_it = process_table_.find(source_pid);
      if (source_it == process_table_.end()) {
        return {{"status", "error"},
                {"error", "source_pid not registered"},
                {"source_pid", source_pid}};
      }
      if (is_terminal_status(source_it->second.status)) {
        return {{"status", "error"},
                {"error", "source_pid is not active"},
                {"source_pid", source_pid}};
      }

      const std::string source_tree = source_it->second.tree_id;

      if (convo_type == "user") {
        // ── User conversation ─────────────────────────────────────────
        // Rule: any process in TREE_HANDLER can use a user convo.
        // Only the handler tree can access persistent user conversations.
        if (!is_system_handler_tree(source_tree)) {
          return {
              {"status", "error"},
              {"error",
               "user conversations can only be used by TREE_HANDLER processes"},
              {"source_pid", source_pid},
              {"source_tree", source_tree}};
        }

        auto meta_it = convo_metadata_.find(convo_id);
        if (meta_it == convo_metadata_.end()) {
          // First LLM use: register in supervisor tracking
          ConvMetadata metadata;
          metadata.convo_id = convo_id;
          metadata.convo_type = "user";
          metadata.user_id = user_id;
          metadata.creator_pid = source_pid;
          metadata.created_at_ms =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
          metadata.last_activity_at = std::chrono::steady_clock::now();
          convo_metadata_[convo_id] = metadata;
          // User convos are NOT tracked in process_convos_: they outlive any
          // pid
        } else {
          if (meta_it->second.convo_type != "user") {
            return {
                {"status", "error"},
                {"error", "convo_id exists but is not a user conversation"}};
          }
          meta_it->second.last_activity_at = std::chrono::steady_clock::now();
        }

        LOG_INFO_CTX("LLM_REQUEST user convo user_id=" + user_id +
                         " source_pid=" + std::to_string(source_pid),
                     "supervisor", "TREE_HANDLER", source_pid,
                     "llm_req_user_convo");

        return {{"status", "ok"},
                {"convo_id", convo_id},
                {"convo_type", "user"},
                {"user_id", user_id}};

      } else {
        // ── Process conversation ──────────────────────────────────────
        // Rule: only the process that created the convo can use it.
        auto meta_it = convo_metadata_.find(convo_id);
        if (meta_it == convo_metadata_.end()) {
          // First use: register; creator = source_pid
          if (is_limit_enabled(config_.max_active_process_convos_per_pid)) {
            const std::size_t active = (process_convos_.count(source_pid)
                                            ? process_convos_[source_pid].size()
                                            : 0);
            if (active >= static_cast<std::size_t>(
                              config_.max_active_process_convos_per_pid)) {
              return {{"status", "error"},
                      {"error", "max_active_process_convos_per_pid exceeded"},
                      {"source_pid", source_pid}};
            }
          }
          ConvMetadata metadata;
          metadata.convo_id = convo_id;
          metadata.convo_type = "process";
          metadata.creator_pid = source_pid;
          metadata.created_at_ms =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
          metadata.last_activity_at = std::chrono::steady_clock::now();
          convo_metadata_[convo_id] = metadata;
          process_convos_[source_pid].insert(convo_id);
        } else {
          if (meta_it->second.convo_type != "process") {
            return {
                {"status", "error"},
                {"error", "convo_id exists but is not a process conversation"}};
          }
          if (meta_it->second.creator_pid != source_pid) {
            return {{"status", "error"},
                    {"error",
                     "process conversations can only be used by their creator"},
                    {"creator_pid", meta_it->second.creator_pid},
                    {"source_pid", source_pid}};
          }
          meta_it->second.last_activity_at = std::chrono::steady_clock::now();
        }

        LOG_INFO_CTX("LLM_REQUEST process convo convo_id=" + convo_id +
                         " source_pid=" + std::to_string(source_pid),
                     "supervisor", source_tree, source_pid,
                     "llm_req_proc_convo");

        return {{"status", "ok"},
                {"convo_id", convo_id},
                {"convo_type", "process"}};
      }

    } else {
      // ── Simple mode: tree-id based ────────────────────────────────────
      if (tree_id.empty()) {
        return {{"status", "error"},
                {"error", "LLM_REQUEST requires tree_id or convo_id"}};
      }

      auto tree_it = tree_table_.find(tree_id);
      if (tree_it == tree_table_.end()) {
        return {{"status", "error"}, {"error", "tree not found"}};
      }

      tree_it->second.llm_request_count += 1;
      if (is_limit_enabled(config_.max_llm_requests_per_tree) &&
          tree_it->second.llm_request_count >
              config_.max_llm_requests_per_tree &&
          !(config_.exempt_system_tree_limits &&
            is_system_handler_tree(tree_id))) {
        tree_it->second.status = TreeStatus::FAILED;
        return {{"status", "error"},
                {"error", "max_llm_requests_per_tree exceeded"},
                {"tree_id", tree_id}};
      }

      return {{"status", "ok"},
              {"tree_id", tree_id},
              {"llm_request_count", tree_it->second.llm_request_count}};
    }
  }

  json handle_tree_status(const json &message) {
    const json payload = message.value("payload", json::object());
    const std::string tree_id =
        payload.value("tree_id", message.value("tree_id", ""));

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (tree_id.empty()) {
      json trees = json::array();
      for (const auto &[id, tree] : tree_table_) {
        trees.push_back(build_tree_json(tree));
      }

      return {{"status", "ok"}, {"trees", trees}};
    }

    auto tree_it = tree_table_.find(tree_id);
    if (tree_it == tree_table_.end()) {
      return {{"status", "error"}, {"error", "tree not found"}};
    }

    json processes = json::array();
    for (const auto &[pid, proc] : process_table_) {
      if (proc.tree_id == tree_id) {
        processes.push_back(build_process_json(proc));
      }
    }

    return {{"status", "ok"},
            {"tree", build_tree_json(tree_it->second)},
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

    std::vector<int> kill_pids;
    std::vector<TerminationTarget> targets;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (source_pid > 0) {
        auto caller_it = process_table_.find(source_pid);
        if (caller_it == process_table_.end()) {
          return {{"status", "error"},
                  {"error", "TREE_KILL caller source_pid is not registered"},
                  {"source_pid", source_pid}};
        }

        const bool caller_is_handler =
            caller_it->second.role == "handler" ||
            is_system_handler_tree(caller_it->second.tree_id);
        if (!caller_is_handler) {
          return {{"status", "error"},
                  {"error", "TREE_KILL allowed only for handler or supervisor "
                            "internal caller"},
                  {"source_pid", source_pid}};
        }
      }

      auto tree_it = tree_table_.find(tree_id);
      if (tree_it == tree_table_.end()) {
        return {{"status", "error"}, {"error", "tree not found"}};
      }

      tree_it->second.status = TreeStatus::KILLED;

      auto tree_index_it = tree_process_index_.find(tree_id);
      if (tree_index_it != tree_process_index_.end()) {
        for (int pid : tree_index_it->second) {
          auto process_it = process_table_.find(pid);
          if (process_it != process_table_.end() &&
              !is_terminal_status(process_it->second.status)) {
            process_it->second.status = ProcessStatus::ERROR;
            kill_pids.push_back(pid);
          }
        }
      }

      targets = build_termination_targets_unlocked(kill_pids);

      // Clean up conversation tracking for killed pids.
      // User convos are NOT tracked in process_convos_ so they survive kills.
      // Process convos are fully removed.
      for (int pid : kill_pids) {
        auto convo_it = process_convos_.find(pid);
        if (convo_it == process_convos_.end()) {
          continue;
        }
        for (const auto &cid : convo_it->second) {
          auto meta_it = convo_metadata_.find(cid);
          if (meta_it != convo_metadata_.end() &&
              meta_it->second.convo_type == "process") {
            convo_metadata_.erase(meta_it);
          }
        }
        process_convos_.erase(convo_it);
      }
    }

    terminate_processes(targets, "tree_kill");

    LOG_WARN_CTX("Tree killed: " + tree_id, "supervisor", tree_id, -1,
                 "tree_kill");

    return {{"status", "ok"}, {"tree_id", tree_id}, {"tree_status", "KILLED"}};
  }

  void watchdog_loop() {
    while (running_) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(config_.watchdog_interval_ms));
      const auto now = std::chrono::steady_clock::now();
      std::unordered_map<int, std::string> pids_to_terminate;
      std::unordered_set<std::string> trees_timed_out;
      std::vector<std::string> trees_completed;
      std::vector<TerminationTarget> termination_targets;

      {
        std::lock_guard<std::mutex> lock(state_mutex_);

        for (auto &[pid, process] : process_table_) {
          const bool completed = is_terminal_status(process.status);

          if (completed) {
            continue;
          }

          if (now - process.last_heartbeat >
              std::chrono::seconds(config_.heartbeat_timeout_sec)) {
            process.status = ProcessStatus::ERROR;
            pids_to_terminate[pid] = "watchdog_timeout";
            trees_timed_out.insert(process.tree_id);
          }
        }

        if (is_limit_enabled(config_.max_tree_runtime_sec)) {
          for (auto &[tree_id, tree] : tree_table_) {
            if (tree.status == TreeStatus::COMPLETED ||
                tree.status == TreeStatus::KILLED) {
              continue;
            }

            if (config_.exempt_system_tree_limits &&
                is_system_handler_tree(tree_id)) {
              continue;
            }

            const auto tree_age_sec =
                std::chrono::duration_cast<std::chrono::seconds>(
                    now - tree.created_at)
                    .count();
            if (tree_age_sec > config_.max_tree_runtime_sec) {
              tree.status = TreeStatus::FAILED;
              const auto tree_pids = collect_active_tree_pids_unlocked(tree_id);
              for (int pid : tree_pids) {
                pids_to_terminate[pid] = "watchdog_timeout";
              }
            }
          }
        }

        for (const auto &tree_id : trees_timed_out) {
          mark_tree_failed_unlocked(tree_id);
        }

        // Watchdog TTL: expire stale process-type convos only.
        // User-type convos are permanent — skip them.
        if (config_.process_convo_ttl_sec > 0) {
          const auto now_tp = std::chrono::steady_clock::now();
          const auto ttl = std::chrono::seconds(config_.process_convo_ttl_sec);

          std::vector<std::string> stale_convos;
          for (const auto &[cid, metadata] : convo_metadata_) {
            if (metadata.convo_type == "user") {
              continue; // user convos never expire via watchdog
            }
            if (now_tp - metadata.last_activity_at > ttl) {
              stale_convos.push_back(cid);
            }
          }
          for (const auto &cid : stale_convos) {
            auto meta_it = convo_metadata_.find(cid);
            if (meta_it == convo_metadata_.end()) {
              continue;
            }
            const int creator_pid = meta_it->second.creator_pid;
            convo_metadata_.erase(meta_it);
            auto pc_it = process_convos_.find(creator_pid);
            if (pc_it != process_convos_.end()) {
              pc_it->second.erase(cid);
              if (pc_it->second.empty()) {
                process_convos_.erase(pc_it);
              }
            }
          }
        }

        for (const auto &[tree_id_k, tree_v] : tree_table_) {
          if (mark_tree_completed_if_done_unlocked(tree_id_k)) {
            trees_completed.push_back(tree_id_k);
          }
        }
        std::vector<int> pids;
        pids.reserve(pids_to_terminate.size());
        for (const auto &[pid, _reason] : pids_to_terminate) {
          pids.push_back(pid);
        }
        termination_targets = build_termination_targets_unlocked(pids);
      }

      if (!termination_targets.empty()) {
        terminate_processes(termination_targets, "watchdog_timeout");
        for (const auto &target : termination_targets) {
          LOG_ERROR_CTX("Terminated pid " + std::to_string(target.velix_pid) +
                            " due watchdog policy",
                        "supervisor", target.tree_id, target.velix_pid,
                        "watchdog_termination");
        }
      }

      for (const auto &tid : trees_completed) {
        LOG_INFO_CTX("Tree completed", "supervisor", tid, -1, "tree_completed");
      }

      // ── Handler auto-restart ──────────────────────────────────────────
      if (config_.handler_auto_restart && !config_.handler_binary.empty()) {
        const int hvpid = handler_velix_pid_.load();
        bool handler_alive = false;

        if (hvpid > 0) {
          std::lock_guard<std::mutex> lock(state_mutex_);
          auto it = process_table_.find(hvpid);
          handler_alive = (it != process_table_.end() &&
                           !is_terminal_status(it->second.status));
        }

        if (!handler_alive) {
          if (!handler_needs_restart_) {
            handler_needs_restart_ = true;
            handler_dead_since_ = now;
            LOG_WARN_CTX("Handler process gone; scheduling restart in " +
                             std::to_string(config_.handler_restart_delay_ms) +
                             " ms",
                         "supervisor", "TREE_HANDLER", -1, "handler_dead");
          } else {
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - handler_dead_since_)
                    .count();
            if (elapsed_ms >= config_.handler_restart_delay_ms) {
              LOG_WARN_CTX("Restarting handler process", "supervisor",
                           "TREE_HANDLER", -1, "handler_restart");
              spawn_handler_process();
              handler_needs_restart_ = false;
              handler_dead_since_ = {};
            }
          }
        } else {
          handler_needs_restart_ = false;
          handler_dead_since_ = {};
        }
      }
    }
  }
};

SupervisorService &supervisor_service() {
  static SupervisorService service;
  return service;
}

} // namespace

void start_supervisor(int port) { supervisor_service().start(port); }

void stop_supervisor() { supervisor_service().stop(); }

} // namespace velix::core
