#include "session_manager.hpp"
#include "../utils/logger.hpp"
#include "compacter.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace velix::llm {

// ── Construction ──────────────────────────────────────────────────────────

SessionManager::SessionManager(const std::string &storage_root)
    : storage_root_(storage_root) {
  // Session directories are created lazily per session.
  // Only taxonomy roots are pre-created.
  try {
    fs::create_directories(storage_root_ + "/sessions/users");
    fs::create_directories(storage_root_ + "/sessions/procs");
    fs::create_directories(storage_root_ + "/agentfiles");
  } catch (const std::exception &e) {
    LOG_ERROR_CTX(std::string("SessionManager init failed: ") + e.what(),
                  "session_mgr", "", -1, "init_error");
  }
}

// ── Static helpers ────────────────────────────────────────────────────────

std::string SessionManager::extract_super_user(const std::string &session_id) {
  // "terminal_vivek_s3" → "terminal_vivek"
  // "terminal_vivek"    → "terminal_vivek"
  const auto pos = session_id.rfind("_s");
  if (pos != std::string::npos && pos + 2 < session_id.size()) {
    const std::string suffix = session_id.substr(pos + 2);
    const bool numeric_suffix = !suffix.empty() &&
                                std::all_of(suffix.begin(), suffix.end(),
                                            [](unsigned char ch) {
                                              return std::isdigit(ch) != 0;
                                            });
    if (numeric_suffix) {
      return session_id.substr(0, pos);
    }
  }
  return session_id;
}

bool SessionManager::is_session_id(const std::string &s) {
  return extract_super_user(s) != s;
}

// ── Path helpers ──────────────────────────────────────────────────────────

std::string SessionManager::session_dir(const std::string &session_id) const {
  if (session_id.rfind("proc_", 0) == 0) {
    const std::string rest = session_id.substr(5);
    const auto sep = rest.find('_');
    if (sep != std::string::npos) {
      const std::string pid = rest.substr(0, sep);
      return storage_root_ + "/sessions/procs/" + pid + "/" + session_id;
    }
  }

  const std::string super_user = extract_super_user(session_id);
  return storage_root_ + "/sessions/users/" + super_user + "/" + session_id;
}

std::string
SessionManager::live_convo_path(const std::string &session_id) const {
  const std::string dir = session_dir(session_id);
  const std::string preferred = dir + "/" + session_id + ".json";

  if (fs::exists(preferred)) {
    return preferred;
  }

  if (fs::exists(dir)) {
    for (const auto &entry : fs::directory_iterator(dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      if (entry.path().extension() != ".json") {
        continue;
      }
      const std::string stem = entry.path().stem().string();
      if (stem.find("_h") == std::string::npos) {
        return entry.path().string();
      }
    }
  }

  return preferred;
}

std::string SessionManager::history_snapshot_path(const std::string &session_id,
                                                  int n) const {
  return session_dir(session_id) + "/" + session_id + "_h" + std::to_string(n) +
         ".json";
}

std::string SessionManager::agentfile_path(const std::string &super_user,
                                           const std::string &filename) const {
  return storage_root_ + "/agentfiles/" + super_user + "/" + filename;
}

std::string SessionManager::index_path() const {
  return storage_root_ + "/sessions/session_index.json";
}

// ── Session index ─────────────────────────────────────────────────────────

json SessionManager::load_index() const {
  std::lock_guard<std::mutex> lk(index_mutex_);
  if (!fs::exists(index_path()))
    return json{{"users", json::object()}};
  try {
    std::ifstream f(index_path());
    json j;
    f >> j;
    return j;
  } catch (...) {
    return json{{"users", json::object()}};
  }
}

void SessionManager::save_index(const json &idx) {
  std::ofstream f(index_path());
  if (f.is_open())
    f << idx.dump(2);
}

void SessionManager::index_add_session(const std::string &super_user,
                                       const std::string &session_id) {
  std::lock_guard<std::mutex> lk(index_mutex_);
  json idx = [&]() {
    if (!fs::exists(index_path()))
      return json{{"users", json::object()}};
    try {
      std::ifstream f(index_path());
      json j;
      f >> j;
      return j;
    } catch (...) {
      return json{{"users", json::object()}};
    }
  }();

  if (!idx.contains("users") || !idx["users"].is_object())
    idx["users"] = json::object();
  if (!idx["users"].contains(super_user) ||
      !idx["users"][super_user].is_array())
    idx["users"][super_user] = json::array();

  // De-duplicate.
  for (const auto &s : idx["users"][super_user]) {
    if (s.is_string() && s.get<std::string>() == session_id) {
      save_index(idx);
      return;
    }
  }
  idx["users"][super_user].push_back(session_id);
  save_index(idx);
}

// ── Session ID generation ─────────────────────────────────────────────────

std::string SessionManager::next_session_id(const std::string &super_user) {
  // Find max N among existing sessions for this super_user, return _s{N+1}.
  const auto sessions = list_sessions(super_user);
  int max_n = 0;
  for (const auto &sid : sessions) {
    const auto pos = sid.rfind("_s");
    if (pos == std::string::npos)
      continue;
    try {
      const int n = std::stoi(sid.substr(pos + 2));
      if (n > max_n)
        max_n = n;
    } catch (...) {
    }
  }
  return super_user + "_s" + std::to_string(max_n + 1);
}

// ── get_or_create_active_session ──────────────────────────────────────────

std::string
SessionManager::get_or_create_active_session(const std::string &super_user) {
  const auto sessions = list_sessions(super_user);
  if (!sessions.empty()) {
    // Return the last (most recently created) session.
    return sessions.back();
  }
  // No sessions yet — create _s1.
  const std::string sid = super_user + "_s1";
  fs::create_directories(session_dir(sid));
  index_add_session(super_user, sid);
  LOG_INFO_CTX("Created first session " + sid, "session_mgr", "", -1,
               "session_create");
  return sid;
}

// ── new_session ───────────────────────────────────────────────────────────

std::string SessionManager::new_session(const std::string &super_user) {
  const std::string sid = next_session_id(super_user);
  fs::create_directories(session_dir(sid));
  index_add_session(super_user, sid);
  LOG_INFO_CTX("New session " + sid, "session_mgr", "", -1, "session_new");
  return sid;
}

// ── list_sessions & list_super_users ──────────────────────────────────────

std::vector<std::string> SessionManager::list_super_users() const {
  std::unordered_set<std::string> users;

  const json idx = load_index();
  if (idx.contains("users") && idx["users"].is_object()) {
    for (auto it = idx["users"].begin(); it != idx["users"].end(); ++it) {
      users.insert(it.key());
    }
  }

  const fs::path users_root = fs::path(storage_root_) / "sessions" / "users";
  if (fs::exists(users_root) && fs::is_directory(users_root)) {
    for (const auto &entry : fs::directory_iterator(users_root)) {
      if (!entry.is_directory()) {
        continue;
      }
      const std::string super_user = entry.path().filename().string();
      if (!super_user.empty()) {
        users.insert(super_user);
      }
    }
  }

  std::vector<std::string> out(users.begin(), users.end());
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string>
SessionManager::list_sessions(const std::string &super_user) const {
  std::unordered_set<std::string> sessions_set;

  const json idx = load_index();
  if (idx.contains("users") && idx["users"].is_object()) {
    const json &users = idx["users"];
    if (users.contains(super_user) && users[super_user].is_array()) {
      for (const auto &s : users[super_user]) {
        if (s.is_string()) {
          sessions_set.insert(s.get<std::string>());
        }
      }
    }
  }

  const fs::path user_root =
      fs::path(storage_root_) / "sessions" / "users" / super_user;
  if (fs::exists(user_root) && fs::is_directory(user_root)) {
    for (const auto &entry : fs::directory_iterator(user_root)) {
      if (!entry.is_directory()) {
        continue;
      }
      const std::string sid = entry.path().filename().string();
      if (extract_super_user(sid) == super_user && is_session_id(sid)) {
        sessions_set.insert(sid);
      }
    }
  }

  std::vector<std::string> out(sessions_set.begin(), sessions_set.end());
  std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) {
    const auto pa = a.rfind("_s");
    const auto pb = b.rfind("_s");
    int na = 0;
    int nb = 0;
    try {
      if (pa != std::string::npos) {
        na = std::stoi(a.substr(pa + 2));
      }
      if (pb != std::string::npos) {
        nb = std::stoi(b.substr(pb + 2));
      }
    } catch (...) {
    }
    if (na != nb) {
      return na < nb;
    }
    return a < b;
  });
  return out;
}

// ── History snapshot ──────────────────────────────────────────────────────

int SessionManager::next_snapshot_index(const std::string &session_id) const {
  int n = 1;
  while (fs::exists(history_snapshot_path(session_id, n)))
    ++n;
  return n;
}

void SessionManager::save_snapshot(const std::string &session_id,
                                   const json &history) {
  const int n = next_snapshot_index(session_id);
  const std::string path = history_snapshot_path(session_id, n);

  // Capture wall-clock time as snapshot metadata.
  const long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

  json snap;
  snap["session_id"] = session_id;
  snap["snapshot_n"] = n;
  snap["snapshot_ms"] = now_ms;
  snap["messages"] = history;

  std::ofstream f(path);
  if (f.is_open())
    f << snap.dump(2);
  LOG_INFO_CTX("Saved snapshot " + path, "session_mgr", "", -1,
               "session_snapshot");
}

// ── Synthetic pre-seeded history ──────────────────────────────────────────

void SessionManager::write_seeded_history(const std::string &session_id,
                                          const std::string &summary,
                                          bool add_continue_turn,
                                          const json &retained_recent) {
  // Synthetic tool-call history injected at the start of the reset session.
  // No actual skill is executed — the summary is already known.
  //
  // user  → "retrieve previous continuation session summary"
  // asst  → tool_call(session_search, {"query":"continue previous session"})
  // tool  → "[PREVIOUS SESSION SUMMARY]\n{summary}"
  // user  → "continue..."   (only when is_auto=true / add_continue_turn=true)
  //
  json messages = json::array();

  messages.push_back(
      {{"role", "user"},
       {"content", "retrieve previous continuation session summary"}});

  messages.push_back(
      {{"role", "assistant"},
       {"content", ""},
       {"tool_calls",
        json::array({json::object(
            {{"id", "tc_compact_001"},
             {"type", "function"},
             {"function",
              {{"name", "session_search"},
               {"arguments",
                "{\"query\": \"continue previous session\"}"}}}})})}});

  messages.push_back({{"role", "tool"},
                      {"tool_call_id", "tc_compact_001"},
                      {"content", "[PREVIOUS SESSION SUMMARY]\n" + summary}});

  // Preserve the most recent real turns so runtime/tool configuration context
  // remains available immediately after compact.
  if (retained_recent.is_array()) {
    for (const auto &m : retained_recent) {
      if (m.is_object()) {
        messages.push_back(m);
      }
    }
  }

  if (add_continue_turn) {
    messages.push_back({{"role", "user"}, {"content", "continue..."}});
  }

  // Write as a bare messages array into the live conversation file.
  // SessionIO will load this as the starting history on next access.
  uint64_t seeded_tokens = 0;
  for (const auto &m : messages) {
    if (!m.is_object()) {
      continue;
    }
    seeded_tokens +=
      static_cast<uint64_t>(m.value("content", std::string("")).size() / 4);
  }

  json convo;
  convo["messages"] = messages;
  convo["convo_id"] = session_id;
  convo["compacted"] = true;
  convo["turn_count"] = static_cast<int>(messages.size());
  convo["current_context_tokens"] = seeded_tokens;

  const std::string path = live_convo_path(session_id);
  fs::create_directories(session_dir(session_id));
  std::ofstream f(path);
  if (f.is_open())
    f << convo.dump(2);
  LOG_INFO_CTX("Pre-seeded history for " + session_id, "session_mgr", "", -1,
               "session_seed");
}

// ── compact ───────────────────────────────────────────────────────────────

SessionManager::CompactResult
SessionManager::compact(const std::string &session_id, const json &history,
                        bool is_auto) {

  CompactResult result;
  result.session_id = session_id;
  result.compacted = false;
  result.compact_reason = "";
  result.tokens_before = 0;
  result.tokens_after = 0;

  // Estimate tokens before (chars / 4 approximation).
  for (const auto &m : history) {
    if (m.is_object())
      result.tokens_before +=
          static_cast<int>(m.value("content", std::string("")).size()) / 4;
  }

  // 1. Run compacter and only reset/snapshot when compaction is applicable.
  std::string summary;
  json retained_recent = json::array();
  if (!history.empty()) {
    // compact_history_if_needed comes from compacter.hpp / compacter.cpp.
    // It takes a mutable history array and a path hint.
    json mutable_history = history;
    const ::velix::llm::CompactResult cr =
        compact_history_if_needed(mutable_history, live_convo_path(session_id));

    if (!cr.compacted) {
      result.compacted = false;
      result.compact_reason = cr.skip_reason.empty()
                                  ? "below_compaction_limit"
                                  : cr.skip_reason;
      result.tokens_after = result.tokens_before;
      LOG_INFO_CTX("Compact skipped for " + session_id + " reason=" +
                       result.compact_reason,
                   "session_mgr", "", -1, "session_compact_skipped");
      return result;
    }

    result.compacted = true;

    // Keep the compacter's recent tail (excluding synthetic summary marker)
    // so the next turn retains exact short-term configuration/tool context.
    if (cr.history.is_array()) {
      for (const auto &m : cr.history) {
        if (!m.is_object()) {
          continue;
        }
        const std::string content = m.value("content", "");
        if (content.find("[CONTEXT COMPACTION]") != std::string::npos ||
            content.find("[CONTEXT SUMMARY]") != std::string::npos) {
          continue;
        }
        retained_recent.push_back(m);
      }
    }

    // Prefer the explicit summary field.
    summary = cr.summary;

    // Fallback: scan compacted messages for [CONTEXT COMPACTION] marker.
    if (summary.empty()) {
      for (const auto &m : cr.history) {
        if (!m.is_object())
          continue;
        const std::string content = m.value("content", "");
        if (content.find("[CONTEXT COMPACTION]") != std::string::npos ||
            content.find("[CONTEXT SUMMARY]") != std::string::npos) {
          const auto nl = content.find('\n');
          summary =
              (nl != std::string::npos) ? content.substr(nl + 1) : content;
          break;
        }
      }
    }
  }

  if (summary.empty()) {
    summary = "(No summary available — conversation was short or empty.)";
  }

  if (retained_recent.empty() && history.is_array()) {
    // Fallback for short conversations where compacter doesn't emit a tail.
    constexpr std::size_t kFallbackKeep = 6;
    const std::size_t start =
        history.size() > kFallbackKeep ? history.size() - kFallbackKeep : 0;
    for (std::size_t i = start; i < history.size(); ++i) {
      if (history[i].is_object()) {
        retained_recent.push_back(history[i]);
      }
    }
  }

  if (is_auto && retained_recent.is_array() && !retained_recent.empty()) {
    const auto &last = retained_recent.back();
    if (last.is_object() && last.value("role", "") == "user") {
      retained_recent.erase(retained_recent.end() - 1);
    }
  }

  result.summary = summary;

  // 2. Save snapshot for session_search (only when compaction actually happens).
  if (!history.empty()) {
    save_snapshot(session_id, history);
  }

  // 3. Overwrite the live conversation file with the pre-seeded history.
  // The session_id stays the same — only the content is reset.
  write_seeded_history(session_id, summary, is_auto, retained_recent);

  // Estimate tokens after (the pre-seeded history is small).
  // user(~6w) + assistant(tool_call~10w) + tool(summary) ≈ summary/4 + 50
  result.tokens_after =
      static_cast<int>(summary.size()) / 4 + (is_auto ? 60 : 50);

  LOG_INFO_CTX("Compact " + session_id + " tokens " +
                   std::to_string(result.tokens_before) + " → " +
                   std::to_string(result.tokens_after),
               "session_mgr", "", -1, "session_compact");

  return result;
}

} // namespace velix::llm
