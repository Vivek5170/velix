#include "termination_engine.hpp"
#include "../communication/network_config.hpp"
#include "../communication/socket_wrapper.hpp"
#include "../utils/config_utils.hpp"
#include "../utils/logger.hpp"
#include "../vendor/nlohmann/json.hpp"

#include <chrono>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

using json = nlohmann::json;

namespace velix::core {

#ifdef _WIN32
constexpr int kTerminateSignal = 15;
constexpr int kForcekillSignal = 9;
#else
constexpr int kTerminateSignal = SIGTERM;
constexpr int kForcekillSignal = SIGKILL;
#endif

void TerminationEngine::kill_processes(
    const std::vector<TerminationTarget> &targets, const std::string &reason,
    std::shared_ptr<ProcessRegistry> registry, int terminate_grace_ms) {
  for (const auto &target : targets) {
    // Step 1: Mark process as killed (lock-free atomic operation)
    registry->mark_process_killed(target.velix_pid);

    // Step 2: Notify bus that child has been terminated
    notify_bus_child_terminated(target, reason);

    // Step 3: Send terminate signal to OS process
    if (target.os_pid > 0) {
      send_signal_to_os_process(target.os_pid, kTerminateSignal);
    }

    // Step 4: Wait for graceful shutdown
    std::this_thread::sleep_for(std::chrono::milliseconds(terminate_grace_ms));

    // Step 5: Send force-kill signal if still alive
    if (target.os_pid > 0) {
      send_signal_to_os_process(target.os_pid, kForcekillSignal);
    }

    // Step 6: Remove from registry
    registry->terminate_process(target.velix_pid);

    LOG_WARN_CTX("Terminated velix pid " + std::to_string(target.velix_pid),
                 "supervisor", target.tree_id, target.velix_pid, reason);
  }
}

bool TerminationEngine::send_signal_to_os_process(int os_pid,
                                                  int signal) const {
  if (os_pid <= 0) {
    return false;
  }

#ifdef _WIN32
  if (signal == kForcekillSignal || signal == kTerminateSignal) {
    HANDLE process =
        OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(os_pid));
    if (process == nullptr) {
      return false; // Process already gone or error
    }
    const BOOL terminated = TerminateProcess(process, 1);
    CloseHandle(process);
    return terminated == TRUE;
  }
  return false;
#else
  if (::kill(static_cast<::pid_t>(os_pid), signal) == -1) {
    // ESRCH = process doesn't exist (which is success for our purposes)
    if (errno == ESRCH) {
      return true;
    }
    return false;
  }
  return true;
#endif
}

void TerminationEngine::notify_bus_child_terminated(
  const TerminationTarget &target, const std::string &reason) const {
  if (target.trace_id.empty() || target.parent_pid <= 0) {
    return;
  }

  try {
    const int bus_port = velix::utils::get_port("BUS", 5174);
    velix::communication::SocketWrapper bus_socket;
    bus_socket.create_tcp_socket();
    bus_socket.connect(
        velix::communication::resolve_service_host("BUS", "127.0.0.1"),
        static_cast<uint16_t>(bus_port));

    json termination_msg = {{"message_type", "CHILD_TERMINATED"},
                            {"source_pid", 0}, // Supervisor
                            {"target_pid", target.parent_pid},
                            {"trace_id", target.trace_id},
                            {"payload",
                             {{"status", "error"},
                              {"error", "child_terminated"},
                              {"reason", reason},
                              {"pid", target.velix_pid}}}};

    velix::communication::send_json(bus_socket, termination_msg.dump());
  } catch (const std::exception &) {
    // Best-effort: bus may be unavailable during shutdown.
  }
}

// ──────────────────────────────────────────────────────────────────────────
// WatchdogEngine implementation
// ──────────────────────────────────────────────────────────────────────────

void WatchdogEngine::watchdog_loop(const std::function<bool()> &is_running) {
  while (is_running()) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.watchdog_interval_ms));

    if (!is_running()) {
      break;
    }

    // Get current time once per loop
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

    // Step 1: Take snapshot - this acquires and releases registry lock
    std::vector<WatchdogEntry> snapshot = registry_->snapshot_for_watchdog();

    // Step 2: Analyze snapshot OUTSIDE of locks

    std::unordered_map<std::string, std::vector<int>> pids_by_tree;
    std::unordered_set<std::string> failed_trees;

    for (const auto &entry : snapshot) {
      // Check heartbeat timeout
      if (is_heartbeat_timeout(entry, now_ms)) {
        pids_by_tree[entry.tree_id].push_back(entry.pid);
        failed_trees.insert(entry.tree_id);
        continue;
      }

      // Check tree memory limit
      if (is_tree_memory_exceeded(entry.tree_id)) {
        pids_by_tree[entry.tree_id].push_back(entry.pid);
        failed_trees.insert(entry.tree_id);
        continue;
      }

      // Check tree LLM request limit
      if (is_tree_llm_limit_exceeded(entry.tree_id)) {
        pids_by_tree[entry.tree_id].push_back(entry.pid);
        failed_trees.insert(entry.tree_id);
      }
    }

    // Check tree runtime limits
    for (const auto &tree_id : registry_->get_all_tree_ids()) {
      auto status = registry_->get_tree_status(tree_id);
      if (!status.found || status.status != TreeStatus::ACTIVE) {
        continue;
      }

      if (should_exempt_tree(tree_id)) {
        continue;
      }

      // Get tree creation time
      // TODO: This requires knowing created_at_ms - we need access to tree
      // created_at
      // For now, we'll defer tree runtime checks to the supervisor level
    }

    // Step 3: Mark failed trees
    for (const auto &tree_id : failed_trees) {
      registry_->mark_tree_failed(tree_id);
    }

    // Step 4: Build termination targets and kill processes
    std::vector<TerminationEngine::TerminationTarget> targets;

    for (const auto &[tree_id, pids] : pids_by_tree) {
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
    }

    if (!targets.empty()) {
      termination_engine_->kill_processes(targets, "watchdog_policy", registry_,
                                          config_.terminate_grace_ms);
    }

    // Step 5: Mark trees as completed if all processes are done
    for (const auto &tree_id : registry_->get_all_tree_ids()) {
      if (registry_->mark_tree_completed_if_done(tree_id)) {
        LOG_INFO_CTX("Tree completed", "supervisor", tree_id, -1,
                     "tree_completed");
      }
    }
  }
}

bool WatchdogEngine::is_heartbeat_timeout(const WatchdogEntry &entry,
                                          uint64_t now_ms) {
  uint64_t elapsed_ms = now_ms - entry.last_heartbeat_ms;
  uint64_t timeout_ms =
      static_cast<uint64_t>(config_.heartbeat_timeout_sec) * 1000;
  return elapsed_ms > timeout_ms;
}

bool WatchdogEngine::is_tree_runtime_exceeded(const std::string &tree_id,
                                              uint64_t tree_created_ms,
                                              uint64_t now_ms) {
  if (config_.max_tree_runtime_sec <= 0) {
    return false;
  }

  if (should_exempt_tree(tree_id)) {
    return false;
  }

  uint64_t elapsed_ms = now_ms - tree_created_ms;
  uint64_t limit_ms =
      static_cast<uint64_t>(config_.max_tree_runtime_sec) * 1000;
  return elapsed_ms > limit_ms;
}

bool WatchdogEngine::is_tree_memory_exceeded(const std::string &tree_id) {
  if (config_.max_memory_per_tree_mb <= 0.0) {
    return false;
  }

  if (should_exempt_tree(tree_id)) {
    return false;
  }

  double memory_mb = registry_->compute_tree_memory_mb(tree_id);
  return memory_mb > config_.max_memory_per_tree_mb;
}

bool WatchdogEngine::is_tree_llm_limit_exceeded(const std::string &tree_id) {
  if (config_.max_llm_requests_per_tree <= 0) {
    return false;
  }

  if (should_exempt_tree(tree_id)) {
    return false;
  }

  auto status = registry_->get_tree_status(tree_id);
  if (!status.found) {
    return false;
  }

  // TODO: We need access to llm_request_count atomic field
  // This requires returning it from get_tree_status or a new API
  return false;
}

bool WatchdogEngine::should_exempt_tree(const std::string &tree_id) const {
  return config_.exempt_system_tree_limits && tree_id == "TREE_HANDLER";
}

} // namespace velix::core
