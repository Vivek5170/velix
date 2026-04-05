#pragma once

#include "../vendor/nlohmann/json.hpp"

#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace velix::llm {

// ── SessionManager ─────────────────────────────────────────────────────────
//
// Manages multi-session lifecycle for super users (e.g. "terminal_vivek").
//
// Session ID convention:
//   {super_user}_s{N}   e.g. "terminal_vivek_s1", "terminal_vivek_s2"
//
// Storage layout  (storage_root = "memory", configurable):
//   memory/sessions/{session_id}/
//     {session_id}.json       ← live conversation file (ConversationManager)
//     {session_id}_h1.json    ← history snapshot after 1st compact
//     {session_id}_h2.json    ← history snapshot after 2nd compact
//   memory/session_index.json ← {"users":{"terminal_vivek":["t_v_s1","t_v_s2"]}}
//   memory/agentfiles/{super_user}/
//     soul.md                 ← per-super-user persona
//     user.md                 ← per-super-user persistent facts
//
// Key invariant:
//   compact() does NOT change the session_id.
//   It saves a _h{N} snapshot and RESETS the live conversation.
//   Only new_session() creates a new session_id.
//   The handler never needs to re-key its sessions map after compact().
//
class SessionManager {
 public:
  explicit SessionManager(const std::string& storage_root = "memory");

  ~SessionManager()                             = default;
  SessionManager(const SessionManager&)         = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  // ── Public API ──────────────────────────────────────────────────────────

  /**
   * Returns the latest session_id for super_user from the index,
   * or creates "super_user_s1" if none exists.
   */
  std::string get_or_create_active_session(const std::string& super_user);

  /**
   * /new — allocate the next session_id (e.g. _s2) for super_user,
   * register it in the index, and return it.
   * No history is touched. The session folder is created lazily on first write.
   */
  std::string new_session(const std::string& super_user);

  /**
   * compact() — called for both manual /compact and auto-compact at 70%.
   *
   * Given the current live history of session_id:
   *   1. Run summary extraction (via compacter).
   *   2. Save full history as {session_id}_h{N}.json (for session_search).
   *   3. Overwrite {session_id}.json with the pre-seeded synthetic tool-call
   *      history so the LLM picks up context on the next turn:
   *        user  → "retrieve previous continuation session summary"
   *        asst  → tool_call(session_search, {"query": "continue previous session"})
   *        tool  → "[PREVIOUS SESSION SUMMARY]\n{summary}"
   *      For auto-compact (is_auto=true) one extra turn is appended:
   *        user  → "continue..."
   *      The scheduler then immediately continues with the original user message.
   *
   * Returns the session_id (unchanged) and the extracted summary text,
   * plus token counts for the LLM_RESPONSE fields.
   */
  struct CompactResult {
    std::string session_id;    // same as input — no session switch
    std::string summary;
    int         tokens_before = 0;
    int         tokens_after  = 0;
  };
  CompactResult compact(const std::string& session_id,
                        const json&         history,
                        bool                is_auto = false);


  /**
   * List all super_users known to the index.
   */
  std::vector<std::string> list_super_users() const;


  /**
   * List all session_ids for super_user in insertion order.
   */
  std::vector<std::string> list_sessions(const std::string& super_user) const;

  // ── Static helpers ───────────────────────────────────────────────────────

  /**
   * "terminal_vivek_s3" → "terminal_vivek"
   * "terminal_vivek"    → "terminal_vivek"  (already a super_user)
   */
  static std::string extract_super_user(const std::string& session_id);

  /**
   * Returns true if session_id ends with _s{one or more digits}.
   */
  static bool is_session_id(const std::string& s);

  // ── Path helpers (public so scheduler can locate the live file) ──────────
  std::string session_dir(const std::string& session_id) const;
  std::string live_convo_path(const std::string& session_id) const;
  std::string history_snapshot_path(const std::string& session_id, int n) const;
  std::string agentfile_path(const std::string& super_user,
                             const std::string& filename) const;

 private:
  std::string storage_root_ = "memory";

  mutable std::mutex index_mutex_;

  // ── Session index ─────────────────────────────────────────────────────
  std::string index_path() const;
  json        load_index() const;            // acquires index_mutex_
  void        save_index(const json& idx);   // caller must hold index_mutex_
  void        index_add_session(const std::string& super_user,
                                const std::string& session_id);

  // ── Session ID generation ──────────────────────────────────────────────
  // Returns the highest N seen in the index for super_user, then adds 1.
  std::string next_session_id(const std::string& super_user);

  // ── History snapshot ───────────────────────────────────────────────────
  int  next_snapshot_index(const std::string& session_id) const;
  void save_snapshot(const std::string& session_id, const json& history);
  void write_seeded_history(const std::string& session_id,
                            const std::string& summary,
                            bool               add_continue_turn);

};

}  // namespace velix::llm
