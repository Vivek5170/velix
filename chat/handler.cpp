#include "../runtime/sdk/cpp/velix_process.hpp"
#include "../utils/string_utils.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <sstream>

using namespace velix::core;
using namespace velix::communication;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Session
// Represents one live gateway connection. Each session owns its socket,
// its request queue, and its worker thread.  All sessions are identified by
// a unique user_id supplied (or accepted) during the registration handshake.
// ---------------------------------------------------------------------------
struct Session {
  std::string user_id;    // the session_id (e.g. terminal_vivek_s1)
  std::string super_user; // the original gateway user (terminal_vivek)
  SocketWrapper socket;
  std::mutex socket_mutex;

  std::queue<std::string> request_queue;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;

  std::atomic<bool> active{true};

  // active_session_id == user_id for session-based users.
  // Always the convo_id passed to the scheduler/CM.
  std::string active_session_id;

  Session() = default;
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
};

// ---------------------------------------------------------------------------
// Handler
// ---------------------------------------------------------------------------
//
// Slash command table
// ───────────────────────────────────────────────────────────────────────────
// A CommandFn receives the active session and a "send to client" function.
// It is called before the message is forwarded as an LLM request.
// New commands: just add one entry to command_table_ in the constructor.
//
class Handler : public VelixProcess {
public:
  using SendFn = std::function<void(const json &)>;
  using CommandFn =
      std::function<void(std::shared_ptr<Session>, const SendFn &)>;

  Handler() : VelixProcess("terminal_handler", "handler") {
    register_commands();
  }

  // ------------------------------------------------------------------
  // on_shutdown: close the gateway listener so accept() unblocks.
  // Individual session sockets are closed by their own threads.
  // ------------------------------------------------------------------
  void on_shutdown() override {
    if (server.is_open()) {
      try {
        server.close();
      } catch (...) {
      }
    }
    // Signal all active sessions to stop.
    std::lock_guard<std::mutex> lock(sessions_mutex);
    for (auto &[uid, session] : sessions) {
      session->active.store(false);
      session->queue_cv.notify_all();
      if (session->socket.is_open()) {
        try {
          session->socket.close();
        } catch (...) {
        }
      }
    }
  }

  // ------------------------------------------------------------------
  // run
  // ------------------------------------------------------------------
  void run() override {
    initialize_known_users();

    // --- Tool lifecycle hooks (for locally executed tools) ---
    // Do not broadcast these globally; lifecycle visibility is routed via
    // user-scoped bus notifications when available.
    on_tool_start = [&](const std::string &tool, const json &args) {
      std::string uid = current_user_context();
      if (uid.empty()) {
        uid = (args.contains("user_id") && args["user_id"].is_string())
                  ? args["user_id"].get<std::string>()
                  : std::string("");
      }
      if (!uid.empty()) {
        send_to_session(
            uid, {{"type", "tool_start"}, {"tool", tool}, {"args", args}});
      }
    };

    on_tool_finish = [&](const std::string &tool, const json &result) {
      std::string uid = current_user_context();
      if (uid.empty()) {
        uid = (result.contains("user_id") && result["user_id"].is_string())
                  ? result["user_id"].get<std::string>()
                  : std::string("");
      }
      if (!uid.empty()) {
        send_to_session(
            uid, {{"type", "tool_finish"}, {"tool", tool}, {"result", result}});
      }
    };

    // --- Bus event hook ---
    // Routes NOTIFY_HANDLER and lifecycle events to sessions.
    on_bus_event = [&](const json &msg) {
      const std::string purpose = msg.value("purpose", "");
      const std::string sender_uid = msg.value("user_id", "");
      const json payload = msg.value("payload", json::object());
      const int source_pid = msg.value("source_pid", -1);

      // Approval request logic
      if (purpose == "APPROVAL_REQUEST") {
        const std::string approval_trace =
            payload.value("approval_trace", std::string(""));
        if (!approval_trace.empty() && source_pid > 0) {
          std::lock_guard<std::mutex> lock(approval_map_mutex);
          approval_trace_to_pid[approval_trace] = source_pid;
        }
      }

      // Standard Notification Processing (As per standardized requirements)
      if (purpose == "NOTIFY_HANDLER") {
        const std::string n_type = payload.value("notify_type", "SYSTEM_EVENT");

        if (n_type == "TOOL_RESULT") {
          if (sender_uid.empty()) {
            std::cerr << "[Handler] TOOL_RESULT dropped: missing user_id"
                      << std::endl;
            return;
          }

          json normalized = payload;
          if (!normalized.contains("result") ||
              !normalized["result"].is_object()) {
            json result = json::object();
            for (auto it = payload.begin(); it != payload.end(); ++it) {
              const std::string key = it.key();
              if (key == "notify_type" || key == "tool") {
                continue;
              }
              result[key] = it.value();
            }
            normalized["result"] = result;
          }

          json tool_message = json::object();
          const json result = normalized.value("result", json::object());

          if (result.is_object() &&
              result.value("role", std::string("")) == "tool" &&
              result.contains("content") && result["content"].is_string()) {
            tool_message = result;
          } else {
            tool_message["role"] = "tool";
            if (result.contains("tool_call_id") &&
                result["tool_call_id"].is_string() &&
                !result["tool_call_id"].get<std::string>().empty()) {
              tool_message["tool_call_id"] = result["tool_call_id"];
            } else if (normalized.contains("tool_call_id") &&
                       normalized["tool_call_id"].is_string() &&
                       !normalized["tool_call_id"].get<std::string>().empty()) {
              tool_message["tool_call_id"] = normalized["tool_call_id"];
            }
            tool_message["content"] = result.dump();
          }

          inject_to_session(sender_uid, {{"type", "tool_message"},
                                         {"tool", normalized.value("tool", "")},
                                         {"tool_message", tool_message}});
          return;
        } else if (n_type == "INDEPENDENT") {
          json job = {{"type", "independent_msg"}, {"payload", payload}};
          if (!sender_uid.empty()) {
            inject_to_session(sender_uid, job);
          } else {
            broadcast_injection(job);
          }
          return;
        } else if (n_type == "SYSTEM_EVENT") {
          json event = {{"type", "notify"},
                        {"notify_type", "SYSTEM_EVENT"},
                        {"payload", payload}};
          if (!sender_uid.empty())
            send_to_session(sender_uid, event);
          else
            broadcast_event(event);
          return;
        }
      }

      // Explicit mapping for remote lifecycle events
      json event;
      if (purpose == "TOOL_START") {
        if (sender_uid.empty()) {
          std::cerr << "[Handler] TOOL_START dropped: missing user_id"
                    << std::endl;
          return;
        }
        event = {{"type", "tool_start"},
                 {"tool", payload.value("tool", "")},
                 {"args", payload.value("args", json::object())}};
      } else if (purpose == "TOOL_FINISH") {
        if (sender_uid.empty()) {
          std::cerr << "[Handler] TOOL_FINISH dropped: missing user_id"
                    << std::endl;
          return;
        }
        event = {{"type", "tool_finish"},
                 {"tool", payload.value("tool", "")},
                 {"result", payload.value("result", json::object())}};
      } else {
        event = {{"type", "notify"},
                 {"purpose", purpose},
                 {"notify_type", payload.value("notify_type", "generic")},
                 {"payload", payload}};
      }

      if (!sender_uid.empty()) {
        send_to_session(sender_uid, event);
      } else {
        broadcast_event(event);
      }
    };

    // --- Start gateway listener ---
    try {
      server.create_tcp_socket();
      server.bind("0.0.0.0", 6060);
      server.listen(32);
      std::cout << "[Handler] Gateway listener on port 6060" << std::endl;

      while (is_running) {
        try {
          SocketWrapper client = server.accept();
          std::thread(&Handler::handle_new_connection, this, std::move(client))
              .detach();
        } catch (const std::exception &e) {
          if (is_running) {
            std::cerr << "[Handler] accept() error: " << e.what() << std::endl;
          }
        }
      }
    } catch (const std::exception &e) {
      std::cerr << "[Handler] Fatal server error: " << e.what() << std::endl;
    }
  }

private:
  static bool is_session_id_format(const std::string &value) {
    const auto pos = value.rfind("_s");
    if (pos == std::string::npos || pos + 2 >= value.size()) {
      return false;
    }
    const std::string suffix = value.substr(pos + 2);
    return !suffix.empty() &&
           std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
             return std::isdigit(c) != 0;
           });
  }

  static std::string extract_super_user(const std::string &value) {
    if (!is_session_id_format(value)) {
      return value;
    }
    const auto pos = value.rfind("_s");
    return value.substr(0, pos);
  }

  static bool is_valid_user_id(const std::string &user_id) {
    if (user_id.empty() || user_id.size() > 128) {
      return false;
    }

    for (const char c : user_id) {
      const bool is_alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9');
      if (!is_alnum && c != '_' && c != '-') {
        return false;
      }
    }

    return true;
  }

  static fs::path known_users_root() {
    return fs::path("memory") / "sessions" / "users";
  }

  void initialize_known_users() {
    try {
      const fs::path root = known_users_root();
      fs::create_directories(root);

      std::unordered_set<std::string> discovered;
      for (const auto &entry : fs::directory_iterator(root)) {
        const fs::path p = entry.path();
        if (entry.is_directory()) {
          const std::string id = p.filename().string();
          if (!id.empty()) {
            discovered.insert(id);
          }
        }
      }

      {
        std::lock_guard<std::mutex> lock(known_users_mutex);
        known_users = std::move(discovered);
      }
    } catch (const std::exception &e) {
      std::cerr << "[Handler] Failed to initialize known users: " << e.what()
                << std::endl;
    }
  }

  std::vector<std::string> snapshot_known_users() {
    std::vector<std::string> users;
    {
      std::lock_guard<std::mutex> lock(known_users_mutex);
      users.reserve(known_users.size());
      for (const auto &id : known_users) {
        users.push_back(id);
      }
    }
    std::sort(users.begin(), users.end());
    return users;
  }

  struct ContextUsage {
    uint64_t current_tokens{0};
    uint64_t max_tokens{0};
    bool valid{false};
  };

  ContextUsage read_context_usage(const std::string &session_id) const {
    ContextUsage out;
    if (session_id.empty()) {
      return out;
    }

    const std::string super_user = extract_super_user(session_id);
    const fs::path convo = fs::path("memory") / "sessions" / "users" / super_user /
                           session_id / (session_id + ".json");
    auto read_text = [](const fs::path &p) -> std::string {
      std::ifstream in(p);
      if (!in.is_open()) {
        return "";
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      return ss.str();
    };

    auto estimate_tokens = [](const std::string &text) -> uint64_t {
      return static_cast<uint64_t>(text.size() / 4);
    };

    uint64_t layered_tokens = 0;
    layered_tokens += estimate_tokens(
        read_text(fs::path("memory") / "general_guidelines.md"));
    layered_tokens +=
        estimate_tokens(read_text(fs::path("memory") / "agentfiles" /
                                  super_user / "soul.md"));
    layered_tokens +=
        estimate_tokens(read_text(fs::path("memory") / "agentfiles" /
                                  super_user / "user.md"));

    try {
      std::ifstream in(convo);
      if (in.is_open()) {
        json j;
        in >> j;
        const uint64_t convo_tokens =
            j.value("current_context_tokens", static_cast<uint64_t>(0));
        out.current_tokens = convo_tokens + layered_tokens;
        out.valid = true;
      }
    } catch (...) {
      return ContextUsage{};
    }

    // Mirror scheduler fallback defaults.
    out.max_tokens = 8192;
    try {
      std::ifstream cfg("config/compacter.json");
      if (!cfg.is_open()) {
        cfg.open("../config/compacter.json");
      }
      if (cfg.is_open()) {
        json c;
        cfg >> c;
        out.max_tokens = c.value("max_context_tokens", out.max_tokens);
      }
    } catch (...) {
    }

    return out;
  }

  bool ensure_known_user(const std::string &user_id) {
    bool newly_added = false;
    {
      std::lock_guard<std::mutex> lock(known_users_mutex);
      auto [_, inserted] = known_users.insert(user_id);
      newly_added = inserted;
    }

    if (!newly_added) {
      return true;
    }

    try {
      // Create agentfiles directory for the super_user to store memory profiles
      const fs::path root = fs::path("memory") / "agentfiles" / user_id;
      fs::create_directories(root);

      const fs::path fallback_soul = fs::path("memory") / "soul.md";
      const fs::path fallback_user = fs::path("memory") / "user.md";

      if (fs::exists(fallback_soul) && !fs::exists(root / "soul.md")) {
        fs::copy(fallback_soul, root / "soul.md");
      }
      if (fs::exists(fallback_user) && !fs::exists(root / "user.md")) {
        fs::copy(fallback_user, root / "user.md");
      }

      return true;
    } catch (const std::exception &e) {
      std::cerr << "[Handler] Failed to initialize agentfiles for '" << user_id
                << "': " << e.what() << std::endl;
      return false;
    }
  }

  void handle_new_connection(SocketWrapper client) {
    std::string user_id;
    std::string actual_super_user;
    bool registered = false;
    bool force_new = false;
    try {
      client.set_timeout_ms(5000);
      while (!registered) {
        const std::string raw = recv_json(client);
        const json req = json::parse(raw);
        const std::string type = req.value("type", std::string(""));

        if (type == "list_users") {
          // Ask scheduler for all super users
          const std::string raw =
              send_session_control("", "list_super_users", "");
          std::unordered_set<std::string> users_set;
          try {
            const json r = json::parse(raw);
            if (r.contains("users") && r["users"].is_array()) {
              for (const auto &u : r["users"]) {
                if (u.is_string())
                  users_set.insert(u.get<std::string>());
              }
            }
          } catch (...) {
          }

          // Fallback to filesystem-discovered users if session index is stale.
          for (const auto &uid : snapshot_known_users()) {
            users_set.insert(uid);
          }

          std::vector<std::string> users(users_set.begin(), users_set.end());
          std::sort(users.begin(), users.end());

          json j_users = json::array();
          {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            for (const auto &uid : users) {
              // Mark active if any of their sessions is actively connected
              bool active = false;
              for (const auto &[sid, sess] : sessions) {
                if (sess->super_user == uid) {
                  active = true;
                  break;
                }
              }
              j_users.push_back({{"id", uid}, {"active", active}});
            }
          }
          send_json(client,
                    json{{"type", "user_list"}, {"users", j_users}}.dump());
          continue;
        }

        if (type == "list_sessions") {
          const std::string super_user =
              req.value("super_user", std::string(""));
          if (!super_user.empty()) {
            const std::string raw =
                send_session_control(super_user, "list", "");
            json j_sessions = json::array();
            try {
              const json r = json::parse(raw);
              if (r.contains("sessions") && r["sessions"].is_array()) {
                std::lock_guard<std::mutex> lock(sessions_mutex);
                for (const auto &s : r["sessions"]) {
                  if (s.is_string()) {
                    std::string sid = s.get<std::string>();
                    bool active = (sessions.count(sid) > 0);
                    j_sessions.push_back({{"id", sid}, {"active", active}});
                  }
                }
              }
            } catch (...) {
              // Fallback parser for legacy textual listing:
              // "Sessions for <super_user>:\n  <sid>\n  <sid>\n"
              std::istringstream iss(raw);
              std::string line;
              std::lock_guard<std::mutex> lock(sessions_mutex);
              while (std::getline(iss, line)) {
                if (line.rfind("Sessions for ", 0) == 0) {
                  continue;
                }
                const auto first = line.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) {
                  continue;
                }
                const auto last = line.find_last_not_of(" \t\r\n");
                line = line.substr(first, last - first + 1);
                if (line.empty()) {
                  continue;
                }
                if (extract_super_user(line) != super_user ||
                    !is_session_id_format(line)) {
                  continue;
                }
                bool active = (sessions.count(line) > 0);
                j_sessions.push_back({{"id", line}, {"active", active}});
              }
            }
            send_json(client,
                      json{{"type", "session_list"}, {"sessions", j_sessions}}
                          .dump());
          }
          continue;
        }

        if (type != "register") {
          send_json(client, json{{"type", "error"},
                                 {"message", "first message must be register, "
                                             "list_users, or list_sessions"}}
                                .dump());
          client.close();
          return;
        }

        const std::string requested_uid = req.value("user_id", std::string(""));
        force_new = req.value("force_new", false);
        user_id =
            requested_uid.empty()
                ? ("terminal_" + velix::utils::generate_uuid().substr(0, 12))
                : requested_uid;

        if (!is_valid_user_id(user_id)) {
          send_json(client, json{{"type", "error"},
                                 {"message", "invalid user_id format"},
                                 {"user_id", user_id}}
                                .dump());
          client.close();
          return;
        }

        actual_super_user = user_id;
        actual_super_user = extract_super_user(actual_super_user);

        if (!ensure_known_user(actual_super_user)) {
          send_json(client, json{{"type", "error"},
                                 {"message", "failed to persist user identity"},
                                 {"user_id", user_id}}
                                .dump());
          client.close();
          return;
        }

        {
          std::lock_guard<std::mutex> lock(sessions_mutex);
          if (sessions.count(user_id) > 0) {
            send_json(client, json{{"type", "error"},
                                   {"message", "user_id already active"},
                                   {"user_id", user_id}}
                                  .dump());
            client.close();
            return;
          }
        }

        registered = true;
      }
    } catch (const std::exception &e) {
      std::cerr << "[Handler] Registration read failed: " << e.what()
                << std::endl;
      try {
        client.close();
      } catch (...) {
      }
      return;
    }

    auto session = std::make_shared<Session>();

    // If user_id looks like a super_user (no _s{N} suffix), resolve to an
    // actual session_id via SESSION_CONTROL/get_or_create so the session is
    // named {super_user}_s1 (or _sN for their latest session).
    std::string action = force_new ? "new" : "get_or_create";
    std::string resolved_session_id;

    if (force_new) {
      resolved_session_id = send_session_control(user_id, "new", "");
    } else if (is_session_id_format(user_id)) {
      // If user_id is already a strict _s{N} session id, use it directly.
      resolved_session_id = user_id;
    } else {
      resolved_session_id = send_session_control(user_id, "get_or_create", "");
    }

    if (resolved_session_id.empty()) {
      try {
        send_json(client, json{{"type", "error"},
                               {"message", "failed to resolve session_id from scheduler"},
                               {"user_id", user_id}}
                              .dump());
      } catch (...) {
      }
      try {
        client.close();
      } catch (...) {
      }
      return;
    }

    session->user_id = resolved_session_id;
    session->super_user = actual_super_user; // the extracted gateway identity
    session->active_session_id = resolved_session_id;
    session->socket = std::move(client);
    session->socket.set_timeout_ms(500);

    try {
      std::lock_guard<std::mutex> lock(session->socket_mutex);
      send_json(session->socket, json{{"type", "registered"},
                                      {"user_id", resolved_session_id},
                                      {"super_user", user_id}}
                                     .dump());
    } catch (...) {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      sessions[resolved_session_id] = session;
    }
    std::cout << "[Handler] Session registered: " << resolved_session_id
              << " (super_user=" << user_id << ")" << std::endl;

    std::thread worker(&Handler::session_worker, this, session);
    std::thread reader(&Handler::session_reader, this, session);
    worker.detach();
    reader.detach();
  }

  void session_reader(std::shared_ptr<Session> session) {
    while (is_running && session->active.load()) {
      try {
        const std::string raw = recv_json(session->socket);
        if (raw.empty())
          break;

        json j = json::parse(raw);
        const std::string type = j.value("type", std::string(""));

        if (type == "approval_reply") {
          handle_approval_reply(j, session);
          continue;
        }

        if (!j.contains("message"))
          continue;
        const std::string msg =
            j["message"].is_string() ? j["message"].get<std::string>() : "";
        if (msg.empty())
          continue;

        {
          std::lock_guard<std::mutex> lock(session->queue_mutex);
          session->request_queue.push(raw);
        }
        session->queue_cv.notify_one();
      } catch (const velix::communication::SocketTimeoutException &) {
        continue;
      } catch (...) {
        break;
      }
    }
    teardown_session(session);
  }

  void session_worker(std::shared_ptr<Session> session) {
    auto send_to_client = [&](const json &event) {
      // Serialize outside the lock so the hot streaming path doesn't hold
      // socket_mutex longer than necessary (bus events contend on this mutex).
      std::string frame = event.dump();
      std::lock_guard<std::mutex> lock(session->socket_mutex);
      if (session->socket.is_open()) {
        try {
          send_json(session->socket, frame);
        } catch (...) {
        }
      }
    };

    while (is_running && session->active.load()) {
      std::string request_str;
      {
        std::unique_lock<std::mutex> lock(session->queue_mutex);
        session->queue_cv.wait(lock, [&] {
          return !session->request_queue.empty() || !session->active.load() ||
                 !is_running;
        });
        if (!session->active.load() || !is_running)
          break;
        request_str = session->request_queue.front();
        session->request_queue.pop();
      }

      try {
        json j = json::parse(request_str);
        const std::string type = j.value("type", "");
        set_current_user_context(session->user_id);
        bool emitted_tokens = false;
        std::string streamed_buffer;
        auto on_token = [&](const std::string &token) {
          if (token.empty()) {
            return;
          }
          emitted_tokens = true;
          streamed_buffer += token;
          send_to_client({{"type", "token"}, {"data", token}});
        };

        auto flush_non_streamed_reply = [&](const std::string &reply) {
          if (reply.empty()) {
            return;
          }

          if (!emitted_tokens) {
            send_to_client({{"type", "token"}, {"data", reply}});
            return;
          }

          // Mixed mode: tool loops may stream an early assistant preface and
          // return a later non-streamed final answer. Reconcile and forward
          // whatever terminal text is still missing.
          if (reply == streamed_buffer) {
            return;
          }

          if (!streamed_buffer.empty() &&
              reply.rfind(streamed_buffer, 0) == 0 &&
              reply.size() > streamed_buffer.size()) {
            send_to_client({{"type", "token"},
                            {"data", reply.substr(streamed_buffer.size())}});
            return;
          }

          send_to_client({{"type", "token"}, {"data", "\n" + reply}});
        };

        if (type == "tool_message") {
          const json tool_message = j.value("tool_message", json::object());
          const std::string reply =
              call_llm_resume("", tool_message, session->user_id, on_token);
          flush_non_streamed_reply(reply);
        } else if (type == "resume_turn") {
          const json p = j.value("payload", json::object());
          const json result = p.value("result", json::object());
          json tool_message = {{"role", "tool"}};
          tool_message["content"] = result.dump();
          if (result.contains("tool_call_id") &&
              result["tool_call_id"].is_string()) {
            tool_message["tool_call_id"] = result["tool_call_id"];
          }
          const std::string reply =
              call_llm_resume("", tool_message, session->user_id, on_token);
          flush_non_streamed_reply(reply);
        } else if (type == "independent_msg") {
          const json p = j.value("payload", json::object());
          const std::string content = p.value("content", "");
          const std::string reply = call_llm_internal(
              "", content, "", session->user_id, "simple", true, on_token);
          flush_non_streamed_reply(reply);
        } else {
          const std::string text = j.value("message", "");
          if (!text.empty()) {
            // ── Slash command dispatch
            // ──────────────────────────────────────── Look up the first word
            // (e.g. "/new" or "/sessions") in the command table. Unknown slash
            // commands fall through to the LLM so the user can ask about
            // commands by name.
            const std::string cmd = text.find(' ') != std::string::npos
                                        ? text.substr(0, text.find(' '))
                                        : text;
            auto it = command_table_.find(cmd);
            if (it != command_table_.end()) {
              it->second(session, send_to_client);
            } else {
              // Normal LLM turn.
              const std::string reply =
                  call_llm_internal("", text, "", session->user_id,
                                    "user_conversation", true, on_token);
              flush_non_streamed_reply(reply);
            }
          }
        }
        clear_current_user_context();
      } catch (const std::exception &e) {
        clear_current_user_context();
        send_to_client({{"type", "token"},
                        {"data", std::string("[Velix Error] ") + e.what()}});
      }
      const ContextUsage usage = read_context_usage(session->active_session_id);
      if (usage.valid && usage.max_tokens > 0) {
        send_to_client({{"type", "context_usage"},
                        {"current_tokens", usage.current_tokens},
                        {"max_tokens", usage.max_tokens}});
      }
      send_to_client({{"type", "end"}});
    }
  }

  void handle_approval_reply(const json &j, std::shared_ptr<Session> session) {
    const std::string approval_trace = j.value("approval_trace", "");
    const std::string scope = j.value("scope", "deny");

    int target_pid = -1;
    {
      std::lock_guard<std::mutex> lock(approval_map_mutex);
      auto it = approval_trace_to_pid.find(approval_trace);
      if (it != approval_trace_to_pid.end()) {
        target_pid = it->second;
        approval_trace_to_pid.erase(it);
      }
    }

    if (target_pid > 0 && !approval_trace.empty()) {
      send_message(target_pid, "APPROVAL_REPLY",
                   {{"approval_trace", approval_trace}, {"scope", scope}});
    }

    std::lock_guard<std::mutex> lock(session->socket_mutex);
    if (session->socket.is_open()) {
      try {
        send_json(session->socket, json{{"type", "approval_ack"},
                                        {"approval_trace", approval_trace},
                                        {"target_pid", target_pid}}
                                       .dump());
      } catch (...) {
      }
    }
  }

  void teardown_session(std::shared_ptr<Session> session) {
    session->active.store(false);
    session->queue_cv.notify_all();
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      sessions.erase(session->user_id);
    }
    {
      std::lock_guard<std::mutex> lock(session->socket_mutex);
      if (session->socket.is_open()) {
        try {
          session->socket.close();
        } catch (...) {
        }
      }
    }
    std::cout << "[Handler] Session ended: " << session->user_id << std::endl;
  }

  void send_to_session(const std::string &user_id, const json &event) {
    std::shared_ptr<Session> session;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      auto it = sessions.find(user_id);
      if (it == sessions.end())
        return;
      session = it->second;
    }
    std::lock_guard<std::mutex> lock(session->socket_mutex);
    if (session->socket.is_open()) {
      try {
        send_json(session->socket, event.dump());
      } catch (...) {
      }
    }
  }

  void inject_to_session(const std::string &user_id, const json &msg) {
    std::shared_ptr<Session> session;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      auto it = sessions.find(user_id);
      if (it == sessions.end())
        return;
      session = it->second;
    }
    {
      std::lock_guard<std::mutex> lock(session->queue_mutex);
      session->request_queue.push(msg.dump());
    }
    session->queue_cv.notify_one();
  }

  void broadcast_injection(const json &msg) {
    std::vector<std::shared_ptr<Session>> snapshot;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      for (auto &[uid, s] : sessions)
        snapshot.push_back(s);
    }
    for (auto &s : snapshot) {
      std::lock_guard<std::mutex> lock(s->queue_mutex);
      s->request_queue.push(msg.dump());
      s->queue_cv.notify_one();
    }
  }

  void broadcast_event(const json &event) {
    std::vector<std::shared_ptr<Session>> snapshot;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      for (auto &[uid, s] : sessions)
        snapshot.push_back(s);
    }
    for (auto &s : snapshot) {
      std::lock_guard<std::mutex> lock(s->socket_mutex);
      if (s->socket.is_open()) {
        try {
          send_json(s->socket, event.dump());
        } catch (...) {
        }
      }
    }
  }

  SocketWrapper server;
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
  std::mutex sessions_mutex;
  std::unordered_set<std::string> known_users;
  std::mutex known_users_mutex;
  std::unordered_map<std::string, int> approval_trace_to_pid;
  std::mutex approval_map_mutex;
  std::unordered_map<std::thread::id, std::string> thread_user_context;
  std::mutex thread_user_context_mutex;

  // ── Slash command table
  // ───────────────────────────────────────────────────── Populated once in
  // register_commands(). Lookup is O(1). To add a command: one line in
  // register_commands(), nothing else changes.
  std::unordered_map<std::string, CommandFn> command_table_;

  void register_commands() {
    // ── /new ───────────────────────────────────────────────────────────────
    // Creates a brand-new session (next _sN). Re-keys the sessions map since
    // the socket now represents a new session_id.
    command_table_["/new"] = [this](auto session, const auto &send) {
      const std::string new_sid =
          send_session_control(session->super_user, "new", "");
      if (!new_sid.empty()) {
        // Re-key the sessions map: old key = current session_id, new key =
        // new_sid.
        std::lock_guard<std::mutex> lk(sessions_mutex);
        sessions.erase(session->user_id);
        session->user_id = new_sid;
        session->active_session_id = new_sid;
        sessions[new_sid] = session;
      }
      send({{"type", "token"},
            {"data", "[New session: " + session->active_session_id + "]\n"}});
    };

    // ── /compact ───────────────────────────────────────────────────────────
    // Compact = save snapshot + reset the SAME session with pre-seeded history.
    // session_id does NOT change. No re-keying needed.
    command_table_["/compact"] = [this](auto session, const auto &send) {
      send({{"type", "token"}, {"data", "[Compacting session...]\n"}});
      // Scheduler handles: snapshot → reset live convo with pre-seeded history.
      // Session reply contains tokens_before / tokens_after for display.
      const std::string raw =
          send_session_control(session->active_session_id, "compact", "");
      // raw may contain a JSON SESSION_RESPONSE — parse and show stats.
      std::string status_msg =
          "[Compacted: " + session->active_session_id + "]";
      try {
        const json r = json::parse(raw);
        if (r.value("session_compacted", false) &&
            r.contains("tokens_before") && r.contains("tokens_after")) {
          status_msg =
              "[Compacted: " + std::to_string(r.value("tokens_before", 0)) +
              " → " + std::to_string(r.value("tokens_after", 0)) + " tokens]";
        } else if (!r.value("session_compacted", true)) {
          const std::string reason = r.value("compact_reason", "below_compaction_limit");
          status_msg = "[Compaction skipped: " + reason + "]";
        }
      } catch (...) {
      }
      send({{"type", "token"}, {"data", status_msg + "\n"}});
    };

    // ── /sessions ──────────────────────────────────────────────────────────
    command_table_["/sessions"] = [this](auto session, const auto &send) {
      std::string listing = "(no sessions)";
      const std::string raw =
          send_session_control(session->super_user, "list", "");
      try {
        const json r = json::parse(raw);
        listing = r.value("listing", listing);
      } catch (...) {
        if (!raw.empty()) {
          listing = raw;
        }
      }
      send({{"type", "token"}, {"data", listing}});
    };

    // ── /help
    // ─────────────────────────────────────────────────────────────────
    command_table_["/help"] = [this](auto /*session*/, const auto &send) {
      std::string msg = "Available commands:\n";
      for (const auto &[name, _] : command_table_) {
        msg += "  " + name + "\n";
      }
      send({{"type", "token"}, {"data", msg}});
    };
    // ───────────────────────────────────────────────────────────────────────────
    // To add a new command: command_table_["/mycommand"] = [this](...) {...};
  }

  // ── Session control ────────────────────────────────────────────────────
  //
  // Send a SESSION_CONTROL frame to the scheduler and parse the response.
  // action: "get_or_create" | "new" | "compact" | "list"
  //
  // The handler is a stateless TCP gateway. All session/conversation lifecycle
  // is owned by the scheduler's SessionManager. The handler never imports or
  // links against any llm/ headers.
  //
  std::string send_session_control(const std::string &user_id,
                                   const std::string &action,
                                   const std::string & /*extra*/) {
    constexpr int kSchedulerPort = 5171;
    constexpr int kRetries = 3;
    constexpr int kRetryDelayMs = 300;
    constexpr int kTimeoutMs = 8000;

    try {
      SocketWrapper sock;
      sock.create_tcp_socket();
      sock.set_timeout_ms(kTimeoutMs);

      // Retry loop mirrors connect_with_retries in the SDK.
      bool connected = false;
      for (int attempt = 0; attempt < kRetries && !connected; ++attempt) {
        try {
          sock.connect("127.0.0.1", kSchedulerPort);
          connected = true;
        } catch (...) {
          if (attempt + 1 < kRetries) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kRetryDelayMs));
          }
        }
      }
      if (!connected) {
        std::cerr << "[Handler] Could not connect to scheduler for "
                  << "SESSION_CONTROL/" << action << std::endl;
        return "";
      }

      const json frame = {{"message_type", "SESSION_CONTROL"},
                          {"action", action},
                          {"user_id", user_id}};
      send_json(sock, frame.dump());

      const std::string raw = recv_json(sock);
      const json response = json::parse(raw);

      if (response.value("message_type", "") != "SESSION_RESPONSE") {
        std::cerr << "[Handler] Unexpected SESSION_CONTROL response: "
                  << response.value("message_type", "(none)") << std::endl;
        return "";
      }
      if (action == "list" || action == "compact" || action == "list_super_users") {
        // Return the full JSON so caller can read tokens_before/tokens_after or
        // users/sessions arrays.
        return raw;
      }
      return response.value("session_id", "");

    } catch (const std::exception &e) {
      std::cerr << "[Handler] SESSION_CONTROL failed (" << action
                << "): " << e.what() << std::endl;
      return "";
    }
  }

  void set_current_user_context(const std::string &user_id) {
    std::lock_guard<std::mutex> lock(thread_user_context_mutex);
    thread_user_context[std::this_thread::get_id()] = user_id;
  }

  void clear_current_user_context() {
    std::lock_guard<std::mutex> lock(thread_user_context_mutex);
    thread_user_context.erase(std::this_thread::get_id());
  }

  std::string current_user_context() {
    std::lock_guard<std::mutex> lock(thread_user_context_mutex);
    auto it = thread_user_context.find(std::this_thread::get_id());
    if (it == thread_user_context.end()) {
      return "";
    }
    return it->second;
  }
};

int main() {
  Handler handler;
  handler.start();
  return 0;
}