// =============================================================================
// handler.cpp — Velix Terminal Handler
//
// Responsibilities:
//   - Gateway listener on port 6060 (configurable via config/handler.json)
//   - Multi-session management: one active socket per (super_user, session_id)
//   - Per-session reader + worker threads with clean lifecycle
//   - Full slash-command dispatch with extensible command table
//   - Streaming-first LLM loop with mixed-mode reconciliation
//   - Tool lifecycle events forwarded to the correct session
//   - Approval request / reply routing
//   - Session control delegated entirely to the Scheduler's SessionManager
//   - All filesystem reads go directly to disk (no scheduler queries for info)
//
// Config file:  config/handler.json  (JSON/JSONC, loaded once at startup)
//   {
//     "port":               6060,
//     "tool_output_mode":   "full" | "summary" | "silent",
//     "stream_enabled":     true,
//     "max_tool_output_len": 512
//   }
// =============================================================================

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
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace velix::core;
using namespace velix::communication;
namespace fs = std::filesystem;

// =============================================================================
// HandlerConfig — loaded once at startup from config/handler.json
// =============================================================================

enum class ToolOutputMode { FULL, SUMMARY, SILENT };

struct HandlerConfig {
    int         port              = 6060;
    ToolOutputMode tool_output_mode = ToolOutputMode::FULL;
    bool        stream_enabled    = true;
    int         max_tool_output_len = 512; // chars; 0 = unlimited

    static HandlerConfig load() {
        HandlerConfig cfg;
        for (const char* p : {"config/handler.json", "../config/handler.json",
                               "build/config/handler.json"}) {
            std::ifstream f(p);
            if (!f.is_open()) continue;
            try {
                const std::string content((std::istreambuf_iterator<char>(f)),
                                          std::istreambuf_iterator<char>());
                // Allow // comments in handler config files.
                json j = json::parse(content, nullptr, true, true);
                cfg.port              = j.value("port", cfg.port);
                cfg.stream_enabled    = j.value("stream_enabled", cfg.stream_enabled);
                cfg.max_tool_output_len = j.value("max_tool_output_len",
                                                   cfg.max_tool_output_len);
                const std::string mode = j.value("tool_output_mode",
                                                  std::string("full"));
                if      (mode == "summary") cfg.tool_output_mode = ToolOutputMode::SUMMARY;
                else if (mode == "silent")  cfg.tool_output_mode = ToolOutputMode::SILENT;
                else                        cfg.tool_output_mode = ToolOutputMode::FULL;
            } catch (...) {}
            break;
        }
        return cfg;
    }
};

// =============================================================================
// Session — one live gateway connection
// =============================================================================

struct Session {
    // Identity
    std::string user_id;       // resolved session_id  e.g. vivek_s2
    std::string super_user;    // gateway identity       e.g. vivek

    // Socket
    SocketWrapper socket;
    std::mutex    socket_mutex;

    // Work queue (reader → worker)
    std::queue<std::string> request_queue;
    std::mutex              queue_mutex;
    std::condition_variable queue_cv;

    std::atomic<bool> active{true};

    // Convenience: always == user_id for resolved sessions
    std::string active_session_id;

    Session() = default;
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;
};

// =============================================================================
// Helpers: filesystem reads
// =============================================================================

namespace detail {

// Truncate tool output string to max_len if > 0.
static std::string truncate_output(const std::string& s, int max_len) {
    if (max_len <= 0 || static_cast<int>(s.size()) <= max_len) return s;
    return s.substr(0, static_cast<size_t>(max_len)) + "… [truncated]";
}

} // namespace detail

// =============================================================================
// Handler
// =============================================================================

class Handler : public VelixProcess {
public:
    using SendFn    = std::function<void(const json&)>;
    using CommandFn = std::function<bool(std::shared_ptr<Session>,
                                         const SendFn&,
                                         const std::string& /*args*/)>;
    // Returns true  → command handled, do NOT forward to LLM.
    // Returns false → pass through to LLM (unknown slash command).

    Handler() : VelixProcess("terminal_handler", "handler") {
        config_ = HandlerConfig::load();
        register_commands();
    }

    // ------------------------------------------------------------------
    // on_shutdown
    // ------------------------------------------------------------------
    void on_shutdown() override {
        if (server_.is_open()) {
            try { server_.close(); } catch (...) {}
        }
        std::lock_guard<std::mutex> lk(sessions_mutex_);
        for (auto& [uid, s] : sessions_) {
            s->active.store(false);
            s->queue_cv.notify_all();
            if (s->socket.is_open()) {
                try { s->socket.close(); } catch (...) {}
            }
        }
    }

    // ------------------------------------------------------------------
    // run
    // ------------------------------------------------------------------
    void run() override {
        initialize_known_users();
        install_tool_hooks();
        install_bus_hook();
        start_gateway_listener();
    }

private:
    // ------------------------------------------------------------------
    // Hooks
    // ------------------------------------------------------------------

    void install_tool_hooks() {
        on_tool_start = [this](const std::string& tool, const json& args) {
            if (config_.tool_output_mode == ToolOutputMode::SILENT) return;
            const std::string uid = resolve_uid_from_context_or_args(args);
            if (uid.empty()) return;
            if (config_.tool_output_mode == ToolOutputMode::SUMMARY) {
                send_to_session(uid, {{"type","tool_start"},
                                      {"tool", tool},
                                      {"summary", "⚙ " + tool + " started"}});
            } else {
                send_to_session(uid, {{"type","tool_start"},
                                      {"tool", tool},
                                      {"args", args}});
            }
        };

        on_tool_finish = [this](const std::string& tool, const json& result) {
            if (config_.tool_output_mode == ToolOutputMode::SILENT) return;
            const std::string uid = resolve_uid_from_context_or_result(result);
            if (uid.empty()) return;
            if (config_.tool_output_mode == ToolOutputMode::SUMMARY) {
                send_to_session(uid, {{"type","tool_finish"},
                                      {"tool", tool},
                                      {"summary", "✓ " + tool + " finished"}});
            } else {
                json display = result;
                // Truncate heavy text fields if configured.
                if (config_.max_tool_output_len > 0 &&
                    display.is_object() && display.contains("output") &&
                    display["output"].is_string()) {
                    display["output"] = detail::truncate_output(
                        display["output"].get<std::string>(),
                        config_.max_tool_output_len);
                }
                send_to_session(uid, {{"type","tool_finish"},
                                      {"tool", tool},
                                      {"result", display}});
            }
        };
    }

    void install_bus_hook() {
        on_bus_event = [this](const json& msg) {
            const std::string purpose    = msg.value("purpose", "");
            const std::string sender_uid = msg.value("user_id", "");
            const json        payload    = msg.value("payload", json::object());
            const int         source_pid = msg.value("source_pid", -1);

            // ── Approval request ──────────────────────────────────────────
            if (purpose == "APPROVAL_REQUEST") {
                const std::string atrace =
                    payload.value("approval_trace", std::string(""));
                if (!atrace.empty() && source_pid > 0) {
                    std::lock_guard<std::mutex> lk(approval_map_mutex_);
                    approval_trace_to_pid_[atrace] = source_pid;
                }
                if (!sender_uid.empty()) {
                    send_to_session(sender_uid,
                                    {{"type","approval_request"},
                                     {"approval_trace", atrace},
                                     {"payload", payload}});
                }
                return;
            }

            // ── NOTIFY_HANDLER ─────────────────────────────────────────────
            if (purpose == "NOTIFY_HANDLER") {
                dispatch_notify_handler(sender_uid, payload, source_pid);
                return;
            }

            // ── Tool lifecycle forwarded over bus ─────────────────────────
            if (purpose == "TOOL_START" || purpose == "TOOL_FINISH") {
                if (sender_uid.empty()) return;
                const std::string etype =
                    (purpose == "TOOL_START") ? "tool_start" : "tool_finish";
                if (config_.tool_output_mode == ToolOutputMode::SILENT) return;
                json event = {{"type", etype},
                              {"tool", payload.value("tool", "")}};
                if (config_.tool_output_mode == ToolOutputMode::FULL) {
                    event[purpose == "TOOL_START" ? "args" : "result"] =
                        payload.value(purpose == "TOOL_START" ? "args" : "result",
                                      json::object());
                } else {
                    event["summary"] = (purpose == "TOOL_START" ? "⚙ " : "✓ ") +
                                       payload.value("tool", std::string(""));
                }
                send_to_session(sender_uid, event);
                return;
            }

            // ── Generic bus event ──────────────────────────────────────────
            json event = {{"type","notify"},{"purpose",purpose},{"payload",payload}};
            if (!sender_uid.empty()) send_to_session(sender_uid, event);
            else                     broadcast_event(event);
        };
    }

    void dispatch_notify_handler(const std::string& sender_uid,
                                  const json& payload,
                                  int /*source_pid*/) {
        const std::string n_type = payload.value("notify_type", "SYSTEM_EVENT");

        if (n_type == "TOOL_RESULT") {
            if (sender_uid.empty()) return;
            // Build a normalised tool_message and inject it into the work queue
            // so the session_worker can resume the LLM turn.
            json result = payload.value("result", json::object());
            json tool_message;
            if (result.is_object() &&
                result.value("role", std::string("")) == "tool" &&
                result.contains("content") && result["content"].is_string()) {
                tool_message = result;
            } else {
                tool_message = {{"role","tool"}};
                if (result.contains("tool_call_id") &&
                    result["tool_call_id"].is_string()) {
                    tool_message["tool_call_id"] = result["tool_call_id"];
                }
                tool_message["content"] = result.dump();
            }
            inject_to_session(sender_uid,
                              {{"type","tool_message"},
                               {"tool", payload.value("tool", "")},
                               {"tool_message", tool_message}});
            return;
        }

        if (n_type == "INDEPENDENT") {
            json job = {{"type","independent_msg"}, {"payload", payload}};
            if (!sender_uid.empty()) inject_to_session(sender_uid, job);
            else                     broadcast_injection(job);
            return;
        }

        // SYSTEM_EVENT and everything else → direct send (no queue).
        json event = {{"type","notify"},
                      {"notify_type", n_type},
                      {"payload", payload}};
        if (!sender_uid.empty()) send_to_session(sender_uid, event);
        else                     broadcast_event(event);
    }

    // ------------------------------------------------------------------
    // Gateway listener
    // ------------------------------------------------------------------

    void start_gateway_listener() {
        try {
            server_.create_tcp_socket();
            server_.bind("0.0.0.0", static_cast<uint16_t>(config_.port));
            server_.listen(32);
            std::cout << "[Handler] Gateway listening on port "
                      << config_.port << std::endl;

            while (is_running) {
                try {
                    SocketWrapper client = server_.accept();
                    std::thread(&Handler::handle_new_connection,
                                this, std::move(client)).detach();
                } catch (const std::exception& e) {
                    if (is_running)
                        std::cerr << "[Handler] accept() error: "
                                  << e.what() << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Handler] Fatal: " << e.what() << std::endl;
        }
    }

    // ------------------------------------------------------------------
    // Connection registration
    // ------------------------------------------------------------------

    void handle_new_connection(SocketWrapper client) {
        std::string user_id;
        std::string actual_super_user;
        bool        force_new   = false;
        bool        registered  = false;

        try {
            client.set_timeout_ms(8000);

            while (!registered) {
                const std::string raw = recv_json(client);
                const json req = json::parse(raw);
                const std::string type = req.value("type", std::string(""));

                // ── list_users ───────────────────────────────────────────
                if (type == "list_users") {
                    const std::string raw = send_session_query("all_sessions", "", "");
                    json j_users = json::array();
                    try {
                        const json resp = json::parse(raw);
                        const json users = resp.value("super_users", json::array());
                        std::lock_guard<std::mutex> lk(sessions_mutex_);
                        for (const auto& u : users) {
                            if (!u.is_object()) continue;
                            const std::string su = u.value("super_user", std::string(""));
                            if (su.empty()) continue;
                            bool active = false;
                            for (const auto& [sid, s] : sessions_) {
                                if (s->super_user == su) { active = true; break; }
                            }
                            j_users.push_back({{"id", su}, {"active", active}});
                        }
                    } catch (...) {}
                    send_json(client,
                              json{{"type","user_list"},{"users",j_users}}.dump());
                    continue;
                }

                // ── list_sessions ─────────────────────────────────────────
                if (type == "list_sessions") {
                    const std::string su = req.value("super_user", std::string(""));
                    json j_sessions = json::array();
                    if (!su.empty()) {
                        const std::string raw = send_session_query("all_sessions", "", "");
                        try {
                            const json resp = json::parse(raw);
                            const json users = resp.value("super_users", json::array());
                            std::lock_guard<std::mutex> lk(sessions_mutex_);
                            for (const auto& u : users) {
                                if (!u.is_object() || u.value("super_user", std::string("")) != su) {
                                    continue;
                                }
                                const json sessions = u.value("sessions", json::array());
                                for (const auto& s : sessions) {
                                    if (!s.is_object()) continue;
                                    const std::string sid = s.value("session_id", std::string(""));
                                    if (sid.empty()) continue;
                                    const bool active = sessions_.count(sid) > 0;
                                    j_sessions.push_back({
                                        {"id", sid},
                                        {"title", s.value("title", std::string(""))},
                                        {"turns", s.value("turn_count", 0)},
                                        {"active", active}
                                    });
                                }
                                break;
                            }
                        } catch (...) {}
                    }
                    send_json(client,
                              json{{"type","session_list"},
                                   {"sessions",j_sessions}}.dump());
                    continue;
                }

                // ── register ──────────────────────────────────────────────
                if (type != "register") {
                    send_json(client,
                              json{{"type","error"},
                                   {"message","first message must be register, "
                                    "list_users, or list_sessions"}}.dump());
                    client.close();
                    return;
                }

                const std::string requested_uid =
                    req.value("user_id", std::string(""));
                force_new = req.value("force_new", false);
                user_id   = requested_uid.empty()
                    ? ("terminal_" + velix::utils::generate_uuid().substr(0, 12))
                    : requested_uid;

                if (!is_valid_user_id(user_id)) {
                    send_json(client,
                              json{{"type","error"},
                                   {"message","invalid user_id format"},
                                   {"user_id", user_id}}.dump());
                    client.close();
                    return;
                }

                actual_super_user = extract_super_user(user_id);
                ensure_known_user(actual_super_user);

                {
                    std::lock_guard<std::mutex> lk(sessions_mutex_);
                    if (sessions_.count(user_id) > 0) {
                        send_json(client,
                                  json{{"type","error"},
                                       {"message","user_id already active"},
                                       {"user_id", user_id}}.dump());
                        client.close();
                        return;
                    }
                }
                registered = true;
            }
        } catch (const std::exception& e) {
            std::cerr << "[Handler] Registration failed: " << e.what() << std::endl;
            try { client.close(); } catch (...) {}
            return;
        }

        // ── Resolve session_id via Scheduler ──────────────────────────────
        std::string resolved_sid;
        if (force_new) {
            resolved_sid = send_session_control(user_id, "new", "");
        } else if (is_session_id_format(user_id)) {
            resolved_sid = user_id;
        } else {
            resolved_sid = send_session_control(user_id, "get_or_create", "");
        }

        if (resolved_sid.empty()) {
            try {
                send_json(client,
                          json{{"type","error"},
                               {"message","failed to resolve session_id"},
                               {"user_id", user_id}}.dump());
            } catch (...) {}
            try { client.close(); } catch (...) {}
            return;
        }

        // ── Build session ─────────────────────────────────────────────────
        auto session = std::make_shared<Session>();
        session->user_id           = resolved_sid;
        session->super_user        = actual_super_user;
        session->active_session_id = resolved_sid;
        session->socket            = std::move(client);
        session->socket.set_timeout_ms(500);

        try {
            std::lock_guard<std::mutex> lk(session->socket_mutex);
            send_json(session->socket,
                      json{{"type","registered"},
                           {"user_id",    resolved_sid},
                           {"super_user", user_id}}.dump());
        } catch (...) { return; }

        {
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            sessions_[resolved_sid] = session;
        }
        std::cout << "[Handler] Session registered: " << resolved_sid
                  << " (super_user=" << user_id << ")" << std::endl;

        std::thread(&Handler::session_reader, this, session).detach();
        std::thread(&Handler::session_worker, this, session).detach();
    }

    // ------------------------------------------------------------------
    // Session reader — pumps raw frames from the socket into the queue
    // ------------------------------------------------------------------

    void session_reader(std::shared_ptr<Session> session) {
        while (is_running && session->active.load()) {
            try {
                const std::string raw = recv_json(session->socket);
                if (raw.empty()) break;

                json j = json::parse(raw);
                const std::string type = j.value("type", std::string(""));

                // Approval replies are routed directly — don't queue.
                if (type == "approval_reply") {
                    handle_approval_reply(j, session);
                    continue;
                }

                // Session switch: client wants to swap to a different session_id.
                if (type == "switch_session") {
                    handle_switch_session(j, session);
                    continue;
                }

                // Must have a "message" field to be LLM-workable.
                if (!j.contains("message") && type != "tool_message" &&
                    type != "resume_turn" && type != "independent_msg") {
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lk(session->queue_mutex);
                    session->request_queue.push(raw);
                }
                session->queue_cv.notify_one();

            } catch (const SocketTimeoutException&) {
                continue;
            } catch (...) {
                break;
            }
        }
        teardown_session(session);
    }

    // ------------------------------------------------------------------
    // Session worker — processes queued requests one at a time
    // ------------------------------------------------------------------

    void session_worker(std::shared_ptr<Session> session) {
        auto send_to_client = [&](const json& event) {
            std::string frame = event.dump();
            std::lock_guard<std::mutex> lk(session->socket_mutex);
            if (session->socket.is_open()) {
                try { send_json(session->socket, frame); } catch (...) {}
            }
        };

        while (is_running && session->active.load()) {
            std::string request_str;
            {
                std::unique_lock<std::mutex> lk(session->queue_mutex);
                session->queue_cv.wait(lk, [&]{
                    return !session->request_queue.empty() ||
                           !session->active.load() || !is_running;
                });
                if (!session->active.load() || !is_running) break;
                request_str = session->request_queue.front();
                session->request_queue.pop();
            }

            process_request(session, request_str, send_to_client);

            // Always send context_usage + end after each turn.
            emit_context_usage(session, send_to_client);
            send_to_client({{"type","end"}});
        }
    }

    // ------------------------------------------------------------------
    // Request processor
    // ------------------------------------------------------------------

    void process_request(std::shared_ptr<Session>   session,
                         const std::string&          request_str,
                         const SendFn&               send_to_client) {
        try {
            json j = json::parse(request_str);
            const std::string type = j.value("type", std::string(""));
            set_current_user_context(session->user_id);

            // ── Streaming state ───────────────────────────────────────────
            bool        emitted_tokens  = false;
            std::string streamed_buffer;

            auto on_token = [&](const std::string& token) {
                if (token.empty()) return;
                emitted_tokens  = true;
                streamed_buffer += token;
                send_to_client({{"type","token"},{"data", token}});
            };

            // Reconcile streaming vs non-streaming final reply.
            auto flush_reply = [&](const std::string& reply) {
                if (reply.empty()) return;
                if (!emitted_tokens) {
                    send_to_client({{"type","token"},{"data", reply}});
                    return;
                }
                if (reply == streamed_buffer) return;
                if (!streamed_buffer.empty() &&
                    reply.rfind(streamed_buffer, 0) == 0 &&
                    reply.size() > streamed_buffer.size()) {
                    send_to_client({{"type","token"},
                                    {"data", reply.substr(streamed_buffer.size())}});
                    return;
                }
                send_to_client({{"type","token"},{"data", "\n" + reply}});
            };

            // ── Dispatch by message type ──────────────────────────────────

            if (type == "tool_message") {
                const json tm = j.value("tool_message", json::object());
                const std::string reply =
                    call_llm_resume("", tm, session->user_id, on_token);
                flush_reply(reply);

            } else if (type == "resume_turn") {
                const json p  = j.value("payload", json::object());
                const json r  = p.value("result", json::object());
                json tm = {{"role","tool"}};
                tm["content"] = r.dump();
                if (r.contains("tool_call_id") && r["tool_call_id"].is_string())
                    tm["tool_call_id"] = r["tool_call_id"];
                const std::string reply =
                    call_llm_resume("", tm, session->user_id, on_token);
                flush_reply(reply);

            } else if (type == "independent_msg") {
                const json p = j.value("payload", json::object());
                const std::string content = p.value("content", std::string(""));
                const std::string reply =
                    call_llm_internal("", content, "", session->user_id,
                                      "simple", config_.stream_enabled, on_token);
                flush_reply(reply);

            } else {
                // Normal text message — check slash-command table first.
                const std::string text = j.value("message", std::string(""));
                if (text.empty()) {
                    clear_current_user_context();
                    return;
                }

                const std::string cmd = text.find(' ') != std::string::npos
                    ? text.substr(0, text.find(' '))
                    : text;
                const std::string args = text.find(' ') != std::string::npos
                    ? text.substr(text.find(' ') + 1)
                    : std::string("");

                auto it = command_table_.find(cmd);
                if (it != command_table_.end()) {
                    // Returns true → handled. Returns false → fall through to LLM.
                    if (it->second(session, send_to_client, args)) {
                        clear_current_user_context();
                        return;
                    }
                }

                // LLM turn.
                const std::string reply =
                    call_llm_internal("", text, "", session->user_id,
                                      "user_conversation",
                                      config_.stream_enabled, on_token);
                flush_reply(reply);
            }

            clear_current_user_context();

        } catch (const std::exception& e) {
            clear_current_user_context();
            send_to_client({{"type","error"},
                            {"message", std::string(e.what())}});
        }
    }

    // ------------------------------------------------------------------
    // Context usage helper
    // ------------------------------------------------------------------

    void emit_context_usage(std::shared_ptr<Session> session,
                             const SendFn& send_to_client) {
        const std::string raw =
            send_session_query("info", "", session->active_session_id);
        uint64_t current_tokens = 0;
        uint64_t max_tokens = 0;
        uint64_t session_tokens = 0;
        uint64_t system_prompt_tokens = 0;
        uint64_t tool_schema_tokens = 0;
        uint64_t request_tokens = 0;
        try {
            const json r = json::parse(raw);
            current_tokens = r.value("total_context_tokens", static_cast<uint64_t>(0));
            max_tokens = r.value("max_context_tokens", static_cast<uint64_t>(0));
            session_tokens = r.value("session_tokens", static_cast<uint64_t>(0));
            system_prompt_tokens =
                r.value("system_prompt_tokens", static_cast<uint64_t>(0));
            tool_schema_tokens =
                r.value("tool_schema_tokens", static_cast<uint64_t>(0));
            request_tokens = r.value("request_tokens", static_cast<uint64_t>(0));
            if (current_tokens == 0 || max_tokens == 0) {
                const json s = r.value("session", json::object());
                current_tokens = s.value("current_context_tokens", static_cast<uint64_t>(0));
                max_tokens = s.value("max_context_tokens", static_cast<uint64_t>(0));
            }
        } catch (...) {
            return;
        }
        if (max_tokens == 0) return;
        send_to_client({{"type",           "context_usage"},
                        {"current_tokens", current_tokens},
                        {"max_tokens",     max_tokens},
                        {"session_tokens", session_tokens},
                        {"system_prompt_tokens", system_prompt_tokens},
                        {"tool_schema_tokens", tool_schema_tokens},
                        {"request_tokens", request_tokens},
                        {"total_context_tokens", current_tokens},
                        {"pct",  static_cast<double>(current_tokens) /
                                 static_cast<double>(max_tokens) * 100.0}});
    }

    // ------------------------------------------------------------------
    // Slash command table
    // ------------------------------------------------------------------
    //
    // Each CommandFn(session, send, args) returns:
    //   true  → handled, do not forward to LLM
    //   false → not handled, forward original message to LLM
    //
    // To add a command: one entry in register_commands(), nothing else.

    void register_commands() {

        // ── /help ──────────────────────────────────────────────────────
        command_table_["/help"] = [this](auto /*s*/, const auto& send,
                                          const std::string& /*args*/) {
            std::ostringstream ss;
            ss << "Available commands:\n\n";
            for (const auto& [name, _] : command_table_)
                ss << "  " << name << "\n";
            ss << "\n";
            ss << "  /new              — start a new session\n";
            ss << "  /compact          — compact current session\n";
            ss << "  /undo             — undo last turn\n";
            ss << "  /sessions         — list sessions for your user\n";
            ss << "  /title <text>     — set title for current session\n";
            ss << "  /session_info     — current session stats from disk\n";
            ss << "  /model_info       — model & adapter configuration\n";
            ss << "  /scheduler_info   — scheduler queue depth\n";
            ss << "  /context          — context window usage\n";
            ss << "  /help             — this message\n";
            send({{"type","token"},{"data", ss.str()}});
            return true;
        };

        // ── /new ───────────────────────────────────────────────────────
        command_table_["/new"] = [this](auto session, const auto& send,
                                         const std::string& /*args*/) {
            const std::string new_sid =
                send_session_control(session->super_user, "new", "");
            if (!new_sid.empty()) {
                std::lock_guard<std::mutex> lk(sessions_mutex_);
                sessions_.erase(session->user_id);
                session->user_id           = new_sid;
                session->active_session_id = new_sid;
                sessions_[new_sid]         = session;
            }
            const std::string label = new_sid.empty()
                ? "[Error: could not create new session]"
                : "[New session: " + session->active_session_id + "]";
            send({{"type","token"},{"data", label + "\n"}});
            return true;
        };

        // ── /compact ──────────────────────────────────────────────────
        command_table_["/compact"] = [this](auto session, const auto& send,
                                             const std::string& /*args*/) {
            send({{"type","token"},{"data","[Compacting…]\n"}});
            const std::string raw =
                send_session_control(session->active_session_id, "compact", "");

            if (raw.empty()) {
                send({{"type","token"},
                      {"data","[Compaction status unavailable: request timed out]\n"}});
                return true;
            }

            std::string msg = "[Compacted: " + session->active_session_id + "]";
            try {
                const json r = json::parse(raw);
                if (r.value("session_compacted", false) &&
                    r.contains("tokens_before") && r.contains("tokens_after")) {
                    msg = "[Compacted: " +
                          std::to_string(r.value("tokens_before", 0)) +
                          " → " +
                          std::to_string(r.value("tokens_after", 0)) +
                          " tokens]";
                } else if (!r.value("session_compacted", true)) {
                    msg = "[Compaction skipped: " +
                          r.value("compact_reason",
                                  std::string("below threshold")) + "]";
                }
            } catch (...) {}
            send({{"type","token"},{"data", msg + "\n"}});
            return true;
        };

        // ── /undo ─────────────────────────────────────────────────────
        command_table_["/undo"] = [this](auto session, const auto& send,
                                          const std::string& /*args*/) {
            const std::string raw =
                send_session_control(session->active_session_id, "undo", "");
            std::string msg = "[Undo attempted]";
            try {
                const json r = json::parse(raw);
                const int removed   = r.value("turns_removed", 0);
                const int remaining = r.value("turns_remaining", 0);
                msg = "[Undo: removed " + std::to_string(removed) +
                      " turn(s), " + std::to_string(remaining) + " remaining]";
            } catch (...) {}
            send({{"type","token"},{"data", msg + "\n"}});
            return true;
        };

        // ── /sessions ─────────────────────────────────────────────────
        command_table_["/sessions"] = [this](auto session, const auto& send,
                                              const std::string& /*args*/) {
            const std::string raw = send_session_query("all_sessions", "", "");
            json target_sessions = json::array();
            try {
                const json resp = json::parse(raw);
                const json users = resp.value("super_users", json::array());
                for (const auto& u : users) {
                    if (!u.is_object()) continue;
                    if (u.value("super_user", std::string("")) == session->super_user) {
                        target_sessions = u.value("sessions", json::array());
                        break;
                    }
                }
            } catch (...) {}

            if (!target_sessions.is_array() || target_sessions.empty()) {
                send({{"type","token"},{"data","(no sessions)\n"}});
                return true;
            }
            std::ostringstream ss;
            ss << "Sessions for " << session->super_user << ":\n";
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            for (const auto& s : target_sessions) {
                if (!s.is_object()) continue;
                const std::string sid = s.value("session_id", std::string(""));
                if (sid.empty()) continue;
                const bool active  = sessions_.count(sid) > 0;
                const bool current = (sid == session->active_session_id);
                ss << "  " << sid;
                const std::string title = s.value("title", std::string(""));
                if (!title.empty()) ss << "  \"" << title << "\"";
                ss << "  [turns=" << s.value("turn_count", 0) << "]";
                ss << "  [tokens=" << s.value("current_context_tokens",
                                                static_cast<uint64_t>(0)) << "]";
                if (active)  ss << "  [active]";
                if (current) ss << "  ← current";
                ss << "\n";
            }
            send({{"type","token"},{"data", ss.str()}});
            return true;
        };

        // ── /title ────────────────────────────────────────────────────
        command_table_["/title"] = [this](auto session, const auto& send,
                                           const std::string& args) {
            if (args.empty()) {
                send({{"type","token"},{"data","Usage: /title <new title>\n"}});
                return true;
            }
            const std::string raw =
                send_session_control(session->active_session_id, "set_title", args);
            std::string msg = "[Title set to: " + args + "]";
            try {
                const json r = json::parse(raw);
                if (r.contains("error"))
                    msg = "[Error setting title: " + r.value("error","") + "]";
            } catch (...) {}
            send({{"type","token"},{"data", msg + "\n"}});
            return true;
        };

        // ── /session_info ─────────────────────────────────────────────
        command_table_["/session_info"] = [this](auto session, const auto& send,
                                                   const std::string& /*args*/) {
            const std::string raw =
                send_session_query("info", "", session->active_session_id);
            json info = json::object();
            uint64_t session_tokens = 0;
            uint64_t system_prompt_tokens = 0;
            uint64_t tool_schema_tokens = 0;
            uint64_t request_tokens = 0;
            uint64_t total_context_tokens = 0;
            uint64_t max_context_tokens = 0;
            double context_fill_pct = 0.0;
            try {
                const json r = json::parse(raw);
                info = r.value("session", json::object());
                session_tokens = r.value("session_tokens", static_cast<uint64_t>(0));
                system_prompt_tokens = r.value("system_prompt_tokens", static_cast<uint64_t>(0));
                tool_schema_tokens = r.value("tool_schema_tokens", static_cast<uint64_t>(0));
                request_tokens = r.value("request_tokens", static_cast<uint64_t>(0));
                total_context_tokens = r.value("total_context_tokens", static_cast<uint64_t>(0));
                max_context_tokens = r.value("max_context_tokens", static_cast<uint64_t>(0));
                context_fill_pct = r.value("context_fill_pct", 0.0);
            } catch (...) {}

            if (total_context_tokens == 0 || max_context_tokens == 0) {
                total_context_tokens =
                    info.value("current_context_tokens", static_cast<uint64_t>(0));
                max_context_tokens =
                    info.value("max_context_tokens", static_cast<uint64_t>(0));
                context_fill_pct = (max_context_tokens > 0)
                    ? (static_cast<double>(total_context_tokens) /
                       static_cast<double>(max_context_tokens) * 100.0)
                    : 0.0;
            }

            std::ostringstream ss;
            ss << "── Session Info ────────────────────────\n";
            ss << "  session_id : " << session->active_session_id << "\n";
            ss << "  super_user : " << session->super_user << "\n";
            ss << "  title      : "
               << info.value("title", std::string("(untitled)")) << "\n";
            ss << "  turns      : " << info.value("turn_count", 0) << "\n";
            ss << "  tokens     : " << total_context_tokens
               << " / " << max_context_tokens
               << "  (" << static_cast<int>(context_fill_pct) << "%)\n";
            ss << "  breakdown  : session=" << session_tokens
               << ", system=" << system_prompt_tokens
                    << ", tools=" << tool_schema_tokens << "\n";
            ss << "────────────────────────────────────────\n";
            send({{"type","token"},{"data", ss.str()}});
            return true;
        };

        // ── /context ──────────────────────────────────────────────────
        command_table_["/context"] = [this](auto session, const auto& send,
                                             const std::string& /*args*/) {
            const std::string raw =
                send_session_query("info", "", session->active_session_id);
            uint64_t current_tokens = 0;
            uint64_t max_tokens = 0;
            try {
                const json r = json::parse(raw);
                current_tokens = r.value("total_context_tokens", static_cast<uint64_t>(0));
                max_tokens = r.value("max_context_tokens", static_cast<uint64_t>(0));
                if (current_tokens == 0 || max_tokens == 0) {
                    const json s = r.value("session", json::object());
                    current_tokens = s.value("current_context_tokens",
                                             static_cast<uint64_t>(0));
                    max_tokens = s.value("max_context_tokens",
                                         static_cast<uint64_t>(0));
                }
            } catch (...) {
                max_tokens = 0;
            }
            if (max_tokens == 0) {
                send({{"type","token"},{"data","[Context info unavailable]\n"}});
                return true;
            }
            const double pct = static_cast<double>(current_tokens) /
                               static_cast<double>(max_tokens) * 100.0;
            std::ostringstream ss;
            ss << "[Context: " << current_tokens << " / " << max_tokens
               << " tokens  (" << static_cast<int>(pct) << "%)]\n";
            send({{"type","token"},{"data", ss.str()}});
            return true;
        };

        // ── /model_info ───────────────────────────────────────────────
        command_table_["/model_info"] = [this](auto /*s*/, const auto& send,
                            const std::string& /*args*/) {
                const std::string raw = send_session_query("model_info", "", "");
                json info = json::object();
                try {
                     info = json::parse(raw);
                } catch (...) {}
            std::ostringstream ss;
            ss << "── Model Info ──────────────────────────\n";
            ss << "  model_name     : " << info.value("model_name","?") << "\n";
            ss << "  model_type     : " << info.value("model_type","?") << "\n";
            ss << "  active_adapter : " << info.value("active_adapter","?") << "\n";
            ss << "  context_tokens : "
                    << info.value("max_context_tokens", static_cast<uint64_t>(0))
                    << "\n";
            ss << "  max_parallel   : "
                    << info.value("max_simultaneous_llm_requests", 0) << "\n";
            ss << "  enabled        : "
               << (info.value("enabled",true) ? "yes" : "no") << "\n";
            ss << "  compact_thresh : "
                    << info.value("auto_compact_threshold_pct", 70) << "%\n";
            ss << "────────────────────────────────────────\n";
            send({{"type","token"},{"data", ss.str()}});
            return true;
        };

        // ── /scheduler_info ───────────────────────────────────────────
        // Asks the scheduler for queue_depth (one SESSION_QUERY round-trip).
        command_table_["/scheduler_info"] = [this](auto /*s*/, const auto& send,
                                                     const std::string& /*args*/) {
            const std::string raw = send_session_query("queue_depth", "", "");
            std::ostringstream ss;
            ss << "── Scheduler Info ──────────────────────\n";
            try {
                const json r = json::parse(raw);
                ss << "  total_pending  : " << r.value("total_pending", 0) << "\n";
                ss << "  queue_depth    : " << r.value("queue_depth", 0) << "\n";
            } catch (...) {
                ss << "  (unavailable)\n";
            }
            ss << "────────────────────────────────────────\n";
            send({{"type","token"},{"data", ss.str()}});
            return true;
        };

    } // register_commands()

    // ------------------------------------------------------------------
    // Session switch (in-band request from reader)
    // ------------------------------------------------------------------

    void handle_switch_session(const json& j,
                               std::shared_ptr<Session> session) {
        const std::string target_sid = j.value("session_id", std::string(""));
        if (target_sid.empty()) return;

        // Ensure it belongs to this super_user.
        const std::string su = extract_super_user(target_sid);
        if (su != session->super_user) return;

        {
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            if (sessions_.count(target_sid) > 0 &&
                sessions_.at(target_sid).get() != session.get()) {
                // Another live socket owns it.
                std::lock_guard<std::mutex> slk(session->socket_mutex);
                try {
                    send_json(session->socket,
                              json{{"type","error"},
                                   {"message","session already active"},
                                   {"session_id", target_sid}}.dump());
                } catch (...) {}
                return;
            }
            sessions_.erase(session->user_id);
            session->user_id           = target_sid;
            session->active_session_id = target_sid;
            sessions_[target_sid]      = session;
        }
        try {
            std::lock_guard<std::mutex> slk(session->socket_mutex);
            send_json(session->socket,
                      json{{"type","session_switched"},
                           {"session_id", target_sid}}.dump());
        } catch (...) {}
    }

    // ------------------------------------------------------------------
    // Approval reply
    // ------------------------------------------------------------------

    void handle_approval_reply(const json& j,
                                std::shared_ptr<Session> session) {
        const std::string atrace = j.value("approval_trace", std::string(""));
        const std::string scope  = j.value("scope", std::string("deny"));
        int target_pid = -1;
        {
            std::lock_guard<std::mutex> lk(approval_map_mutex_);
            auto it = approval_trace_to_pid_.find(atrace);
            if (it != approval_trace_to_pid_.end()) {
                target_pid = it->second;
                approval_trace_to_pid_.erase(it);
            }
        }
        if (target_pid > 0 && !atrace.empty()) {
            send_message(target_pid, "APPROVAL_REPLY",
                         {{"approval_trace", atrace}, {"scope", scope}});
        }
        std::lock_guard<std::mutex> slk(session->socket_mutex);
        try {
            send_json(session->socket,
                      json{{"type","approval_ack"},
                           {"approval_trace", atrace},
                           {"target_pid",     target_pid}}.dump());
        } catch (...) {}
    }

    // ------------------------------------------------------------------
    // Session teardown
    // ------------------------------------------------------------------

    void teardown_session(std::shared_ptr<Session> session) {
        session->active.store(false);
        session->queue_cv.notify_all();
        {
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            sessions_.erase(session->user_id);
        }
        {
            std::lock_guard<std::mutex> lk(session->socket_mutex);
            if (session->socket.is_open()) {
                try { session->socket.close(); } catch (...) {}
            }
        }
        std::cout << "[Handler] Session ended: " << session->user_id << std::endl;
    }

    // ------------------------------------------------------------------
    // Broadcast helpers
    // ------------------------------------------------------------------

    // Direct send (does NOT go through work queue).
    void send_to_session(const std::string& uid, const json& event) {
        std::shared_ptr<Session> s;
        {
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            auto it = sessions_.find(uid);
            if (it == sessions_.end()) return;
            s = it->second;
        }
        std::lock_guard<std::mutex> slk(s->socket_mutex);
        if (s->socket.is_open()) {
            try { send_json(s->socket, event.dump()); } catch (...) {}
        }
    }

    // Enqueue into the work queue (will be processed by the worker thread).
    void inject_to_session(const std::string& uid, const json& msg) {
        std::shared_ptr<Session> s;
        {
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            auto it = sessions_.find(uid);
            if (it == sessions_.end()) return;
            s = it->second;
        }
        {
            std::lock_guard<std::mutex> lk(s->queue_mutex);
            s->request_queue.push(msg.dump());
        }
        s->queue_cv.notify_one();
    }

    void broadcast_event(const json& event) {
        std::vector<std::shared_ptr<Session>> snap;
        {
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            snap.reserve(sessions_.size());
            for (auto& [_, s] : sessions_) snap.push_back(s);
        }
        for (auto& s : snap) {
            std::lock_guard<std::mutex> lk(s->socket_mutex);
            if (s->socket.is_open()) {
                try { send_json(s->socket, event.dump()); } catch (...) {}
            }
        }
    }

    void broadcast_injection(const json& msg) {
        std::vector<std::shared_ptr<Session>> snap;
        {
            std::lock_guard<std::mutex> lk(sessions_mutex_);
            snap.reserve(sessions_.size());
            for (auto& [_, s] : sessions_) snap.push_back(s);
        }
        for (auto& s : snap) {
            std::lock_guard<std::mutex> lk(s->queue_mutex);
            s->request_queue.push(msg.dump());
            s->queue_cv.notify_one();
        }
    }

    // ------------------------------------------------------------------
    // Scheduler communication helpers
    // ------------------------------------------------------------------

    // SESSION_CONTROL: returns raw JSON string from scheduler.
    std::string send_session_control(const std::string& user_id,
                                     const std::string& action,
                                     const std::string& title) {
        constexpr int kPort        = 5171;
        constexpr int kRetries     = 3;
        constexpr int kRetryMs     = 300;
        constexpr int kDefaultTimeoutMs = 8000;

        auto load_scheduler_wait_timeout_ms = []() {
            std::ifstream model_file("config/model.json");
            if (!model_file.is_open()) {
                model_file.open("../config/model.json");
            }
            if (!model_file.is_open()) {
                model_file.open("build/config/model.json");
            }
            if (!model_file.is_open()) {
                return 65000;
            }
            try {
                json model;
                model_file >> model;
                const int request_timeout_ms =
                    model.value("request_timeout_ms", 60000);
                return request_timeout_ms + 5000;
            } catch (...) {
                return 65000;
            }
        };

        const int kTimeoutMs =
            (action == "compact")
                ? std::max(load_scheduler_wait_timeout_ms() + 2000,
                           kDefaultTimeoutMs)
                : kDefaultTimeoutMs;

        try {
            SocketWrapper sock;
            sock.create_tcp_socket();
            sock.set_timeout_ms(kTimeoutMs);

            bool ok = false;
            for (int i = 0; i < kRetries && !ok; ++i) {
                try { sock.connect("127.0.0.1", kPort); ok = true; }
                catch (...) {
                    if (i + 1 < kRetries)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(kRetryMs));
                }
            }
            if (!ok) return "";

            json frame = {{"message_type","SESSION_CONTROL"},
                          {"action", action},
                          {"user_id", user_id}};
            if (!title.empty()) frame["title"] = title;
            send_json(sock, frame.dump());

            const std::string raw  = recv_json(sock);
            const json        resp = json::parse(raw);

            if (resp.value("message_type","") != "SESSION_RESPONSE") return "";

            // Actions that need full JSON back.
            if (action == "list" || action == "compact" ||
                action == "undo"  || action == "set_title" ||
                action == "list_super_users") {
                return raw;
            }

            // For get_or_create / new: return resolved session_id.
            // The scheduler returns it as session.session_id or session_id.
            if (resp.contains("session") && resp["session"].is_object()) {
                const std::string sid =
                    resp["session"].value("session_id", std::string(""));
                if (!sid.empty()) return sid;
            }
            return resp.value("session_id", std::string(""));

        } catch (const std::exception& e) {
            std::cerr << "[Handler] SESSION_CONTROL(" << action << "): "
                      << e.what() << " (timeout_ms=" << kTimeoutMs << ")"
                      << std::endl;
            return "";
        }
    }

    // SESSION_QUERY: returns raw JSON string from scheduler.
    std::string send_session_query(const std::string& query_type,
                                   const std::string& queue_key,
                                   const std::string& user_id) {
        constexpr int kPort      = 5171;
        constexpr int kTimeoutMs = 5000;
        try {
            SocketWrapper sock;
            sock.create_tcp_socket();
            sock.set_timeout_ms(kTimeoutMs);
            sock.connect("127.0.0.1", kPort);

            json frame = {{"message_type","SESSION_QUERY"},
                          {"query_type",  query_type}};
            if (!queue_key.empty()) frame["queue_key"] = queue_key;
            if (!user_id.empty()) frame["user_id"] = user_id;
            send_json(sock, frame.dump());
            return recv_json(sock);
        } catch (...) {
            return "{}";
        }
    }

    // ------------------------------------------------------------------
    // User management helpers
    // ------------------------------------------------------------------

    void initialize_known_users() {
        std::unordered_set<std::string> found;
        try {
            const std::string raw = send_session_query("all_sessions", "", "");
            const json resp = json::parse(raw);
            const json users = resp.value("super_users", json::array());
            for (const auto& u : users) {
                if (!u.is_object()) {
                    continue;
                }
                const std::string super_user =
                    u.value("super_user", std::string(""));
                if (!super_user.empty()) {
                    found.insert(super_user);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Handler] initialize_known_users query: "
                      << e.what() << std::endl;
        }

        std::lock_guard<std::mutex> lk(known_users_mutex_);
        known_users_ = std::move(found);
    }

    void ensure_known_user(const std::string& super_user) {
        bool is_new = false;
        {
            std::lock_guard<std::mutex> lk(known_users_mutex_);
            is_new = known_users_.insert(super_user).second;
        }
        if (!is_new) return;

        // Ask the scheduler to create the super_user in its session store.
        send_session_control(super_user, "create_super_user", "");

        // Mirror agentfiles on disk for local memory reads.
        try {
            const fs::path af = fs::path("memory") / "agentfiles" / super_user;
            fs::create_directories(af);
            for (const char* f : {"soul.md", "user.md"}) {
                const fs::path src = fs::path("memory") / f;
                const fs::path dst = af / f;
                if (fs::exists(src) && !fs::exists(dst))
                    fs::copy(src, dst);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Handler] ensure_known_user agentfiles: "
                      << e.what() << std::endl;
        }
    }

    // ------------------------------------------------------------------
    // Thread-local user context
    // ------------------------------------------------------------------

    void set_current_user_context(const std::string& uid) {
        std::lock_guard<std::mutex> lk(thread_ctx_mutex_);
        thread_ctx_[std::this_thread::get_id()] = uid;
    }

    void clear_current_user_context() {
        std::lock_guard<std::mutex> lk(thread_ctx_mutex_);
        thread_ctx_.erase(std::this_thread::get_id());
    }

    std::string current_user_context() const {
        std::lock_guard<std::mutex> lk(thread_ctx_mutex_);
        auto it = thread_ctx_.find(std::this_thread::get_id());
        return (it == thread_ctx_.end()) ? "" : it->second;
    }

    std::string resolve_uid_from_context_or_args(const json& args) const {
        std::string uid = current_user_context();
        if (uid.empty() && args.is_object() &&
            args.contains("user_id") && args["user_id"].is_string())
            uid = args["user_id"].get<std::string>();
        return uid;
    }

    std::string resolve_uid_from_context_or_result(const json& result) const {
        std::string uid = current_user_context();
        if (uid.empty() && result.is_object() &&
            result.contains("user_id") && result["user_id"].is_string())
            uid = result["user_id"].get<std::string>();
        return uid;
    }

    // ------------------------------------------------------------------
    // Static validation helpers
    // ------------------------------------------------------------------

    static bool is_session_id_format(const std::string& v) {
        const auto pos = v.rfind("_s");
        if (pos == std::string::npos || pos + 2 >= v.size()) return false;
        const std::string suf = v.substr(pos + 2);
        return !suf.empty() &&
               std::all_of(suf.begin(), suf.end(),
                            [](unsigned char c){ return std::isdigit(c) != 0; });
    }

    static std::string extract_super_user(const std::string& v) {
        if (!is_session_id_format(v)) return v;
        return v.substr(0, v.rfind("_s"));
    }

    static bool is_valid_user_id(const std::string& uid) {
        if (uid.empty() || uid.size() > 128) return false;
        for (char c : uid) {
            const bool alnum = (c >= 'a' && c <= 'z') ||
                               (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9');
            if (!alnum && c != '_' && c != '-') return false;
        }
        return true;
    }

    // ------------------------------------------------------------------
    // Member variables
    // ------------------------------------------------------------------

    HandlerConfig config_;

    SocketWrapper server_;

    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    std::mutex sessions_mutex_;

    std::unordered_set<std::string> known_users_;
    std::mutex known_users_mutex_;

    std::unordered_map<std::string, int> approval_trace_to_pid_;
    std::mutex approval_map_mutex_;

    mutable std::mutex thread_ctx_mutex_;
    std::unordered_map<std::thread::id, std::string> thread_ctx_;

    std::unordered_map<std::string, CommandFn> command_table_;
};

// =============================================================================
// main
// =============================================================================

int main() {
    Handler h;
    h.start();
    return 0;
}