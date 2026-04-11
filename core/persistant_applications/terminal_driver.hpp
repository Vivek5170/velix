#pragma once

/**
 * terminal_driver.hpp
 *
 * TerminalDriver interface and concrete drivers.
 *
 * Driver hierarchy (extend here, nothing else changes):
 *   TerminalDriver   — abstract interface
 *   LocalPTYDriver   — forkpty() on POSIX / ConPTY on Windows
 *   SSHDriver        — remote shell via libssh2
 *   DockerDriver     — `docker exec` into a running container (wraps LocalPTY)
 *   K8sDriver        — future: kubectl exec
 *
 * A Driver owns exactly one persistent shell process or connection.
 *
 * Public API:
 *   exec()              — submit a command, return job_id immediately.
 *   run_cmd()           — one-shot subprocess; blocks until done.
 *   cancel_current_job()— send SIGINT / Ctrl+C to the running job.
 *   close()             — clean shutdown.
 *   is_alive()          — true when the shell/connection is up.
 *
 * Thread safety: ALL public methods are safe to call from any thread.
 *
 * ── Sentinel protocol ───────────────────────────────────────────────────────
 *  Injected markers facilitate job state tracking without polling:
 *
 *    START frame: RS VELIX_START US token US job_id US pid US
 *    END frame:   RS VELIX_END   US token US job_id US exit_code US
 *
 *  RS (Record Separator) = \x1E
 *  US (Unit Separator)   = \x1F
 *
 *  These choices are safer than STX/ETX as they never conflict with TTY
 *  line discipline signal characters (like \x03 = SIGINT).
 *
 *  PTY echo is disabled so these characters never leak into job output.
 *  A random session token prevents spoofing by untrusted output.
 *
 *  A partial-frame flood guard limits accumulated unparsed bytes to
 *  kMaxPendingFrameBytes before the partial frame is discarded as output.
 * ────────────────────────────────────────────────────────────────────────────
 */

#include "../../vendor/nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <Windows.h>
#  include <winsock2.h>
#else
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <termios.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <util.h>
#  else
#    include <pty.h>
#  endif
#endif

namespace velix::app_manager {

using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// Sentinel constants  (one definition — used by drivers and parser alike)
// ─────────────────────────────────────────────────────────────────────────────

inline static constexpr char kFrameStart     = '\x1E'; // RS (Record Separator)
inline static constexpr char kFieldSeparator = '\x1F'; // US (Unit Separator)
inline static constexpr std::string_view kStartTag = "VELIX_START";
inline static constexpr std::string_view kEndTag   = "VELIX_END";

// Max bytes buffered while waiting for a partial frame to complete.
// If exceeded the STX byte is re-emitted as literal output and scanning
// continues, preventing a malformed frame from blocking the parser.
inline constexpr size_t kMaxPendingFrameBytes = 4096;

// ─────────────────────────────────────────────────────────────────────────────
// JobStatus
// ─────────────────────────────────────────────────────────────────────────────

enum class JobStatus { Running, Finished, Error, Timeout };

inline std::string to_string(JobStatus s) {
    switch (s) {
    case JobStatus::Running:  return "running";
    case JobStatus::Finished: return "finished";
    case JobStatus::Error:    return "error";
    case JobStatus::Timeout:  return "timeout";
    }
    return "error";
}

// ─────────────────────────────────────────────────────────────────────────────
// Job — in-flight or completed command result
// ─────────────────────────────────────────────────────────────────────────────

struct Job {
    std::string  job_id;
    std::string  session_id;        // "user_id:session_name"
    std::string  cmd;
    JobStatus    status          = JobStatus::Running;
    int          exit_code       = -1;
    std::string  output;            // merged stdout+stderr (PTY = single stream)
    uint64_t     submitted_at_ms = 0;
    uint64_t     finished_at_ms  = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// ExecResult — return type for one-shot run_cmd()
// ─────────────────────────────────────────────────────────────────────────────

struct ExecResult {
    std::string out;
    std::string err;
    int         exit_code = 0;
    bool        timed_out = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// JobStore — thread-safe map: job_id → Job
// ─────────────────────────────────────────────────────────────────────────────

class JobStore {
public:
    void upsert(const Job &job) {
        std::scoped_lock lk(mx_);
        jobs_[job.job_id] = job;
    }

    std::shared_ptr<Job> get(const std::string &job_id) const {
        std::scoped_lock lk(mx_);
        auto it = jobs_.find(job_id);
        return it == jobs_.end() ? nullptr : std::make_shared<Job>(it->second);
    }

    void append_output(const std::string &job_id, std::string_view chunk) {
        if (chunk.empty()) return;
        std::scoped_lock lk(mx_);
        auto it = jobs_.find(job_id);
        if (it != jobs_.end()) it->second.output.append(chunk);
    }

    void finish(const std::string &job_id, JobStatus status, int exit_code) {
        std::scoped_lock lk(mx_);
        auto it = jobs_.find(job_id);
        if (it != jobs_.end()) {
            it->second.status         = status;
            it->second.exit_code      = exit_code;
            it->second.finished_at_ms = now_ms();
        }
    }

    void prune(uint64_t max_age_ms) {
        const uint64_t now = now_ms();
        std::scoped_lock lk(mx_);
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            const Job &j = it->second;
            if (j.status != JobStatus::Running &&
                now - j.finished_at_ms > max_age_ms)
                it = jobs_.erase(it);
            else
                ++it;
        }
    }

private:
    mutable std::mutex mx_;
    std::unordered_map<std::string, Job> jobs_;

    static uint64_t now_ms() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// DriverConfig — all parameters for any driver type.
//
// New driver types add fields here; callers that don't care about those
// fields leave them at their defaults.
// ─────────────────────────────────────────────────────────────────────────────

struct DriverConfig {
    std::string type;           // "local_pty" | "ssh" | "docker" | "k8s"

    // ── local_pty ────────────────────────────────────────────────────
    std::string shell;          // binary path override (empty → $SHELL / bash)

    // ── ssh ──────────────────────────────────────────────────────────
    std::string ssh_host;
    uint16_t    ssh_port        = 22;
    std::string ssh_user;
    std::string ssh_key_path;   // preferred auth: path to private key
    std::string ssh_password;   // fallback (avoid; prefer key)
    int         ssh_keepalive_s = 30;

    // ── docker ───────────────────────────────────────────────────────
    std::string docker_container;
    std::string docker_user;
    std::string docker_shell;   // default: /bin/sh
    std::string docker_host;    // DOCKER_HOST override (empty = default socket)

    // ── k8s (future) ─────────────────────────────────────────────────
    std::string k8s_namespace;
    std::string k8s_pod;
    std::string k8s_container;
    std::string k8s_kubeconfig;

    // ── terminal geometry ─────────────────────────────────────────────
    int cols = 220;
    int rows  = 50;

    static DriverConfig from_json(const json &j) {
        DriverConfig c;
        auto s = [&](const char *k, std::string &v) {
            if (j.contains(k) && j[k].is_string()) v = j[k].get<std::string>();
        };
        auto i16 = [&](const char *k, uint16_t &v) {
            if (j.contains(k) && j[k].is_number_integer()) v = j[k].get<uint16_t>();
        };
        auto i32 = [&](const char *k, int &v) {
            if (j.contains(k) && j[k].is_number_integer()) v = j[k].get<int>();
        };
        s("type",             c.type);
        s("shell",            c.shell);
        s("ssh_host",         c.ssh_host);
        i16("ssh_port",       c.ssh_port);
        s("ssh_user",         c.ssh_user);
        s("ssh_key_path",     c.ssh_key_path);
        s("ssh_password",     c.ssh_password);
        i32("ssh_keepalive_s",c.ssh_keepalive_s);
        s("docker_container", c.docker_container);
        s("docker_user",      c.docker_user);
        s("docker_shell",     c.docker_shell);
        s("docker_host",      c.docker_host);
        s("k8s_namespace",    c.k8s_namespace);
        s("k8s_pod",          c.k8s_pod);
        s("k8s_container",    c.k8s_container);
        s("k8s_kubeconfig",   c.k8s_kubeconfig);
        i32("cols",           c.cols);
        i32("rows",           c.rows);
        if (c.type.empty()) c.type = "local_pty";
        return c;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TerminalDriver — abstract driver interface
// ─────────────────────────────────────────────────────────────────────────────

class TerminalDriver {
public:
    virtual ~TerminalDriver() = default;

    virtual std::string exec(const std::string &cmd,
                             int timeout_sec,
                             std::shared_ptr<JobStore> job_store) = 0;

    virtual ExecResult run_cmd(const std::string &cmd,
                               const std::vector<std::string> &args,
                               const std::filesystem::path &cwd,
                               int timeout_sec,
                               bool use_pty) = 0;

    virtual bool cancel_current_job() = 0;
    virtual void close() = 0;
    virtual bool is_alive() const = 0;
    virtual std::string driver_type() const = 0;
    virtual const DriverConfig &config() const = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// SentinelParser — shared stateful frame parser for all reader loops.
//
// Feed raw bytes from the shell stream with feed().
// Callbacks are invoked synchronously from the calling thread.
// Not thread-safe — each reader thread owns its own instance.
// ─────────────────────────────────────────────────────────────────────────────

class SentinelParser {
public:
    using OutputCb = std::function<void(std::string_view)>;
    using EndCb    = std::function<void(std::string_view token, std::string_view job_id, int exit_code)>;
    using PidCb    = std::function<void(std::string_view token, std::string_view job_id, long pid)>;

    SentinelParser(OutputCb on_output, PidCb on_pid, EndCb on_end)
        : on_output_(std::move(on_output))
        , on_pid_(std::move(on_pid))
        , on_end_(std::move(on_end))
    {}

    // Feed raw bytes. Callbacks may fire synchronously.
    void feed(const char *data, size_t len);

    // Flush any buffered bytes as output (call when stream ends).
    void flush_remaining();

private:
    OutputCb on_output_;
    PidCb    on_pid_;
    EndCb    on_end_;

    std::string buf_;   // bytes pending parse

    // Try to parse a complete frame starting at buf_[0] (which must be STX).
    // Returns number of bytes consumed (> 0) or 0 if frame is incomplete.
    // On parse failure (not a known tag), returns 1 to skip the STX byte.
    size_t try_parse_frame();
};

// ─────────────────────────────────────────────────────────────────────────────
// LocalPTYDriver
// ─────────────────────────────────────────────────────────────────────────────

class LocalPTYDriver final : public TerminalDriver {
public:
    explicit LocalPTYDriver(DriverConfig cfg = {});
    ~LocalPTYDriver() override { close(); }

    std::string exec(const std::string &cmd,
                     int timeout_sec,
                     std::shared_ptr<JobStore> job_store) override;

    ExecResult run_cmd(const std::string          &cmd,
                       const std::vector<std::string> &args,
                       const std::filesystem::path &cwd,
                       int                          timeout_sec,
                       bool                         use_pty) override;

    bool cancel_current_job() override;
    void close() override;
    bool is_alive() const override { return alive_.load(std::memory_order_acquire); }
    std::string driver_type() const override { return "local_pty"; }
    const DriverConfig &config() const override { return cfg_; }

private:
    DriverConfig      cfg_;
    std::string       session_token_;   // Random token to prevent sentinel spoofing

#ifdef _WIN32
    HPCON   hpc_       = nullptr;
    HANDLE  hProc_     = INVALID_HANDLE_VALUE;
    HANDLE  write_end_ = INVALID_HANDLE_VALUE;
    HANDLE  read_end_  = INVALID_HANDLE_VALUE;
#else
    int     master_fd_ = -1;
    pid_t   shell_pid_ = -1;
#endif

    std::atomic<bool>  alive_{false};
    std::thread        reader_thread_;

    // ── Per-job state ─────────────────────────────────────────────────
    struct PendingJob {
        std::string                           job_id;
        std::shared_ptr<JobStore>             store;
        int                                   timeout_sec = 0;
        std::chrono::steady_clock::time_point submitted_at;
        std::chrono::steady_clock::time_point deadline;  // steady_clock::time_point::max() = no deadline
        bool                                  has_deadline = false;
#ifndef _WIN32
        pid_t   job_pgid = -1;
#else
        DWORD   job_pid  = 0;
#endif
    };

    mutable std::mutex      job_mx_;
    std::condition_variable job_cv_;
    std::shared_ptr<PendingJob> current_job_;   // null when shell is idle

    // ── Timeout watchdog thread ───────────────────────────────────────
    std::thread             timeout_thread_;
    std::mutex              timeout_mx_;
    std::condition_variable timeout_cv_;
    bool                    timeout_stop_ = false;

    // ── Internal helpers ──────────────────────────────────────────────
    void spawn_shell();
    void reader_loop();
    void timeout_loop();

    // Finalize current_job_ under job_mx_. Notifies job_cv_.
    void finish_job_locked(const std::string &job_id, JobStatus st, int code);

    bool write_to_shell(const char *data, size_t len);
    bool write_to_shell(const std::string &s) { return write_to_shell(s.data(), s.size()); }

    static std::string make_job_id();
    static uint64_t    now_ms();
};

// ─────────────────────────────────────────────────────────────────────────────
// DockerDriver — wraps LocalPTYDriver running `docker exec -it <container> sh`
// Reuses all sentinel/timeout machinery for free.
// ─────────────────────────────────────────────────────────────────────────────

class DockerDriver final : public TerminalDriver {
public:
    explicit DockerDriver(DriverConfig cfg);
    ~DockerDriver() override { close(); }

    std::string exec(const std::string &cmd, int timeout_sec,
                     std::shared_ptr<JobStore> store) override;
    ExecResult  run_cmd(const std::string &cmd,
                        const std::vector<std::string> &args,
                        const std::filesystem::path &cwd,
                        int timeout_sec, bool use_pty) override;
    bool cancel_current_job() override;
    void close() override;
    bool is_alive() const override;
    std::string driver_type() const override { return "docker"; }
    const DriverConfig &config() const override { return cfg_; }

private:
    DriverConfig                    cfg_;
    std::atomic<bool>               alive_{false};
    std::unique_ptr<LocalPTYDriver> inner_;

    // Verify the target container is running (called at construction and
    // periodically by is_alive).
    static bool container_is_running(const DriverConfig &cfg);
};

// ─────────────────────────────────────────────────────────────────────────────
// SSHDriver — declared here; implementation in ssh_driver.cpp (requires libssh2)
// ─────────────────────────────────────────────────────────────────────────────

class SSHDriver final : public TerminalDriver {
public:
    explicit SSHDriver(DriverConfig cfg);
    ~SSHDriver() override;

    std::string exec(const std::string &cmd, int timeout_sec,
                     std::shared_ptr<JobStore> store) override;
    ExecResult  run_cmd(const std::string &cmd,
                        const std::vector<std::string> &args,
                        const std::filesystem::path &cwd,
                        int timeout_sec, bool use_pty) override;
    bool cancel_current_job() override;
    void close() override;
    bool is_alive() const override { return alive_.load(std::memory_order_acquire); }
    std::string driver_type() const override { return "ssh"; }
    const DriverConfig &config() const override { return cfg_; }

private:
    DriverConfig      cfg_;
    std::atomic<bool> alive_{false};
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Free utility functions
// ─────────────────────────────────────────────────────────────────────────────

// Safe shell quoting for interpolation into shell commands.
// POSIX: single-quote with embedded-single-quote escaping.
// Windows: double-quote with double-quote doubling.
std::string shell_quote(const std::string &s);

// Resolve and validate a cwd inside a sandbox root.
// Throws std::runtime_error if the resolved path escapes root.
// Symlink-safe: follows symlinks and checks the final real path.
std::filesystem::path resolve_sandbox_cwd(const std::string &sub,
                                          const std::string &root,
                                          bool allow_path_escape = false);

// Driver factory.
std::unique_ptr<TerminalDriver> make_driver(const DriverConfig &cfg);

// One-shot process run (no persistent shell).
ExecResult run_cmd(const std::string          &cmd,
                   const std::vector<std::string> &args,
                   const std::filesystem::path &cwd,
                   int                          timeout_sec,
                   bool                         use_pty);

} // namespace velix::app_manager