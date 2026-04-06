#pragma once

#include "process_registry.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace velix::core {

/**
 * TerminationEngine: Perform OS-level termination and bus notifications.
 *
 * This engine is responsible for:
 * 1. Marking processes as killed in the registry
 * 2. Sending CHILD_TERMINATED notifications to the BUS
 * 3. Sending SIGTERM to the OS process
 * 4. Waiting for graceful shutdown
 * 5. Sending SIGKILL if process still alive
 * 6. Removing process from registry
 *
 * Important: This class does NOT hold registry locks. It interacts with
 * the registry only through public APIs that don't require caller to hold
 * locks.
 *
 * Thread-safety:
 * - All methods are thread-safe
 * - Methods may be called from multiple threads simultaneously
 * - No internal state that needs protection (stateless)
 */
class TerminationEngine {
public:
  struct TerminationTarget {
    int velix_pid;
    int os_pid;
    std::string tree_id;
    std::string trace_id;
    int parent_pid;
  };

  /**
   * Kill a list of processes.
   *
   * For each process:
   * 1. Call registry.mark_process_killed(pid) to set alive=false atomically
   * 2. Send CHILD_TERMINATED notification to BUS (if applicable)
   * 3. Send SIGTERM to OS process
   * 4. Wait terminate_grace_ms
   * 5. Send SIGKILL if still alive
   * 6. Call registry.terminate_process(pid) to remove from registry
   *
   * All OS operations occur OUTSIDE registry locks.
   *
   * No locks are held during execution.
   */
  void kill_processes(const std::vector<TerminationTarget> &targets,
                      const std::string &reason,
                      std::shared_ptr<ProcessRegistry> registry,
                      int terminate_grace_ms);

private:
  /**
   * Send OS signal to process (SIGTERM or SIGKILL).
   *
   * Returns true if signal was sent or process already gone.
   * Returns false if signal failed.
   *
   * Platform-specific: Uses kill() on Unix, TerminateProcess() on Windows.
   */
  bool send_signal_to_os_process(int os_pid, int signal) const;

  /**
   * Notify the BUS that a child process has terminated.
   *
   * This is best-effort - failures are logged but not fatal.
   */
  void notify_bus_child_terminated(const TerminationTarget &target,
                                   const std::string &reason) const;
};

/**
 * WatchdogEngine: Monitor system health and enforce resource limits.
 *
 * Runs in a dedicated thread and performs periodic checks:
 * - Heartbeat timeouts (process didn't report status recently)
 * - Tree runtime limits (tree has been running too long)
 * - Memory limits (tree exceeded memory quota)
 * - LLM request limits (tree exceeded request quota)
 *
 * Thread model:
 * - Single watchdog thread calls watchdog_loop() continuously
 * - Takes registry snapshots periodically
 * - Processes snapshots outside of registry locks
 * - Dispatches terminations to TerminationEngine
 *
 * Configuration (passed at construction):
 * - heartbeat_timeout_sec: how long until a process is considered dead
 * - max_tree_runtime_sec: maximum age for a tree
 * - max_memory_per_tree_mb: memory quota per tree
 * - max_llm_requests_per_tree: request quota per tree
 * - watchdog_interval_ms: how often to check (in milliseconds)
 * - terminate_grace_ms: grace period for SIGTERM before SIGKILL
 * - exempt_system_tree_limits: whether to skip limits for TREE_HANDLER
 */
class WatchdogEngine {
public:
  struct Config {
    int heartbeat_timeout_sec = 15;
    int max_tree_runtime_sec = 3600;
    double max_memory_per_tree_mb = 2048.0;
    int max_llm_requests_per_tree = 1000;
    int watchdog_interval_ms = 1000;
    int terminate_grace_ms = 1500;
    bool exempt_system_tree_limits = true;
  };

  WatchdogEngine(std::shared_ptr<ProcessRegistry> registry,
                 std::shared_ptr<TerminationEngine> termination_engine,
                 const Config &config)
      : registry_(registry),
        termination_engine_(termination_engine),
        config_(config) {}

  /**
   * Main watchdog loop - runs in dedicated thread.
   *
   * Loop:
   * 1. Sleep watchdog_interval_ms
   * 2. snapshot_for_watchdog() - get current state
   * 3. Check each process for violations:
   *    - Heartbeat timeout
   *    - Tree runtime exceeded
   *    - Tree memory exceeded
   *    - Tree LLM request limit
   * 4. Collect all violating pids
   * 5. Call kill_processes() for violators
   * 6. Check for tree completion
   *
   * Never holds locks for extended periods.
   */
    void watchdog_loop(
      const std::function<bool()> &is_running);  // Called with running flag

private:
  std::shared_ptr<ProcessRegistry> registry_;
  std::shared_ptr<TerminationEngine> termination_engine_;
  Config config_;

  /**
   * Check if a process has timed out (missed heartbeat).
   *
   * Returns true if now_ms - last_heartbeat_ms > heartbeat_timeout_sec * 1000
   */
  bool is_heartbeat_timeout(const WatchdogEntry &entry, uint64_t now_ms);

  /**
   * Check if tree has exceeded runtime limit.
   *
   * Returns true if age > max_tree_runtime_sec (unless exempted).
   */
  bool is_tree_runtime_exceeded(const std::string &tree_id,
                                uint64_t tree_created_ms,
                                uint64_t now_ms);

  /**
   * Check if tree has exceeded memory limit.
   *
   * Returns true if memory > max_memory_per_tree_mb (unless exempted).
   */
  bool is_tree_memory_exceeded(const std::string &tree_id);

  /**
   * Check if tree has exceeded LLM request limit.
   *
   * Returns true if llm_request_count > max_llm_requests_per_tree
   * (unless exempted).
   */
  bool is_tree_llm_limit_exceeded(const std::string &tree_id);

  /**
   * Check if system tree limits should be exempted.
   *
   * Returns true if config_.exempt_system_tree_limits && tree_id ==
   * "TREE_HANDLER".
   */
  bool should_exempt_tree(const std::string &tree_id) const;
};

}  // namespace velix::core
