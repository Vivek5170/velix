/**
 * application_manager.cpp
 *
 * Network service wrapping TerminalRegistry.
 *
 * Fixes from original:
 *  BUG-22 — stop() join_watchdog() called only once via atomic flag.
 *  BUG-23 — Auth token comparison uses constant-time compare.
 *  BUG-20 — EXECUTE now accepts a full driver_config JSON blob so SSH/Docker
 *            params flow through cleanly.
 *  NEW    — CANCEL_JOB message type added.
 *  NEW    — driver_config JSON field parsed into DriverConfig and passed
 *            to registry->get_or_create().
 *
 * ── Wire protocol ────────────────────────────────────────────────────────────
 *
 *  All messages are newline-framed JSON.  Each request → one response.
 *
 *  EXECUTE
 *    Request:
 *      {
 *        "message_type":  "EXECUTE",
 *        "user_id":       "<string>",
 *        "session_name":  "<string>",        // optional, default "default"
 *        "cmd":           "<shell command>",
 *        "timeout_sec":   0,                  // 0 = no timeout
 *        "driver_config": {                   // optional; defaults to local_pty
 *          "type":             "local_pty",   // or "ssh" or "docker"
 *          "shell":            "",
 *          "ssh_host":         "",
 *          "ssh_port":         22,
 *          "ssh_user":         "",
 *          "ssh_key_path":     "",
 *          "docker_container": "",
 *          "docker_user":      "",
 *          "docker_shell":     ""
 *        }
 *      }
 *    Response:
 *      { "status": "ok", "job_id": "<string>",
 *        "session_created": <bool>, "session_id": "<string>" }
 *
 *  POLL
 *    Request:  { "message_type": "POLL", "job_id": "<string>" }
 *    Response: { "status": "ok", "job_status": "...", "exit_code": <int>, "output": "..." }
 *
 *  CANCEL_JOB
 *    Request:  { "message_type": "CANCEL_JOB", "user_id": "...", "session_name": "..." }
 *    Response: { "status": "ok", "sent": <bool> }
 *
 *  KILL_SESSION
 *    Request:  { "message_type": "KILL_SESSION", "user_id": "...", "session_name": "..." }
 *              session_name may be omitted to kill all sessions for user_id.
 *    Response: { "status": "ok" }
 *
 *  LIST_SESSIONS
 *    Request:  { "message_type": "LIST_SESSIONS" }
 *              { "message_type": "LIST_SESSIONS", "user_id": "<string>" }
 *    Response: { "status": "ok", "sessions": [ { session info … }, … ] }
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "application_manager.hpp"
#include "terminal_registry.hpp"

#include "../../communication/network_config.hpp"
#include "../../communication/socket_wrapper.hpp"
#include "../../utils/config_utils.hpp"
#include "../../utils/logger.hpp"
#include "../../utils/thread_pool.hpp"
#include "../../vendor/nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <cstring>   // for memcmp (constant-time auth)
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using json = nlohmann::json;

namespace velix::app_manager {

namespace {

constexpr int kDefaultPort          = 5175;
constexpr int kWatchdogIntervalMs   = 5000;
constexpr int kJobPruneIntervalMs   = 60000;

// ─────────────────────────────────────────────────────────────────────────────
// Config
// ─────────────────────────────────────────────────────────────────────────────

struct AppManagerConfig {
    int         max_client_threads   = 64;
    int         watchdog_interval_ms = kWatchdogIntervalMs;
    int         idle_timeout_min     = 15;
    int         max_sessions_total   = 64;
    int         max_sessions_per_user = 8;
    int         job_retention_min    = 60;
    bool        require_auth_token   = false;
    std::string auth_token;
};

static AppManagerConfig load_config() {
    AppManagerConfig cfg;
    const char *paths[] = {
        "config/application_manager.json",
        "../config/application_manager.json",
        "build/config/application_manager.json"
    };
    std::ifstream file;
    for (const char *p : paths) {
        file.open(p);
        if (file.is_open()) break;
    }
    if (!file.is_open()) {
        LOG_WARN_CTX("application_manager.json not found, using defaults",
                     "app_manager", "", -1, "config_default");
        return cfg;
    }
    try {
        json j;
        file >> j;
        cfg.max_client_threads    = j.value("max_client_threads",    cfg.max_client_threads);
        cfg.watchdog_interval_ms  = j.value("watchdog_interval_ms",  cfg.watchdog_interval_ms);
        cfg.idle_timeout_min      = j.value("idle_timeout_min",      cfg.idle_timeout_min);
        cfg.max_sessions_total    = j.value("max_sessions_total",    cfg.max_sessions_total);
        cfg.max_sessions_per_user = j.value("max_sessions_per_user", cfg.max_sessions_per_user);
        cfg.job_retention_min     = j.value("job_retention_min",     cfg.job_retention_min);
        cfg.require_auth_token    = j.value("require_auth_token",    cfg.require_auth_token);
        cfg.auth_token            = j.value("auth_token",            cfg.auth_token);
        LOG_INFO_CTX("Loaded application_manager config", "app_manager", "", -1, "config_loaded");
    } catch (const std::exception &e) {
        LOG_ERROR_CTX(std::string("Failed to parse application_manager.json: ") + e.what(),
                      "app_manager", "", -1, "config_parse_error");
    }
    return cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constant-time string compare (BUG-23 fix)
// ─────────────────────────────────────────────────────────────────────────────

static bool constant_time_equal(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    // XOR all bytes; result is 0 iff all equal.
    unsigned char result = 0;
    for (size_t i = 0; i < a.size(); ++i)
        result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return result == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Expected-disconnect noise check
// ─────────────────────────────────────────────────────────────────────────────

static bool is_expected_disconnect(const std::string &err) {
    return err.find("Broken pipe")       != std::string::npos ||
           err.find("connection closed") != std::string::npos ||
           err.find("Connection reset")  != std::string::npos ||
           err.find("errno 32")          != std::string::npos ||
           err.find("errno 104")         != std::string::npos;
}

// ─────────────────────────────────────────────────────────────────────────────
// ApplicationManagerService
// ─────────────────────────────────────────────────────────────────────────────

class ApplicationManagerService {
public:
    ApplicationManagerService()
        : config_(load_config())
        , thread_pool_(config_.max_client_threads, config_.max_client_threads * 4) {

        registry_ = std::make_shared<TerminalRegistry>();
        registry_->max_sessions_total    = config_.max_sessions_total;
        registry_->max_sessions_per_user = config_.max_sessions_per_user;
        registry_->idle_timeout_ms =
            static_cast<uint64_t>(config_.idle_timeout_min) * 60 * 1000;
        registry_->job_retention_ms =
            static_cast<uint64_t>(config_.job_retention_min) * 60 * 1000;
    }

    ~ApplicationManagerService() { stop(); }

    void start(int port) {
        if (running_.exchange(true)) return; // already running

        LOG_INFO_CTX("ApplicationManager starting on port " + std::to_string(port),
                     "app_manager", "", -1, "startup");

        watchdog_thread_ = std::thread([this] { watchdog_loop(); });

        try {
            const std::string bind_host =
                velix::communication::resolve_bind_host("APP_MANAGER", "127.0.0.1");

            {
                std::scoped_lock lk(server_mx_);
                server_socket_ = std::make_shared<velix::communication::SocketWrapper>();
                server_socket_->create_tcp_socket();
                server_socket_->bind(bind_host, static_cast<uint16_t>(port));
                server_socket_->listen(8);
            }

            LOG_INFO_CTX("ApplicationManager listening on " + bind_host + ":" +
                         std::to_string(port), "app_manager", "", -1, "listen");

            while (running_) {
                try {
                    std::shared_ptr<velix::communication::SocketWrapper> server;
                    {
                        std::scoped_lock lk(server_mx_);
                        server = server_socket_;
                    }
                    if (!server || !server->is_open()) break;

                    auto client_ptr = std::make_shared<velix::communication::SocketWrapper>(
                        server->accept());

                    const bool submitted = thread_pool_.try_submit(
                        [this, client_ptr]() mutable {
                            try { handle_client(std::move(*client_ptr)); } catch (...) {}
                        });

                    if (!submitted) {
                        try {
                            LOG_WARN_CTX("Thread pool busy, rejecting request", "app_manager", "", -1, "pool_full");
                            json busy = {{"status", "error"},
                                         {"error", "app_manager busy: thread pool full"}};
                            velix::communication::send_json(*client_ptr, busy.dump());
                        } catch (...) {}
                    }
                } catch (const std::exception &e) {
                    if (!running_) break;
                    LOG_WARN_CTX(std::string("accept error: ") + e.what(),
                                 "app_manager", "", -1, "accept_error");
                }
            }

            stop();
        } catch (const std::exception &e) {
            running_.store(false);
            join_watchdog();
            LOG_ERROR_CTX(std::string("ApplicationManager startup failed: ") + e.what(),
                          "app_manager", "", -1, "startup_error");
            throw;
        }
    }

    void stop() {
        if (!running_.exchange(false)) {
            join_watchdog(); // idempotent via watchdog_joined_ flag
            return;
        }

        {
            std::scoped_lock lk(server_mx_);
            if (server_socket_ && server_socket_->is_open())
                server_socket_->close();
            server_socket_.reset();
        }

        join_watchdog();

        // Tear down sessions after watchdog is stopped (no concurrent eviction).
        for (const auto &info : registry_->snapshot())
            registry_->kill_session_by_id(info.session_id);

        LOG_INFO_CTX("ApplicationManager stopped", "app_manager", "", -1, "shutdown");
    }

private:
    AppManagerConfig config_;
    std::shared_ptr<TerminalRegistry> registry_;
    velix::utils::ThreadPool thread_pool_;

    std::mutex server_mx_;
    std::shared_ptr<velix::communication::SocketWrapper> server_socket_;

    std::atomic<bool> running_{false};

    // BUG-22 fix: track whether watchdog has been joined to prevent UB on
    // calling join() twice.
    std::mutex        watchdog_joined_mx_;
    bool              watchdog_joined_ = false;
    std::thread       watchdog_thread_;

    void join_watchdog() {
        std::scoped_lock lk(watchdog_joined_mx_);
        if (!watchdog_joined_ && watchdog_thread_.joinable()) {
            watchdog_thread_.join();
            watchdog_joined_ = true;
        }
    }

    // ── Client handling ──────────────────────────────────────────────

    void handle_client(velix::communication::SocketWrapper sock) {
        try {
            const std::string raw  = velix::communication::recv_json(sock);
            const json request     = json::parse(raw);
            const json response    = dispatch(request);
            velix::communication::send_json(sock, response.dump());
        } catch (const std::exception &e) {
            const std::string err = e.what();
            if (is_expected_disconnect(err)) return;
            LOG_ERROR_CTX(std::string("client error: ") + err,
                          "app_manager", "", -1, "client_error");
            try {
                json resp = {{"status", "error"}, {"error", err}};
                velix::communication::send_json(sock, resp.dump());
            } catch (...) {}
        }
    }

    // ── Message dispatch ─────────────────────────────────────────────

    json dispatch(const json &req) {
        if (!req.is_object() || !req.contains("message_type") ||
            !req["message_type"].is_string())
            return {{"status", "error"}, {"error", "missing or invalid message_type"}};

        // Auth check.
        if (config_.require_auth_token) {
            const json meta = req.value("metadata", json::object());
            const std::string token = meta.value("auth_token", "");
            // BUG-23 fix: constant-time compare.
            if (!constant_time_equal(token, config_.auth_token))
                return {{"status", "error"}, {"error", "auth failed"}};
        }

        const std::string msg_type = req["message_type"].get<std::string>();

        if (msg_type == "EXECUTE")      return handle_execute(req);
        if (msg_type == "POLL")         return handle_poll(req);
        if (msg_type == "CANCEL_JOB")   return handle_cancel_job(req);
        if (msg_type == "KILL_SESSION") return handle_kill_session(req);
        if (msg_type == "LIST_SESSIONS")return handle_list_sessions(req);

        return {{"status", "error"}, {"error", "unknown message_type: " + msg_type}};
    }

    // ── EXECUTE ──────────────────────────────────────────────────────

    json handle_execute(const json &req) {
        const std::string user_id      = req.value("user_id", "");
        const std::string session_name = req.value("session_name", "");
        const std::string cmd          = req.value("cmd", "");
        const int         timeout_sec  = req.value("timeout_sec", 0);

        if (user_id.empty())
            return {{"status", "error"}, {"error", "EXECUTE requires user_id"}};
        if (cmd.empty())
            return {{"status", "error"}, {"error", "EXECUTE requires cmd"}};

        // Parse driver_config (BUG-20 fix: full DriverConfig passthrough).
        DriverConfig dcfg;
        if (req.contains("driver_config") && req["driver_config"].is_object()) {
            dcfg = DriverConfig::from_json(req["driver_config"]);
        } else {
            dcfg.type = req.value("driver", "local_pty"); // legacy fallback
        }

        auto gcr = registry_->get_or_create(user_id, session_name, dcfg);
        if (!gcr.success)
            return {{"status", "error"}, {"error", gcr.error}};

        std::string job_id;
        try {
            job_id = registry_->exec(user_id, session_name, cmd, timeout_sec);
        } catch (const std::exception &e) {
            return {{"status", "error"},
                    {"error", std::string("exec failed: ") + e.what()}};
        }

        LOG_INFO_CTX("Submitted job " + job_id + " for session " + gcr.session_id,
                     "app_manager", user_id, -1, "job_submitted");

        return {{"status",         "ok"},
                {"job_id",         job_id},
                {"session_id",     gcr.session_id},
                {"session_created", !gcr.was_existing}};
    }

    // ── POLL ─────────────────────────────────────────────────────────

    json handle_poll(const json &req) {
        const std::string job_id = req.value("job_id", "");
        if (job_id.empty())
            return {{"status", "error"}, {"error", "POLL requires job_id"}};

        auto job = registry_->poll_job(job_id);
        if (!job)
            return {{"status", "error"}, {"error", "unknown job_id: " + job_id}};

        return {{"status",     "ok"},
                {"job_status", to_string(job->status)},
                {"exit_code",  job->exit_code},
                {"output",     job->output}};
    }

    // ── CANCEL_JOB ───────────────────────────────────────────────────

    json handle_cancel_job(const json &req) {
        const std::string user_id      = req.value("user_id", "");
        const std::string session_name = req.value("session_name", "");
        if (user_id.empty())
            return {{"status", "error"}, {"error", "CANCEL_JOB requires user_id"}};

        bool sent = registry_->cancel_job(user_id, session_name);
        return {{"status", "ok"}, {"sent", sent}};
    }

    // ── KILL_SESSION ─────────────────────────────────────────────────

    json handle_kill_session(const json &req) {
        const std::string user_id      = req.value("user_id", "");
        const std::string session_name = req.value("session_name", "");
        if (user_id.empty())
            return {{"status", "error"}, {"error", "KILL_SESSION requires user_id"}};

        registry_->kill_session(user_id, session_name);
        LOG_INFO_CTX("Session(s) killed by request", "app_manager", user_id, -1, "session_killed");
        return {{"status", "ok"}};
    }

    // ── LIST_SESSIONS ────────────────────────────────────────────────

    json handle_list_sessions(const json &req) {
        const std::string user_id = req.value("user_id", "");
        const auto sessions = user_id.empty() ? registry_->snapshot()
                                              : registry_->snapshot(user_id);
        json arr = json::array();
        for (const auto &info : sessions) {
            arr.push_back({
                {"session_id",   info.session_id},
                {"user_id",      info.user_id},
                {"session_name", info.session_name},
                {"driver_type",  info.driver_type},
                {"status",       to_string(info.status)},
                {"created_at_ms",info.created_at_ms},
                {"last_used_ms", info.last_used_ms},
                {"last_error",   info.last_error_msg}
            });
        }
        return {{"status", "ok"}, {"sessions", arr}};
    }

    // ── Watchdog ─────────────────────────────────────────────────────

    void watchdog_loop() {
        uint64_t last_prune_ms = 0;

        while (running_) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.watchdog_interval_ms));
            if (!running_) break;

            const auto dead = registry_->detect_dead_sessions();
            if (!dead.empty())
                LOG_WARN_CTX("Watchdog detected " + std::to_string(dead.size()) +
                             " dead session(s)", "app_manager", "", -1, "watchdog_dead");

            const int evicted = registry_->evict_dead_and_idle();
            if (evicted > 0)
                LOG_INFO_CTX("Watchdog evicted " + std::to_string(evicted) +
                             " session(s)", "app_manager", "", -1, "watchdog_evict");

            const uint64_t now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            if (now_ms - last_prune_ms > static_cast<uint64_t>(kJobPruneIntervalMs)) {
                registry_->prune_jobs();
                last_prune_ms = now_ms;
            }
        }
    }
};

// Singleton
ApplicationManagerService &service_instance() {
    static ApplicationManagerService svc;
    return svc;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void start_application_manager(int port) {
    if (port <= 0)
        port = velix::utils::get_port("APP_MANAGER", kDefaultPort);
    service_instance().start(port);
}

void stop_application_manager() {
    service_instance().stop();
}

} // namespace velix::app_manager