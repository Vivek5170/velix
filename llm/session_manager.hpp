#pragma once

#include "../vendor/nlohmann/json.hpp"
#include "storage/istorage_provider.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace velix::llm {

class SessionIO;
namespace tools {
class ToolRegistry;
}

// ── SessionManager ─────────────────────────────────────────────────────────
//
// Manages multi-session lifecycle for super users (e.g. "terminal_vivek").
//
// Session ID convention  (auto-generated, immutable):
//   {super_user}_s{N}   e.g. "terminal_vivek_s1", "terminal_vivek_s2"
//
// Session title (optional, mutable, user-supplied display name):
//   Stored in session metadata; never affects the session_id or folder name.
//   An empty string means "untitled".
//
// Storage layout  (storage_root = "memory", configurable):
//   memory/sessions/users/{super_user}/{session_id}/
//     {session_id}.json        ← live conversation (SessionIO reads/writes)
//     {session_id}_h{n}.json   ← snapshot after n-th compact (1-indexed)
//   memory/sessions/procs/{pid}/{session_id}/
//     {session_id}.json
//     {session_id}_h{n}.json
//   memory/sessions/session_index.json
//     {
//       "users": {
//         "terminal_vivek": [
//           { "session_id": "terminal_vivek_s1", "title": "Debug run" },
//           { "session_id": "terminal_vivek_s2", "title": "" }
//         ]
//       }
//     }
//   memory/agentfiles/{super_user}/
//     soul.md        ← per-super-user persona  (read by SessionIO, unchanged)
//     user.md        ← per-super-user facts    (read by SessionIO, unchanged)
//
// Key invariants:
//   compact()    — does NOT change the session_id; only resets live content.
//   new_session()— allocates the next _s{N} and registers it in the index.
//   set_session_title() — updates the mutable title field only.
//   super_user names must never contain '_' (enforced by create_super_user).
//
class SessionManager {
 public:
  explicit SessionManager(
      const std::string& storage_root = "memory",
      std::shared_ptr<storage::IStorageProvider> storage_provider = nullptr);

  ~SessionManager()                               = default;
  SessionManager(const SessionManager&)           = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  // ── Session / super-user info types ─────────────────────────────────────

  // Live stats read from the {session_id}.json file on disk.
  // Populated by get_session_info() and get_super_user_info().
  struct SessionLiveStats {
    uint64_t current_context_tokens = 0;  // tokens in the active context window
    uint64_t total_tokens_used      = 0;  // cumulative tokens across the session
    int      turn_count             = 0;  // number of persisted turns
    bool     compacted              = false; // whether session has been compacted
  };

  struct SessionInfo {
    std::string      session_id;        // e.g. "terminal_vivek_s2"
    std::string      title;             // user-supplied display name, may be empty
    int              snapshot_count = 0;// number of {session_id}_h{n}.json files present
    SessionLiveStats live_stats;        // loaded from {session_id}.json on construction
  };

  struct SuperUserInfo {
    std::string              super_user;
    std::vector<SessionInfo> sessions;   // ordered by _sN ascending
  };

  struct SessionTarget {
    bool is_session_id = false;
    std::string super_user;
    std::string session_id;
  };

  // ── Compact result ────────────────────────────────────────────────────────

  struct CompactResult {
    std::string session_id;
    std::string summary;
    bool        compacted     = false;
    std::string compact_reason;
    int         tokens_before = 0;
    int         tokens_after  = 0;
  };

  struct AutoCompactGuardResult {
    bool threshold_exceeded = false;
    bool compact_attempted = false;
    bool compacted = false;
    std::string skip_reason;
    int tokens_before = 0;
    int tokens_after = 0;
  };

  struct ContextUsage {
    uint64_t session_tokens = 0;
    uint64_t system_prompt_tokens = 0;
    uint64_t tool_schema_tokens = 0;
    uint64_t request_tokens = 0;
    uint64_t total_context_tokens = 0;
    uint64_t max_context_tokens = 0;
    double context_fill_pct = 0.0;
  };

  // ── Public API ────────────────────────────────────────────────────────────

  /**
   * Ensure a super_user exists (directory + index entry created if missing).
   * super_user must not contain '_'.
   * Returns the super_user string on success; throws on invalid name.
   */
  std::string create_super_user(const std::string& super_user);

  /**
   * Returns the latest session_id for super_user, or creates _s1 if none.
   * Reply always includes {session_id, title}.
   */
  std::string get_or_create_active_session(const std::string& super_user);

  /**
   * Allocate the next session_id (_s{N+1}) for super_user, register in index.
   * Optional title stored immediately.
   * Returns the new session_id.
   */
  std::string new_session(const std::string& super_user,
                          const std::string& title = "");

  /**
   * Update the mutable display title for an existing session.
   * Session_id and folder are not touched.
   */
  void set_session_title(const std::string& session_id,
                         const std::string& new_title);

  /**
   * Delete a specific session id from index and local session directory.
   * Returns true when the session existed in index or on disk.
   */
  bool delete_session(const std::string& session_id);

  /**
   * Destroy a super-user identity and all sessions.
   * Returns true if anything was removed.
   */
  bool delete_super_user(const std::string& super_user);

  /**
   * Return full SessionInfo including title, snapshot_count, and live token
  * stats read from {session_id}.json. Does not throw if the live file is absent;
   * live_stats will be zero-initialised in that case.
   */
  SessionInfo get_session_info(const std::string& session_id) const;

  /**
   * Return info for every session belonging to super_user, ordered ascending.
   */
  SuperUserInfo get_super_user_info(const std::string& super_user) const;

  /** Build canonical session object used by control/query replies. */
  static json build_session_object(const SessionInfo& info,
                                   std::size_t max_ctx = 0);

  /** Convenience wrapper: load session info and return canonical json. */
  json get_session_object(const std::string& session_id,
                          std::size_t max_ctx = 0) const;

  /**
   * compact() — called for both manual /compact and auto-compact at 70 %.
   *
   * Steps:
   *   1. Run summary extraction via compacter.
    *   2. Save full history as {session_id}_h{n}.json (for session_search).
  *   3. Overwrite {session_id}.json with the pre-seeded synthetic tool-call history.
   */
  CompactResult compact(const std::string& session_id,
                        const json&        history,
                        bool               is_auto = false);

  /**
   * Pre-send hard guard for all conversation modes.
   * If estimated request context exceeds threshold, compaction is attempted.
   */
  AutoCompactGuardResult run_auto_compact_guard(
      const std::string& convo_id,
      const ContextUsage& usage,
      double auto_compact_threshold,
      SessionIO& session_io);

    ContextUsage compute_context_usage(const std::string& session_id,
                     const json& request_messages,
                     const tools::ToolRegistry& tools,
                     const std::string& mode,
                     std::size_t max_context_tokens) const;

  /**
   * List all super_users known to the index (union of index + filesystem scan).
   */
  std::vector<std::string> list_super_users() const;

  /**
   * List session_ids for super_user in ascending _sN order.
   */
  std::vector<std::string> list_sessions(const std::string& super_user) const;

  // ── Static helpers ────────────────────────────────────────────────────────

  /** "terminal_vivek_s3" → "terminal_vivek";  "terminal_vivek" → "terminal_vivek" */
  static std::string extract_super_user(const std::string& session_id);

  /** True iff session_id ends with _s{digits}. */
  static bool is_session_id(const std::string& s);

  /** Resolve incoming user_id to {super_user, optional session_id}. */
  static SessionTarget resolve_target(const std::string& user_id);

  // ── Path helpers (public so SessionIO and scheduler can locate files) ─────

  std::string session_dir(const std::string& session_id)        const;
  std::string live_convo_path(const std::string& session_id)    const;
  std::string history_snapshot_path(const std::string& session_id, int n) const;
  std::string agentfile_path(const std::string& super_user,
                             const std::string& filename)        const;

 private:
  std::string storage_root_;
  std::shared_ptr<storage::IStorageProvider> storage_provider_;

  mutable std::mutex index_mutex_;

  // ── Session index helpers ─────────────────────────────────────────────
  std::string index_path() const;
  json        load_index() const;          // acquires index_mutex_
  void        save_index(const json& idx); // caller must hold index_mutex_

  // Add or update an entry {session_id, title} in the index for super_user.
  void index_upsert_session(const std::string& super_user,
                            const std::string& session_id,
                            const std::string& title = "");

  // ── Session ID generation ─────────────────────────────────────────────
  std::string next_session_id(const std::string& super_user);

  // ── Snapshot helpers ───────────────────────────────────────────────────
  int  next_snapshot_index(const std::string& session_id) const;
  int  snapshot_count(const std::string& session_id) const;
  void save_snapshot(const std::string& session_id, const json& history);
  void write_seeded_history(const std::string& session_id,
                            const std::string& summary,
                            const json&        retained_recent = json::array());

  // ── Validation ────────────────────────────────────────────────────────
  static void validate_super_user_name(const std::string& super_user);
  static uint64_t estimate_tokens(const std::string& text);
  static uint64_t estimate_request_tokens(const json& request_messages);
};

}  // namespace velix::llm
