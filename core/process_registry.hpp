#pragma once

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>

namespace velix::core {

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

/**
 * ProcessInfo: Thread-safe process metadata.
 *
 * Structural fields (never change after registration):
 * - pid, os_pid, tree_id, parent_pid (read-only after construction)
 *
 * Telemetry fields (updated frequently via lock-free atomics):
 * - status, memory_mb, last_heartbeat_ms, alive
 *
 * Rule: Heartbeat updates must NEVER acquire a mutex.
 */
struct ProcessInfo {
  // ─── Structural (immutable after registration) ──────────────────
  int pid;
  int os_pid;
  int parent_pid;

  std::string tree_id;
  std::string role;
  std::string trace_id;

  // ─── Telemetry (lock-free atomic updates) ───────────────────────
  std::atomic<ProcessStatus> status;
  std::atomic<double> memory_mb;
  std::atomic<uint64_t> last_heartbeat_ms;
  std::atomic<bool> alive;
};

/**
 * TreeInfo: Thread-safe tree metadata.
 *
 * Structural fields (protected by registry_mutex):
 * - tree_id, root_pid, created_at_ms
 *
 * Telemetry fields (atomic):
 * - status, llm_request_count
 */
struct TreeInfo {
  std::string tree_id;
  int root_pid{-1};
  uint64_t created_at_ms{0};

  std::atomic<TreeStatus> status{TreeStatus::ACTIVE};
  std::atomic<int> llm_request_count{0};

  TreeInfo() = default;

  TreeInfo(const TreeInfo &other)
      : tree_id(other.tree_id),
        root_pid(other.root_pid),
        created_at_ms(other.created_at_ms),
        status(other.status.load()),
        llm_request_count(other.llm_request_count.load()) {}

  TreeInfo &operator=(const TreeInfo &other) {
    if (this != &other) {
      tree_id = other.tree_id;
      root_pid = other.root_pid;
      created_at_ms = other.created_at_ms;
      status.store(other.status.load());
      llm_request_count.store(other.llm_request_count.load());
    }
    return *this;
  }
};

/**
 * WatchdogEntry: Snapshot of process telemetry for watchdog processing.
 *
 * Created inside lock scope, processed outside lock scope.
 * Prevents holding registry_mutex during expensive checks.
 *
 * IMPORTANT: Includes all fields needed by watchdog to avoid second lookup.
 */
struct WatchdogEntry {
  int pid;
  int os_pid;  // ← NEW: Avoid get_process() call in watchdog
  std::string tree_id;
  int parent_pid;  // ← NEW: For termination notifications
  std::string trace_id;  // ← NEW: For BUS notifications
  uint64_t last_heartbeat_ms;
  uint64_t tree_created_at_ms;  // ← NEW: For tree runtime limit checks
  double memory_mb;
  ProcessStatus status;
  bool alive;
};

/**
 * ProcessRegistry: Single authoritative owner of all process and tree
 * structural state.
 *
 * Thread-safety:
 * - Structural state protected by shared_mutex registry_mutex
 * - Telemetry state uses atomic fields (lock-free)
 * - All structural modifications use unique_lock
 * - All structural reads use shared_lock
 *
 * Lifetime safety:
 * - All process_table entries are shared_ptr<ProcessInfo>
 * - Removal is safe; ProcessInfo may outlive registry entry
 *
 * Deadlock prevention:
 * - Never acquire registry_mutex while holding other locks
 * - Never perform OS operations while holding registry_mutex
 * - Snapshot operations release locks before processing
 */
class ProcessRegistry {
public:
  ProcessRegistry();
  ~ProcessRegistry() = default;

  // ─── Process Registration ──────────────────────────────────────────
  /**
   * Register a new process in the registry.
   *
   * Lock: unique_lock(registry_mutex)
   *
   * Returns:
   * - ProcessInfo structure with assigned pid, tree_id
   * - error string if registration fails
   */
  struct RegisterResult {
    bool success;
    std::string error;
    std::shared_ptr<ProcessInfo> process;
    std::string tree_id;
  };

  RegisterResult register_process(
      int os_pid,
      std::string_view tree_id,  // empty = create new tree
      int parent_pid,
      std::string_view role,
      std::string_view trace_id,
      ProcessStatus initial_status,
      double initial_memory_mb,
      bool is_system_handler);

  // ─── Process Lookup ────────────────────────────────────────────────
  /**
   * Get process by pid (lock-free safe).
   *
   * Lock: shared_lock(registry_mutex)
   *
   * Returns shared_ptr<ProcessInfo> or nullptr if not found.
   * Caller can access telemetry fields without locks.
   */
  std::shared_ptr<ProcessInfo> get_process(int pid);

  // ─── Heartbeat Updates (Lock-Free) ─────────────────────────────────
  /**
   * Update process telemetry atomically.
   *
   * Lock: NONE (uses atomic operations only)
   *
   * Safe to call from any thread without holding registry_mutex.
   */
  void update_heartbeat(int pid,
                        ProcessStatus status,
                        double memory_mb,
                        uint64_t now_ms);

  // ─── Tree Lookup ───────────────────────────────────────────────────
  /**
   * Get tree status.
   *
   * Lock: shared_lock(registry_mutex)
   */
  struct TreeStatusResult {
    bool found;
    TreeStatus status;
    int root_pid;
  };

  TreeStatusResult get_tree_status(std::string_view tree_id);

  /**
   * Get all processes in a tree.
   *
   * Lock: shared_lock(registry_mutex)
   *
   * Returns vector of pids only (not ProcessInfo).
   */
  std::vector<int> get_tree_processes(std::string_view tree_id);

  /**
   * Compute total memory usage for a tree (from live processes).
   *
   * Lock: shared_lock(registry_mutex) + read from atomic fields
   */
  double compute_tree_memory_mb(std::string_view tree_id);

  // ─── Watchdog Snapshots ────────────────────────────────────────────
  /**
   * Create a snapshot of all process telemetry for watchdog processing.
   *
   * Lock: shared_lock(registry_mutex) within function
   *       RELEASED before returning
   *
   * Returns vector<WatchdogEntry> excluding terminated processes.
   *
   * Purpose: Allow watchdog thread to process data without holding locks.
   */
  std::vector<WatchdogEntry> snapshot_for_watchdog();

  // ─── Tree Termination ──────────────────────────────────────────────
  /**
   * Mark a tree as killed and collect active pids.
   *
   * Lock: unique_lock(registry_mutex)
   *
   * Returns vector of pids to terminate (not including already-terminated).
   *
   * Important: Does NOT perform OS termination. Caller must use
   * TerminationEngine.
   */
  std::vector<int> kill_tree(std::string_view tree_id);

  // ─── Process Termination ───────────────────────────────────────────
  /**
   * Mark a process as killed.
   *
   * Lock: Can be called with or without lock held, depending on design
   *       Currently uses shared_lock just to read
   *
   * Sets status=KILLED and alive=false atomically.
   */
  void mark_process_killed(int pid);

  /**
   * Remove a process from the registry.
   *
   * Lock: unique_lock(registry_mutex)
   *
   * Must update all indexes (tree_process_index, process_children) in
   * same lock scope.
   *
   * IMPORTANT: Idempotent - safe to call multiple times for same pid.
   * Uses alive flag to detect if already terminated.
   */
  void terminate_process(int pid);

  // ─── Tree Status ───────────────────────────────────────────────────
  /**
   * Check if all processes in tree are terminal, and mark as COMPLETED
   * if so.
   *
   * Lock: unique_lock(registry_mutex)
   *
   * Returns true if tree was marked COMPLETED.
   */
  bool mark_tree_completed_if_done(std::string_view tree_id);

  /**
   * Mark a tree as FAILED.
   *
   * Lock: unique_lock(registry_mutex)
   */
  void mark_tree_failed(std::string_view tree_id);

  // ─── Tree Creation ─────────────────────────────────────────────────
  /**
   * Create a new tree and return its tree_id.
   *
   * Lock: unique_lock(registry_mutex)
   *
   * FIX #7: Added explicit tree_id parameter for clarity.
   * If empty string provided, auto-generates ID as "TREE_<counter>"
   */
  std::string create_tree(std::string_view explicit_id = "");

  // ─── Utilities ─────────────────────────────────────────────────────
  /**
   * Get current pid counter value (for testing/debugging).
   */
  uint64_t get_process_counter() const;

  /**
   * Get current tree counter value (for testing/debugging).
   */
  uint64_t get_tree_counter() const;

  /**
   * Get all tree IDs (for debugging).
   *
   * Lock: shared_lock(registry_mutex)
   */
  std::vector<std::string> get_all_tree_ids();

  /**
   * Get all tree IDs with creation times (FIX #6 - for watchdog iteration).
   *
   * Lock: shared_lock(registry_mutex)
   *
   * Returns vector of (tree_id, created_at_ms) pairs to avoid holding lock
   * during watchdog's tree iteration.
   */
  std::vector<std::pair<std::string, uint64_t>>
  get_all_tree_ids_with_created_at();

  /**
   * Get all process pids (for debugging).
   *
   * Lock: shared_lock(registry_mutex)
   */
  std::vector<int> get_all_pids();

private:
  // ─── Structural State (protected by mutex) ──────────────────────────
  mutable std::shared_mutex registry_mutex_;

  std::unordered_map<int, std::shared_ptr<ProcessInfo>> process_table_;
  std::unordered_map<std::string, TreeInfo> tree_table_;
  std::unordered_map<std::string, std::unordered_set<int>> tree_process_index_;
  std::unordered_map<int, std::unordered_set<int>> process_children_;

  // ─── Telemetry Counters (atomic) ────────────────────────────────────
  std::atomic<uint64_t> process_counter_{1000};
  std::atomic<uint64_t> tree_counter_{1};

  // ─── Helper methods (assumes caller holds appropriate lock) ─────────
  void on_process_registered_unlocked_(const ProcessInfo &process);
  void on_process_terminal_unlocked_(int pid);
};

}  // namespace velix::core
