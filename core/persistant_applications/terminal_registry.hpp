#pragma once

/**
 * terminal_registry.hpp
 *
 * Owns all terminal sessions for the ApplicationManager.
 *
 * Sessions are keyed by (user_id, session_name).
 * The full session_id = "user_id:session_name".
 *
 * Each session holds:
 *   - DriverConfig used to create it (serialised to JSON for snapshots).
 *   - The TerminalDriver instance.
 *   - Metadata: created_at, last_used, status.
 *
 * Limits (configurable before first use):
 *   max_sessions_total    — hard ceiling across all users (default 64)
 *   max_sessions_per_user — per-user limit (default 8)
 *   idle_timeout_ms       — evict sessions idle longer than this (default 15 min)
 *   job_retention_ms      — prune completed job records older than this (default 60 min)
 *
 * Thread safety: all public methods are safe to call concurrently.
 *
 * Locking discipline:
 *   sessions_mx_ is a shared_mutex.
 *   Reads (snapshot, poll_job, exec) take shared_lock.
 *   Mutations (get_or_create, kill, evict) take unique_lock.
 *
 *   IMPORTANT: driver->close() is NEVER called while holding sessions_mx_.
 *   Drivers are moved out of the map and closed after releasing the lock,
 *   preventing a deadlock where driver->close() joining its reader thread
 *   (which might try to acquire some lock) blocks the registry lock.
 */

#include "terminal_driver.hpp"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace velix::app_manager {

// ─────────────────────────────────────────────────────────────────────────────
// SessionStatus
// ─────────────────────────────────────────────────────────────────────────────

enum class SessionStatus {
    Active,  // shell alive, accepting commands
    Dead,    // shell exited or was killed; will be evicted by watchdog
    Error,   // driver-level error on last operation
};

inline std::string to_string(SessionStatus s) {
    switch (s) {
    case SessionStatus::Active: return "active";
    case SessionStatus::Dead:   return "dead";
    case SessionStatus::Error:  return "error";
    }
    return "error";
}

// ─────────────────────────────────────────────────────────────────────────────
// SessionInfo — public snapshot (no driver internals exposed)
// ─────────────────────────────────────────────────────────────────────────────

struct SessionInfo {
    std::string   session_id;      // "user_id:session_name"
    std::string   user_id;
    std::string   session_name;
    std::string   driver_type;     // "local_pty" | "ssh" | "docker"
    SessionStatus status           = SessionStatus::Active;
    uint64_t      created_at_ms    = 0;
    uint64_t      last_used_ms     = 0;
    std::string   last_error_msg;
};

// ─────────────────────────────────────────────────────────────────────────────
// TerminalRegistry
// ─────────────────────────────────────────────────────────────────────────────

class TerminalRegistry {
public:
    // ── Configuration ────────────────────────────────────────────────────
    // Set before first use; not thread-safe to change after sessions exist.
    int      max_sessions_total    = 64;
    int      max_sessions_per_user = 8;
    uint64_t idle_timeout_ms       = 15ULL * 60 * 1000;   // 15 min
    uint64_t job_retention_ms      = 60ULL * 60 * 1000;   // 60 min

    // ── Session lifecycle ─────────────────────────────────────────────────

    struct GetOrCreateResult {
        bool        success      = false;
        bool        was_existing = false;
        std::string session_id;
        std::string error;
    };

    /**
     * get_or_create
     *
     * Returns a live session or creates one.
     * If a session exists but its driver is dead, it is evicted and
     * recreated transparently.
     *
     * cfg.type: "local_pty" (default), "ssh", "docker"
     * All connection parameters live in cfg.
     */
    GetOrCreateResult get_or_create(const std::string &user_id,
                                    const std::string &session_name,
                                    const DriverConfig &cfg);

    /**
     * exec
     *
     * Submits a command to the named session.
     * Returns job_id on success. Throws std::runtime_error on failure.
     */
    std::string exec(const std::string &user_id,
                     const std::string &session_name,
                     const std::string &cmd,
                     int                timeout_sec);

    /**
     * poll_job  — returns a snapshot or nullptr if job_id unknown.
     */
    std::shared_ptr<Job> poll_job(const std::string &job_id) const;

    /**
     * cancel_job
     *
     * Sends SIGINT (or Ctrl+C on Windows) to the currently running job
     * in the given session.
     */
    bool cancel_job(const std::string &user_id, const std::string &session_name);

    /**
     * kill_session
     *
     * Terminates one session (if session_name non-empty) or all sessions
     * for a user (if session_name empty).
     */
    void kill_session(const std::string &user_id,
                      const std::string &session_name = "");

    /**
     * kill_session_by_id — terminates a session by its full session_id.
     */
    void kill_session_by_id(const std::string &session_id);

    /**
     * snapshot — copy of all session metadata (thread-safe, no driver ptrs).
     */
    std::vector<SessionInfo> snapshot() const;
    std::vector<SessionInfo> snapshot(const std::string &user_id) const;

    // ── Watchdog helpers (called from ApplicationManager's watchdog loop) ──

    /**
     * detect_dead_sessions
     *
     * Marks sessions whose driver reports !is_alive() as SessionStatus::Dead.
     * Returns session_ids that were newly marked dead.
     */
    std::vector<std::string> detect_dead_sessions();

    /**
     * evict_dead_and_idle
     *
     * Removes Dead sessions and sessions idle longer than idle_timeout_ms.
     * Returns the count of evicted sessions.
     * NOTE: Calls driver->close() outside the registry lock to avoid deadlocks.
     */
    int evict_dead_and_idle();

    /**
     * prune_jobs — removes finished jobs older than job_retention_ms.
     */
    void prune_jobs();

    // Shared job store (all sessions write here).
    std::shared_ptr<JobStore> job_store() const { return job_store_; }

private:
    struct Session {
        SessionInfo                    info;
        std::shared_ptr<TerminalDriver> driver;
        DriverConfig                   cfg;   // kept for recreation on dead session
    };

    mutable std::shared_mutex sessions_mx_;
    std::unordered_map<std::string, Session> sessions_;

    std::shared_ptr<JobStore> job_store_ = std::make_shared<JobStore>();

    static uint64_t now_ms() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    static std::string normalize_session_name(const std::string &name) {
        return name.empty() ? "default" : name;
    }

    static std::string make_session_key(const std::string &user_id,
                                        const std::string &session_name) {
        return user_id + ":" + normalize_session_name(session_name);
    }

    // Caller must hold UNIQUE lock.
    // Removes session from map and returns the driver for close() outside lock.
    std::shared_ptr<TerminalDriver> extract_locked(const std::string &session_id);
};

} // namespace velix::app_manager