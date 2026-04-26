#include "process_registry.hpp"

#include <limits>
#include <sstream>

#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace velix::core {

ProcessRegistry::ProcessRegistry() = default;

ProcessRegistry::RegisterResult ProcessRegistry::register_process(
    int os_pid, std::string_view tree_id, int parent_pid, std::string_view role,
    std::string_view trace_id, ProcessStatus initial_status,
    double initial_memory_mb, bool is_system_handler) {
  std::unique_lock<std::shared_mutex> lock(registry_mutex_);

  // Generate new pid
  uint64_t pid_candidate = process_counter_.fetch_add(1);
  if (pid_candidate > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    return {false, "pid space exhausted", nullptr, ""};
  }
  int pid = static_cast<int>(pid_candidate);

  // Determine tree_id
  std::string final_tree_id(tree_id);
  bool is_tree_root = false;

  if (is_system_handler) {
    final_tree_id = "TREE_HANDLER";
    is_tree_root = true;
  } else if (final_tree_id.empty()) {
    // Create new tree
    // register_process already holds unique_lock(registry_mutex_)
    // so call internal unlocked helper to avoid double-locking.
    final_tree_id = create_tree_locked();
    is_tree_root = true;
  } else {
    // Verify tree exists
    if (auto tree_it = tree_table_.find(final_tree_id);
        tree_it == tree_table_.end()) {
      return {false, "tree not found", nullptr, final_tree_id};
    } else if (tree_it->second.status.load() != TreeStatus::ACTIVE) {
      return {false, "tree is not active", nullptr, final_tree_id};
    }
  }

  // Create ProcessInfo with atomic fields
  auto process_info = std::make_shared<ProcessInfo>();
  process_info->pid = pid;
  process_info->os_pid = os_pid;
  process_info->parent_pid = parent_pid;
  process_info->tree_id = final_tree_id;
  process_info->role = std::string(role);
  process_info->trace_id = std::string(trace_id);
  process_info->status.store(initial_status);
  process_info->memory_mb.store(initial_memory_mb);
  process_info->last_heartbeat_ms.store(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  process_info->alive.store(true);

  // Insert into registry
  process_table_[pid] = process_info;

  // Update indexes
  on_process_registered_unlocked_(*process_info);

  // Update tree root_pid if this is tree root registration
  if (auto tree_it = tree_table_.find(final_tree_id);
      tree_it != tree_table_.end() && is_tree_root) {
    tree_it->second.root_pid = pid;
  }

  return {true, "", process_info, final_tree_id};
}

std::shared_ptr<ProcessInfo> ProcessRegistry::get_process(int pid) {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);

  auto it = process_table_.find(pid);
  if (it == process_table_.end()) {
    return nullptr;
  }
  return it->second;
}

std::vector<int> ProcessRegistry::get_process_children(int pid) {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);

  std::vector<int> children;
  auto it = process_children_.find(pid);
  if (it != process_children_.end()) {
    children.reserve(it->second.size());
    for (int child : it->second) {
      children.push_back(child);
    }
  }

  return children;
}

std::vector<ProcessInfoSnapshot> ProcessRegistry::get_process_snapshot() {
  std::vector<ProcessInfoSnapshot> out;

  std::shared_lock<std::shared_mutex> lock(registry_mutex_);

  out.reserve(process_table_.size());

  for (const auto &[pid, proc] : process_table_) {
    ProcessInfoSnapshot s;
    s.pid = pid;
    s.os_pid = proc->os_pid;
    s.tree_id = proc->tree_id;
    s.parent_pid = proc->parent_pid;
    s.role = proc->role;
    s.status = proc->status.load();
    s.memory_mb = proc->memory_mb.load();
    out.push_back(std::move(s));
  }

  return out;
}

void ProcessRegistry::update_heartbeat(int pid, ProcessStatus status,
                                       double memory_mb, uint64_t now_ms) {
  // LOCK: NONE - this is intentionally lock-free
  // We get the shared_ptr outside to ensure it stays valid

  auto process = get_process(pid);
  if (!process || !process->alive.load()) {
    return;
  }

  // All atomic updates - lock-free
  process->status.store(status);
  process->memory_mb.store(memory_mb);
  process->last_heartbeat_ms.store(now_ms);
}

ProcessRegistry::TreeStatusResult
ProcessRegistry::get_tree_status(std::string_view tree_id) {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);

  const std::string tree_key(tree_id);
  auto it = tree_table_.find(tree_key);
  if (it == tree_table_.end()) {
    return {false, TreeStatus::FAILED, -1, 0};
  }

  return {true, it->second.status.load(), it->second.root_pid,
          it->second.llm_request_count.load()};
}

int ProcessRegistry::get_tree_llm_request_count(std::string_view tree_id) {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);
  auto it = tree_table_.find(std::string(tree_id));
  if (it == tree_table_.end()) {
    return 0;
  }
  return it->second.llm_request_count.load();
}

std::vector<int> ProcessRegistry::get_tree_processes(std::string_view tree_id) {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);

  std::vector<int> pids;
  const std::string tree_key(tree_id);
  auto index_it = tree_process_index_.find(tree_key);
  if (index_it != tree_process_index_.end()) {
    pids.reserve(index_it->second.size());
    for (int pid : index_it->second) {
      pids.push_back(pid);
    }
  }
  return pids;
}

size_t ProcessRegistry::get_tree_process_count(std::string_view tree_id) {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);

  auto it = tree_process_index_.find(std::string(tree_id));
  if (it == tree_process_index_.end()) {
    return 0;
  }

  return it->second.size();
}

double ProcessRegistry::compute_tree_memory_mb(std::string_view tree_id) {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);

  double total = 0.0;
  const std::string tree_key(tree_id);
  auto index_it = tree_process_index_.find(tree_key);
  if (index_it == tree_process_index_.end()) {
    return total;
  }

  for (int pid : index_it->second) {
    auto process_it = process_table_.find(pid);
    if (process_it != process_table_.end()) {
      // Read from atomic field - no additional lock needed
      total += process_it->second->memory_mb.load();
    }
  }
  return total;
}

std::vector<WatchdogEntry> ProcessRegistry::snapshot_for_watchdog() {
  std::vector<WatchdogEntry> snapshot;

  {
    std::shared_lock<std::shared_mutex> lock(registry_mutex_);

    snapshot.reserve(process_table_.size());
    for (const auto &[pid, process] : process_table_) {
      if (!process->alive.load()) {
        continue;
      }

      // FIX #3: Include ALL fields needed by watchdog to avoid second lookup
      WatchdogEntry entry;
      entry.pid = pid;
      entry.os_pid = process->os_pid; // ← NEW: Avoid get_process() in watchdog
      entry.tree_id = process->tree_id;
      entry.parent_pid = process->parent_pid; // ← NEW: For BUS notifications
      entry.trace_id = process->trace_id;     // ← NEW: For BUS notifications
      entry.last_heartbeat_ms = process->last_heartbeat_ms.load();
      entry.memory_mb = process->memory_mb.load();
      entry.status = process->status.load();
      entry.alive = true;

      // Get tree creation time for runtime limit checks
      auto tree_it = tree_table_.find(process->tree_id);
      entry.tree_created_at_ms =
          (tree_it != tree_table_.end()) ? tree_it->second.created_at_ms : 0;

      snapshot.push_back(entry);
    }
  }
  // Lock released here before returning

  return snapshot;
}

std::vector<int> ProcessRegistry::kill_tree(std::string_view tree_id) {
  std::unique_lock<std::shared_mutex> lock(registry_mutex_);

  std::vector<int> pids;
  const std::string tree_key(tree_id);

  auto tree_it = tree_table_.find(tree_key);
  if (tree_it != tree_table_.end()) {
    tree_it->second.status.store(TreeStatus::KILLED);
  }

  auto index_it = tree_process_index_.find(tree_key);
  if (index_it != tree_process_index_.end()) {
    for (int pid : index_it->second) {
      auto process_it = process_table_.find(pid);
      if (process_it != process_table_.end() &&
          process_it->second->alive.load()) {
        pids.push_back(pid);
        process_it->second->status.store(ProcessStatus::KILLED);
      }
    }
  }

  return pids;
}

void ProcessRegistry::mark_process_killed(int pid) {
  auto process = get_process(pid);
  if (!process) {
    return;
  }

  process->status.store(ProcessStatus::KILLED);
  process->alive.store(false);
}

void ProcessRegistry::terminate_process(int pid) {
  // FIX #1: Make idempotent - safe to call multiple times for same pid
  // Multiple heartbeats might race to terminate same process
  std::unique_lock<std::shared_mutex> lock(registry_mutex_);

  auto process_it = process_table_.find(pid);
  if (process_it == process_table_.end()) {
    return;
  }

  // Idempotency check: already terminated?
  if (!process_it->second->alive.exchange(false)) {
    // Was already dead - nothing to do
    return;
  }

  const std::string tree_id = process_it->second->tree_id;

  // Remove from tree index
  auto tree_index_it = tree_process_index_.find(tree_id);
  if (tree_index_it != tree_process_index_.end()) {
    tree_index_it->second.erase(pid);
    if (tree_index_it->second.empty()) {
      tree_process_index_.erase(tree_index_it);
    }
  }

  // Remove from parent's children
  int parent_pid = process_it->second->parent_pid;
  if (parent_pid > 0) {
    auto children_it = process_children_.find(parent_pid);
    if (children_it != process_children_.end()) {
      children_it->second.erase(pid);
      if (children_it->second.empty()) {
        process_children_.erase(children_it);
      }
    }
  }

  // Remove own children entry
  auto own_children_it = process_children_.find(pid);
  if (own_children_it != process_children_.end()) {
    process_children_.erase(own_children_it);
  }

  // Retention: Do NOT remove from process_table_ yet.
  // Instead mark termination time.
  process_it->second->termination_time_ms.store(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

void ProcessRegistry::increment_llm_request(std::string_view tree_id) {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);

  auto it = tree_table_.find(std::string(tree_id));
  if (it != tree_table_.end()) {
    it->second.llm_request_count.fetch_add(1);
  }
}

void ProcessRegistry::cleanup_historical_processes(uint64_t now_ms,
                                                   uint64_t retention_ms) {
  std::unique_lock<std::shared_mutex> lock(registry_mutex_);

  auto it = process_table_.begin();
  while (it != process_table_.end()) {
    auto &proc = it->second;
    if (!proc->alive.load()) {
      uint64_t term_time = proc->termination_time_ms.load();
      if (term_time > 0 && (now_ms - term_time) > retention_ms) {
        it = process_table_.erase(it);
        continue;
      }
    }
    ++it;
  }
}

bool ProcessRegistry::mark_tree_completed_if_done(std::string_view tree_id) {
  std::unique_lock<std::shared_mutex> lock(registry_mutex_);
  const std::string tree_key(tree_id);

  auto tree_it = tree_table_.find(tree_key);
  if (tree_it == tree_table_.end()) {
    return false;
  }

  // Don't mark system handler tree as completed
  if (tree_id == "TREE_HANDLER") {
    return false;
  }

  // Check if tree is already in terminal status
  TreeStatus status = tree_it->second.status.load();
  if (status == TreeStatus::COMPLETED || status == TreeStatus::FAILED ||
      status == TreeStatus::KILLED) {
    return false;
  }

  // Check if any processes remain
  auto index_it = tree_process_index_.find(tree_key);
  if (index_it != tree_process_index_.end() && !index_it->second.empty()) {
    return false;
  }

  // Check if all processes in tree are terminal
  for (const auto &[pid, process] : process_table_) {
    if (process->tree_id == tree_key) {
      ProcessStatus proc_status = process->status.load();
      if (proc_status != ProcessStatus::FINISHED &&
          proc_status != ProcessStatus::KILLED &&
          proc_status != ProcessStatus::ERROR) {
        return false;
      }
    }
  }

  // Tree is done - mark as completed
  tree_it->second.status.store(TreeStatus::COMPLETED);
  return true;
}

void ProcessRegistry::mark_tree_failed(std::string_view tree_id) {
  std::unique_lock<std::shared_mutex> lock(registry_mutex_);
  const std::string tree_key(tree_id);

  auto tree_it = tree_table_.find(tree_key);
  if (tree_it != tree_table_.end()) {
    TreeStatus current = tree_it->second.status.load();
    if (current != TreeStatus::KILLED && current != TreeStatus::COMPLETED &&
        current != TreeStatus::FAILED) {
      tree_it->second.status.store(TreeStatus::FAILED);
    }
  }
}

std::string ProcessRegistry::create_tree(std::string_view explicit_id) {
  // Acquire exclusive lock for tree creation/modification
  std::unique_lock<std::shared_mutex> lock(registry_mutex_);
  return create_tree_locked(explicit_id);
}

std::string ProcessRegistry::create_tree_locked(std::string_view explicit_id) {
  // Caller MUST hold unique_lock(registry_mutex_)
  std::string tree_id;
  if (!explicit_id.empty()) {
    tree_id = std::string(explicit_id);
  } else {
    std::ostringstream oss;
    oss << "TREE_" << tree_counter_++;
    tree_id = oss.str();
  }

  if (auto tree_it = tree_table_.find(tree_id); tree_it == tree_table_.end()) {
    TreeInfo tree_info;
    tree_info.tree_id = tree_id;
    tree_info.root_pid = -1;
    tree_info.created_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    tree_info.status.store(TreeStatus::ACTIVE);
    tree_info.llm_request_count.store(0);
    tree_table_.try_emplace(tree_id, tree_info);
  } else {
    tree_it->second.tree_id = tree_id;
    tree_it->second.root_pid = -1;
    tree_it->second.created_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    tree_it->second.status.store(TreeStatus::ACTIVE);
    tree_it->second.llm_request_count.store(0);
  }

  return tree_id;
}

uint64_t ProcessRegistry::get_process_counter() const {
  return process_counter_.load();
}

uint64_t ProcessRegistry::get_tree_counter() const {
  return tree_counter_.load();
}

std::vector<std::string> ProcessRegistry::get_all_tree_ids() {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);

  std::vector<std::string> ids;
  ids.reserve(tree_table_.size());
  for (const auto &[id, _tree] : tree_table_) {
    ids.push_back(id);
  }
  return ids;
}

std::vector<std::pair<std::string, uint64_t>>
ProcessRegistry::get_all_tree_ids_with_created_at() {
  // FIX #6: Return vector of pairs (tree_id, created_at_ms) so watchdog
  // doesn't need to call get_tree_status for each tree
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);

  std::vector<std::pair<std::string, uint64_t>> pairs;
  pairs.reserve(tree_table_.size());
  for (const auto &[id, tree] : tree_table_) {
    pairs.emplace_back(id, tree.created_at_ms);
  }
  return pairs;
}

std::vector<int> ProcessRegistry::get_all_pids() {
  std::shared_lock<std::shared_mutex> lock(registry_mutex_);

  std::vector<int> pids;
  pids.reserve(process_table_.size());
  for (const auto &[pid, _process] : process_table_) {
    pids.push_back(pid);
  }
  return pids;
}

// ──────────────────────────────────────────────────────────────────────────
// Private helper methods
// ──────────────────────────────────────────────────────────────────────────

void ProcessRegistry::on_process_registered_unlocked_(
    const ProcessInfo &process) {
  tree_process_index_[process.tree_id].insert(process.pid);
  if (process.parent_pid > 0) {
    process_children_[process.parent_pid].insert(process.pid);
  }
}

void ProcessRegistry::on_process_terminal_unlocked_(int pid) {
  auto process_it = process_table_.find(pid);
  if (process_it == process_table_.end()) {
    return;
  }

  const std::string tree_id = process_it->second->tree_id;

  // Remove from tree index
  auto tree_index_it = tree_process_index_.find(tree_id);
  if (tree_index_it != tree_process_index_.end()) {
    tree_index_it->second.erase(pid);
    if (tree_index_it->second.empty()) {
      tree_process_index_.erase(tree_index_it);
    }
  }

  // Remove from parent's children
  int parent_pid = process_it->second->parent_pid;
  if (parent_pid > 0) {
    auto children_it = process_children_.find(parent_pid);
    if (children_it != process_children_.end()) {
      children_it->second.erase(pid);
      if (children_it->second.empty()) {
        process_children_.erase(children_it);
      }
    }
  }

  // Remove own children entry
  auto own_children_it = process_children_.find(pid);
  if (own_children_it != process_children_.end()) {
    process_children_.erase(own_children_it);
  }
}

} // namespace velix::core
