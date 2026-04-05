#include "../runtime/sdk/cpp/velix_process.hpp"
#include "../utils/string_utils.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
  std::string user_id;
  SocketWrapper socket;
  std::mutex socket_mutex;

  // Inbound request queue fed by the reader thread, drained by the worker.
  std::queue<std::string> request_queue;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;

  std::atomic<bool> active{true};

  // State persistence per user
  std::string active_convo_id;

  // Non-copyable, non-movable — always held behind a shared_ptr.
  Session() = default;
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
};

// ---------------------------------------------------------------------------
// Handler
// ---------------------------------------------------------------------------
class Handler : public VelixProcess {
public:
  Handler() : VelixProcess("terminal_handler", "handler") {}

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
  static bool is_valid_user_id(const std::string &user_id) {
    if (user_id.empty() || user_id.size() > 128) {
      return false;
    }

    const auto sep = user_id.find('_');
    if (sep == std::string::npos || sep == 0 || sep == user_id.size() - 1) {
      return false;
    }

    const std::string prefix = user_id.substr(0, sep);
    static const std::unordered_set<std::string> allowed_prefixes = {
        "telegram", "terminal", "web", "cron", "gw"};
    if (allowed_prefixes.count(prefix) == 0) {
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
    return fs::path("memory") / "conversations" / "users";
  }

  void initialize_known_users() {
    try {
      const fs::path root = known_users_root();
      fs::create_directories(root);

      std::unordered_set<std::string> discovered;
      for (const auto &entry : fs::directory_iterator(root)) {
        const fs::path p = entry.path();
        if (entry.is_regular_file() && p.extension() == ".json") {
          const std::string id = p.stem().string();
          if (!id.empty()) {
            discovered.insert(id);
          }
        } else if (entry.is_directory()) {
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
      const fs::path root = known_users_root();
      fs::create_directories(root);
      const fs::path file_path = root / (user_id + ".json");
      if (!fs::exists(file_path)) {
        std::ofstream out(file_path);
        out << "{}\n";
      }
      return true;
    } catch (const std::exception &e) {
      std::cerr << "[Handler] Failed to persist known user '" << user_id
                << "': " << e.what() << std::endl;
      return false;
    }
  }

  void handle_new_connection(SocketWrapper client) {
    std::string user_id;
    bool registered = false;
    try {
      client.set_timeout_ms(5000);
      while (!registered) {
        const std::string raw = recv_json(client);
        const json req = json::parse(raw);
        const std::string type = req.value("type", std::string(""));

        if (type == "list_users") {
          std::vector<std::string> users = snapshot_known_users();
          json j_users = json::array();
          {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            for (const auto &uid : users) {
              bool active = (sessions.count(uid) > 0);
              j_users.push_back({{"id", uid}, {"active", active}});
            }
          }
          send_json(client,
                    json{{"type", "user_list"}, {"users", j_users}}.dump());
          continue;
        }

        if (type != "register") {
          send_json(
              client,
              json{{"type", "error"},
                   {"message", "first message must be register or list_users"}}
                  .dump());
          client.close();
          return;
        }

        const std::string requested_uid = req.value("user_id", std::string(""));
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

        if (!ensure_known_user(user_id)) {
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
    session->user_id = user_id;
    session->socket = std::move(client);
    session->socket.set_timeout_ms(500);

    try {
      std::lock_guard<std::mutex> lock(session->socket_mutex);
      send_json(session->socket,
                json{{"type", "registered"}, {"user_id", user_id}}.dump());
    } catch (...) {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      sessions[user_id] = session;
    }
    std::cout << "[Handler] Session registered: " << user_id << std::endl;

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
      std::lock_guard<std::mutex> lock(session->socket_mutex);
      if (session->socket.is_open()) {
        try {
          send_json(session->socket, event.dump());
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
          // Backward compatibility for pre-standardized queued notifications.
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
            const std::string reply =
                call_llm_internal("", text, "", session->user_id,
                                  "user_conversation", true, on_token);
            flush_non_streamed_reply(reply);
          }
        }
        clear_current_user_context();
      } catch (const std::exception &e) {
        clear_current_user_context();
        send_to_client({{"type", "token"},
                        {"data", std::string("[Velix Error] ") + e.what()}});
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