#include "session_manager.hpp"
#include "session_io.hpp"
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
  try {
    fs::create_directories(storage_root_ + "/sessions/users");
    fs::create_directories(storage_root_ + "/sessions/procs");
    fs::create_directories(storage_root_ + "/agentfiles");
  } catch (const std::exception &e) {
    LOG_ERROR_CTX(std::string("SessionManager init failed: ") + e.what(),
                  "session_mgr", "", -1, "init_error");
  }
}

void SessionManager::validate_super_user_name(const std::string &super_user) {
  if (super_user.empty()) {
    throw std::runtime_error("super_user name must not be empty");
  }
  for (char c : super_user) {
    if (c == '_') {
      throw std::runtime_error("super_user name must not contain '_'");
    }
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') {
      throw std::runtime_error(
          "super_user name must be alphanumeric (hyphens allowed)");
    }
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

SessionManager::SessionTarget
SessionManager::resolve_target(const std::string &user_id) {
  SessionTarget target;
  target.is_session_id = is_session_id(user_id);
  target.super_user =
      target.is_session_id ? extract_super_user(user_id) : user_id;
  target.session_id = target.is_session_id ? user_id : std::string("");
  return target;
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
  return session_dir(session_id) + "/active.json";
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
  fs::create_directories(fs::path(index_path()).parent_path());
  std::ofstream f(index_path());
  if (f.is_open())
    f << idx.dump(2);
}

void SessionManager::index_upsert_session(const std::string &super_user,
                                          const std::string &session_id,
                                          const std::string &title) {
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

  for (auto &entry : idx["users"][super_user]) {
    if (entry.is_object() && entry.value("session_id", "") == session_id) {
      if (!title.empty()) {
        entry["title"] = title;
      }
      save_index(idx);
      return;
    }
    if (entry.is_string() && entry.get<std::string>() == session_id) {
      entry = json{{"session_id", session_id}, {"title", title}};
      save_index(idx);
      return;
    }
  }

  idx["users"][super_user].push_back(
      json{{"session_id", session_id}, {"title", title}});
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
  index_upsert_session(super_user, sid, "");
  LOG_INFO_CTX("Created first session " + sid, "session_mgr", "", -1,
               "session_create");
  return sid;
}

// ── new_session ───────────────────────────────────────────────────────────

std::string SessionManager::new_session(const std::string &super_user,
                                        const std::string &title) {
  validate_super_user_name(super_user);
  const std::string sid = next_session_id(super_user);
  fs::create_directories(session_dir(sid));
  index_upsert_session(super_user, sid, title);
  LOG_INFO_CTX("New session " + sid, "session_mgr", "", -1, "session_new");
  return sid;
}

std::string SessionManager::create_super_user(const std::string &super_user) {
  validate_super_user_name(super_user);
  fs::create_directories(storage_root_ + "/sessions/users/" + super_user);
  fs::create_directories(storage_root_ + "/agentfiles/" + super_user);

  {
    std::lock_guard<std::mutex> lk(index_mutex_);
    json idx = json{{"users", json::object()}};
    if (fs::exists(index_path())) {
      try {
        std::ifstream f(index_path());
        f >> idx;
      } catch (...) {
        idx = json{{"users", json::object()}};
      }
    }
    if (!idx.contains("users") || !idx["users"].is_object()) {
      idx["users"] = json::object();
    }
    if (!idx["users"].contains(super_user)) {
      idx["users"][super_user] = json::array();
    }
    save_index(idx);
  }

  return super_user;
}

void SessionManager::set_session_title(const std::string &session_id,
                                       const std::string &new_title) {
  if (!is_session_id(session_id)) {
    throw std::runtime_error("set_session_title requires session_id");
  }
  index_upsert_session(extract_super_user(session_id), session_id, new_title);
}

SessionManager::SessionInfo
SessionManager::get_session_info(const std::string &session_id) const {
  SessionInfo info;
  info.session_id = session_id;

  const std::string super_user = extract_super_user(session_id);
  const json idx = load_index();
  if (idx.contains("users") && idx["users"].is_object() &&
      idx["users"].contains(super_user) &&
      idx["users"][super_user].is_array()) {
    for (const auto &entry : idx["users"][super_user]) {
      if (entry.is_object() && entry.value("session_id", "") == session_id) {
        info.title = entry.value("title", "");
        break;
      }
    }
  }

  const std::string live = live_convo_path(session_id);
  if (fs::exists(live)) {
    try {
      std::ifstream f(live);
      json convo;
      f >> convo;
      info.live_stats.current_context_tokens =
          convo.value("current_context_tokens", static_cast<uint64_t>(0));
      info.live_stats.total_tokens_used =
          convo.value("total_tokens_used", static_cast<uint64_t>(0));
      info.live_stats.turn_count = convo.value("turn_count", 0);
      info.live_stats.compacted = convo.value("compacted", false);
    } catch (...) {
    }
  }

  info.snapshot_count = snapshot_count(session_id);
  return info;
}

SessionManager::SuperUserInfo
SessionManager::get_super_user_info(const std::string &super_user) const {
  SuperUserInfo out;
  out.super_user = super_user;
  for (const auto &sid : list_sessions(super_user)) {
    out.sessions.push_back(get_session_info(sid));
  }
  return out;
}

json SessionManager::build_session_object(const SessionInfo &info,
                                          std::size_t max_ctx) {
  json session;
  session["session_id"] = info.session_id;
  session["title"] = info.title;
  session["current_context_tokens"] = info.live_stats.current_context_tokens;
  session["total_tokens_used"] = info.live_stats.total_tokens_used;
  session["turn_count"] = info.live_stats.turn_count;
  session["snapshot_count"] = info.snapshot_count;
  session["compacted"] = info.live_stats.compacted;

  if (max_ctx > 0) {
    session["max_context_tokens"] = max_ctx;
    const double fill =
        static_cast<double>(info.live_stats.current_context_tokens) /
        static_cast<double>(max_ctx) * 100.0;
    session["context_fill_pct"] =
        static_cast<double>(static_cast<int>(fill * 10.0)) / 10.0;
  }

  return session;
}

json SessionManager::get_session_object(const std::string &session_id,
                                        std::size_t max_ctx) const {
  return build_session_object(get_session_info(session_id), max_ctx);
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
      for (const auto &entry : users[super_user]) {
        if (entry.is_string()) {
          sessions_set.insert(entry.get<std::string>());
        }
        if (entry.is_object()) {
          const std::string sid = entry.value("session_id", "");
          if (!sid.empty()) {
            sessions_set.insert(sid);
          }
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

int SessionManager::snapshot_count(const std::string &session_id) const {
  int n = 0;
  while (fs::exists(history_snapshot_path(session_id, n + 1)))
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
    const ::velix::llm::CompactResult cr =
      compact_history_if_needed(history);

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
        if (content.find("[CONTEXT COMPACTION]") != std::string::npos) {
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
        if (content.find("[CONTEXT COMPACTION]") != std::string::npos) {
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

SessionManager::AutoCompactGuardResult SessionManager::run_auto_compact_guard(
    const std::string &convo_id, std::size_t estimated_request_tokens,
    std::size_t max_context_tokens, double auto_compact_threshold,
    SessionIO &session_io) {
  AutoCompactGuardResult result;

  if (convo_id.empty()) {
    result.skip_reason = "missing_convo_id";
    return result;
  }
  if (max_context_tokens == 0) {
    result.skip_reason = "unknown_context_limit";
    return result;
  }
  if (auto_compact_threshold <= 0.0) {
    auto_compact_threshold = 0.70;
  }

  const double fill_ratio =
      static_cast<double>(estimated_request_tokens) /
      static_cast<double>(max_context_tokens);
  if (fill_ratio < auto_compact_threshold) {
    result.skip_reason = "below_compaction_limit";
    return result;
  }

  result.threshold_exceeded = true;
  result.compact_attempted = true;

  const Conversation convo = session_io.get_conversation(convo_id);
  json history = json::array();
  for (const auto &message : convo.messages) {
    if (message.is_object()) {
      history.push_back(message);
    }
  }

  const auto compact_result = compact(convo_id, history, true);
  result.compacted = compact_result.compacted;
  result.skip_reason = compact_result.compact_reason;
  result.tokens_before = compact_result.tokens_before;
  result.tokens_after = compact_result.tokens_after;

  if (!result.compacted && result.skip_reason.empty()) {
    result.skip_reason = "below_compaction_limit";
  }

  return result;
}

} // namespace velix::llm
