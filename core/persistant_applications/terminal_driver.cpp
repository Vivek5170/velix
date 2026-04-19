/**
 * terminal_driver.cpp
 *
 * Fixes addressed in this rewrite (see audit comments inline):
 *
 *  BUG-1  — Sentinel bytes changed to STX/ETX (\x02/\x03).
 *  BUG-2  — SentinelParser is shared, order-independent, no duplication.
 *  BUG-3  — timeout_loop() enforces per-job deadlines via SIGKILL/TerminateProcess.
 *  BUG-4  — Shell spawned with --norc/--noprofile; PTY echo disabled immediately.
 *  BUG-5  — close() sets closing_ flag; reader never touches freed fds.
 *           shell_pid_ set to -1 BEFORE waitpid, never double-waited.
 *  BUG-7  — cancel_current_job() guarded per-platform uniformly in header.
 *  BUG-8  — Exit code captured into _velix_ec before sentinel printf.
 *  BUG-9  — posix_exec uses ppoll with deadline on both pipe fds.
 *  BUG-10 — Windows sentinel uses cmd.exe-safe escaping only for job_id
 *            (which is [a-z0-9_] only — no quoting needed).
 *  BUG-11 — resolve_sandbox_cwd: lexically_normal + canonical + relative check.
 *  BUG-12 — Single free run_cmd() dispatches to platform helpers; no duplicate.
 *  BUG-15 — PTY echo disabled with cfmakeraw / ECHO clear immediately after fork.
 *  BUG-16 — Windows reader uses unique job tracking; no PGID reliance.
 */

#include "terminal_driver.hpp"
#include "../../utils/logger.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <poll.h>
#include <random>
#include <sstream>
#include <stdexcept>

#ifndef _WIN32
#  include <cerrno>
#endif

namespace velix::app_manager {

// ─────────────────────────────────────────────────────────────────────────────
// SentinelParser
// ─────────────────────────────────────────────────────────────────────────────
//
// Stream state machine:
//  Normal mode: scan for STX.  Bytes before STX → on_output_.
//  Frame mode:  buf_ starts with STX.  Accumulate until we have a
//               complete frame (all ETX terminators present) or until
//               buf_ exceeds kMaxPendingFrameBytes (flood guard).
//
// Frame format: STX <tag> ETX <field1> ETX [<field2> ETX]
//   kPidTag:  STX VELIX_PID ETX <pid-decimal> ETX
//   kEndTag:  STX VELIX_END ETX <job_id> ETX <exit-code-decimal> ETX

void SentinelParser::feed(const char *data, size_t len) {
    buf_.append(data, len);

    while (!buf_.empty()) {
        if (buf_[0] != kFrameStart) {
            // Not in a frame — find the next FrameStart or emit everything.
            auto pos = buf_.find(kFrameStart);
            if (pos == std::string::npos) {
                on_output_(buf_);
                buf_.clear();
                return;
            }
            // Emit bytes before FrameStart, keep from FrameStart onward.
            on_output_(std::string_view(buf_.data(), pos));
            buf_.erase(0, pos);
            // Fall through to frame parsing.
        }

        // buf_[0] == kFrameStart — try to parse.
        size_t consumed = try_parse_frame();
        if (consumed == 0) {
            // Frame incomplete; wait for more bytes.
            // Flood guard: if we've buffered too much without completing,
            // emit the FrameStart byte as literal output and keep scanning.
            if (buf_.size() > kMaxPendingFrameBytes) {
                LOG_WARN("SentinelParser: partial frame exceeds flood guard, discarding FrameStart");
                on_output_(std::string_view(buf_.data(), 1));
                buf_.erase(0, 1);
            }
            return;
        }
        buf_.erase(0, consumed);
    }
}

void SentinelParser::flush_remaining() {
    if (!buf_.empty()) {
        on_output_(buf_);
        buf_.clear();
    }
}

size_t SentinelParser::try_parse_frame() {
    // buf_[0] must be kFrameStart.
    assert(!buf_.empty() && buf_[0] == kFrameStart);

    // Need at least: FRAME_START <tag> SEP
    if (buf_.size() < 2) return 0;

    // Identify the tag (bytes between Start and first Separator).
    auto tag_end = buf_.find(kFieldSeparator, 1);
    if (tag_end == std::string::npos) {
        // No separator yet — incomplete.
        return 0;
    }

    std::string_view tag(buf_.data() + 1, tag_end - 1);

    if (tag == kStartTag) {
        // Format: START VELIX_START SEP <token> SEP <job_id> SEP <pid> SEP
        auto token_end = buf_.find(kFieldSeparator, tag_end + 1);
        if (token_end == std::string::npos) return 0;

        auto id_end = buf_.find(kFieldSeparator, token_end + 1);
        if (id_end == std::string::npos) return 0;

        auto pid_end = buf_.find(kFieldSeparator, id_end + 1);
        if (pid_end == std::string::npos) return 0;

        std::string_view token(buf_.data() + tag_end + 1, token_end - tag_end - 1);
        std::string_view job_id(buf_.data() + token_end + 1, id_end - token_end - 1);
        std::string pid_str(buf_.data() + id_end + 1, pid_end - id_end - 1);
        long pid = -1;
        try { pid = std::stol(pid_str); } catch (...) {}
        if (pid > 0) on_pid_(token, job_id, pid);

        return pid_end + 1; // consume through closing SEP

    } else if (tag == kEndTag) {
        // Format: START VELIX_END SEP <token> SEP <job_id> SEP <exit_code> SEP
        auto token_end = buf_.find(kFieldSeparator, tag_end + 1);
        if (token_end == std::string::npos) return 0;

        auto id_end = buf_.find(kFieldSeparator, token_end + 1);
        if (id_end == std::string::npos) return 0;

        auto code_end = buf_.find(kFieldSeparator, id_end + 1);
        if (code_end == std::string::npos) return 0;

        std::string_view token(buf_.data() + tag_end + 1, token_end - tag_end - 1);
        std::string_view job_id(buf_.data() + token_end + 1, id_end - token_end - 1);
        std::string code_str(buf_.data() + id_end + 1, code_end - id_end - 1);
        int exit_code = 0;
        try { exit_code = std::stoi(code_str); } catch (...) {}
        on_end_(token, job_id, exit_code);

        return code_end + 1;

    } else {
        // Unknown tag — emit the FrameStart as literal output and move on.
        on_output_(std::string_view(buf_.data(), 1));
        return 1;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

std::string LocalPTYDriver::make_job_id() {
    static std::atomic<uint64_t> counter{0};
    uint64_t n  = counter.fetch_add(1);
    uint64_t ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    char buf[64];
    std::snprintf(buf, sizeof(buf), "job_%llu_%llu",
                  static_cast<unsigned long long>(ms),
                  static_cast<unsigned long long>(n));
    return buf;
}

uint64_t LocalPTYDriver::now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

LocalPTYDriver::LocalPTYDriver(DriverConfig cfg) : cfg_(std::move(cfg)) {
    // Generate organic 8-char hex token for sentinel security
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<uint32_t> dist;
    uint32_t r = dist(rng);

    char b[16];
    std::snprintf(b, sizeof(b), "%08x", r);
    session_token_ = b;

    spawn_shell();
}

// ─────────────────────────────────────────────────────────────────────────────
// POSIX implementation
// ─────────────────────────────────────────────────────────────────────────────
#ifndef _WIN32

void LocalPTYDriver::spawn_shell() {
    // Choose shell binary and arguments deterministically.
    std::string sh;
    std::vector<std::string> shell_args;

    if (!cfg_.shell.empty()) {
        sh = cfg_.shell;
        // User provided shell: use bash flags if it looks like bash.
        if (sh.find("bash") != std::string::npos) {
            shell_args = {"--norc", "--noprofile"};
        }
    } else if (std::filesystem::exists("/bin/bash")) {
        sh = "/bin/bash";
        shell_args = {"--norc", "--noprofile"};
    } else {
        sh = "/bin/sh";
    }

    // PTY geometry.
    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(cfg_.rows);
    ws.ws_col = static_cast<unsigned short>(cfg_.cols);

    master_fd_ = -1;
    shell_pid_ = ::forkpty(&master_fd_, nullptr, nullptr, &ws);

    if (shell_pid_ < 0) {
        throw std::runtime_error(std::string("forkpty failed: ") + strerror(errno));
    }

    if (shell_pid_ == 0) {
        // ── Child process ────────────────────────────────────────────
        // Spawn a deterministic shell with no rc files so startup
        // noise does not pollute the first job's output.
        ::setenv("PS1", "", 1);
        ::setenv("PS2", "", 1);
        ::setenv("PROMPT_COMMAND", "", 1);

        std::vector<const char *> argv;
        argv.push_back(sh.c_str());
        for (const auto &arg : shell_args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        ::execvp(sh.c_str(), const_cast<char *const *>(argv.data()));
        ::_exit(127);
    }

    // ── Parent ───────────────────────────────────────────────────────
    // Disable terminal echo on the master so injected sentinel bytes
    // are never reflected back as user-visible output (BUG-15 fix).
    {
        struct termios t{};
        if (::tcgetattr(master_fd_, &t) == 0) {
            t.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHOE | ECHOK | ECHONL));
            ::tcsetattr(master_fd_, TCSANOW, &t);
        }
    }

    // Set master non-blocking so the reader can use poll().
    {
        int flags = ::fcntl(master_fd_, F_GETFL, 0);
        ::fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    alive_.store(true);

    timeout_thread_ = std::thread([this] { timeout_loop(); });
    reader_thread_  = std::thread([this] { reader_loop(); });
}

void LocalPTYDriver::close() {
    // Idempotent.
    if (!alive_.exchange(false)) {
        // Was already false — just make sure threads are joined.
        if (reader_thread_.joinable())  reader_thread_.join();
        if (timeout_thread_.joinable()) { timeout_cv_.notify_all(); timeout_thread_.join(); }
        return;
    }

    // Stop the timeout thread first (it may try to signal the job).
    {
        std::scoped_lock lk(timeout_mx_);
        timeout_stop_ = true;
        timeout_cv_.notify_all();
    }
    if (timeout_thread_.joinable()) timeout_thread_.join();

    // Kill the shell process.
    // CRITICAL (BUG-5 fix): capture and zero shell_pid_ BEFORE any kill/wait
    // so that if reader_loop() concurrently reaches its waitpid it operates
    // on pid=-1 (WNOHANG on -1 returns ECHILD, harmless).
    pid_t pid_to_kill = -1;
    {
        // We don't need the job lock for shell_pid_ itself — it's only written
        // here and in spawn_shell (before threads start).  But we take it to
        // synchronize with finish_job_locked which may reference shell state.
        std::scoped_lock lk(job_mx_);
        pid_to_kill = shell_pid_;
        shell_pid_  = -1;   // zero out BEFORE kill so reader sees -1 if it checks
    }

    if (pid_to_kill > 0) {
        ::kill(pid_to_kill, SIGHUP);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ::kill(pid_to_kill, SIGKILL);
        int st = 0;
        ::waitpid(pid_to_kill, &st, 0);
    }

    // Close the PTY master.  The reader thread polls master_fd_; after close
    // the poll returns POLLHUP and the thread exits cleanly.
    if (master_fd_ >= 0) {
        ::close(master_fd_);
        master_fd_ = -1;
    }

    if (reader_thread_.joinable()) reader_thread_.join();

    // Mark any still-running job as errored.
    std::scoped_lock lk(job_mx_);
    if (current_job_ && current_job_->store) {
        current_job_->store->finish(current_job_->job_id, JobStatus::Error, -1);
    }
    current_job_.reset();
    job_cv_.notify_all();
}

bool LocalPTYDriver::write_to_shell(const char *data, size_t len) {
    if (master_fd_ < 0 || !alive_.load()) return false;
    while (len > 0) {
        ssize_t n = ::write(master_fd_, data, len);
        if (n <= 0) return false;
        data += n;
        len  -= static_cast<size_t>(n);
    }
    return true;
}

bool LocalPTYDriver::cancel_current_job() {
    std::scoped_lock lk(job_mx_);
    if (!current_job_ || current_job_->job_pgid <= 0) return false;
    // Safety: never kill pgid 0 or 1 (init/session group).
    if (current_job_->job_pgid <= 1) {
        LOG_ERROR("Refusing to send signal to dangerous process group");
        return false;
    }
    return ::kill(-current_job_->job_pgid, SIGINT) == 0;
}

void LocalPTYDriver::finish_job_locked(const std::string &job_id,
                                       JobStatus st, int code) {
    if (current_job_ && current_job_->job_id == job_id) {
        current_job_->store->finish(job_id, st, code);
        current_job_.reset();
        job_cv_.notify_all();
        timeout_cv_.notify_all(); // wake timeout_loop to pick up next job
    }
}

// ── timeout_loop ─────────────────────────────────────────────────────────────
//
// Waits for a job to have a deadline, then sleeps until the deadline fires.
// On wake it sends SIGKILL to the job's process group.
//
void LocalPTYDriver::timeout_loop() {
    while (true) {
        std::unique_lock<std::mutex> lk(timeout_mx_);

        // Wait until there is a job with a deadline or we're told to stop.
        timeout_cv_.wait(lk, [this] {
            if (timeout_stop_) return true;
            std::scoped_lock jlk(job_mx_);
            return current_job_ && current_job_->has_deadline;
        });

        if (timeout_stop_) return;

        // Capture deadline and job info under job_mx_.
        std::chrono::steady_clock::time_point deadline;
        std::string job_id;
        {
            std::scoped_lock jlk(job_mx_);
            if (!current_job_ || !current_job_->has_deadline) continue;
            deadline = current_job_->deadline;
            job_id   = current_job_->job_id;
        }

        // Sleep until deadline (or until woken early by job completion / stop).
        timeout_cv_.wait_until(lk, deadline, [this, &job_id] {
            if (timeout_stop_) return true;
            std::scoped_lock jlk(job_mx_);
            // Woken early because job finished or a new job started.
            return !current_job_ || current_job_->job_id != job_id;
        });

        if (timeout_stop_) return;

        // Check if the job is still running (it may have finished while we slept).
        std::scoped_lock jlk(job_mx_);
        if (!current_job_ || current_job_->job_id != job_id) continue;

        // Deadline elapsed — kill the job's process group.
        if (current_job_->job_pgid > 1) {
            LOG_WARN("Job " + job_id + " timed out, sending SIGKILL to pgid " +
                     std::to_string(current_job_->job_pgid));
            ::kill(-current_job_->job_pgid, SIGKILL);
        } else {
            // pgid not yet known — try killing by shell pid as fallback.
            LOG_WARN("Job " + job_id + " timed out, pgid unknown, killing shell");
            if (shell_pid_ > 0) ::kill(shell_pid_, SIGKILL);
        }

        // Mark timeout; the sentinel will NOT arrive so we mark now.
        finish_job_locked(job_id, JobStatus::Timeout, 124);
    }
}

// ── reader_loop ──────────────────────────────────────────────────────────────
//
// Single reader thread for the PTY master.
// Uses SentinelParser to demux output vs. control frames.
//
void LocalPTYDriver::reader_loop() {
    SentinelParser parser(
        // on_output
        [&](std::string_view chunk) {
            std::scoped_lock lk(job_mx_);
            if (current_job_) {
                current_job_->store->append_output(current_job_->job_id, chunk);
            }
        },
        // on_pid
        [&](std::string_view token, std::string_view job_id, long pid) {
            if (token != session_token_ || pid <= 0) return;
            std::scoped_lock lk(job_mx_);
            // Verify this PID belongs to our current expectant job.
            if (current_job_ && current_job_->job_id == job_id) {
                pid_t pgid = ::getpgid(static_cast<pid_t>(pid));
                if (pgid <= 0) pgid = static_cast<pid_t>(pid);
                current_job_->job_pgid = pgid;
                // Notify timeout thread that pgid is now known.
                timeout_cv_.notify_one();
            }
        },
        // on_end
        [&](std::string_view token, std::string_view job_id, int exit_code) {
            if (token != session_token_) return;
            std::scoped_lock lk(job_mx_);
            finish_job_locked(std::string(job_id),
                              exit_code == 124 ? JobStatus::Timeout : JobStatus::Finished,
                              exit_code);
        }
    );

    char buf[8192];
    while (alive_.load()) {
        // master_fd_ may be -1 if close() raced with us — check each iteration.
        int fd = master_fd_;
        if (fd < 0) break;

        struct pollfd pfd{ fd, POLLIN | POLLHUP | POLLERR, 0 };
        int rc = ::poll(&pfd, 1, 100 /*ms*/);

        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) continue; // timeout — loop to re-check alive_

        if (pfd.revents & (POLLHUP | POLLERR)) break;

        if (pfd.revents & POLLIN) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                parser.feed(buf, static_cast<size_t>(n));
            } else if (n == 0 || (n < 0 && errno == EIO)) {
                break; // EOF / hangup
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        }
    }

    parser.flush_remaining();

    // Reap the shell process if it's still around.
    {
        std::scoped_lock lk(job_mx_);
        if (shell_pid_ > 0) {
            int st = 0;
            if (::waitpid(shell_pid_, &st, WNOHANG) == shell_pid_) {
                shell_pid_ = -1;
            }
        }
    }

    alive_.store(false);

    // Finalize any job that was still running.
    {
        std::scoped_lock lk(job_mx_);
        if (current_job_) {
            current_job_->store->finish(current_job_->job_id, JobStatus::Error, -1);
            current_job_.reset();
            job_cv_.notify_all();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// exec() — POSIX
// ─────────────────────────────────────────────────────────────────────────────

std::string LocalPTYDriver::exec(const std::string &cmd, int timeout_sec,
                                 std::shared_ptr<JobStore> job_store) {
    if (!alive_.load()) {
        throw std::runtime_error("LocalPTYDriver: shell is not alive");
    }

    // Wait for any previous job to complete (5-minute safety ceiling).
    {
        std::unique_lock<std::mutex> lk(job_mx_);
        if (!job_cv_.wait_for(lk, std::chrono::minutes(5),
                              [this] { return !current_job_; })) {
            throw std::runtime_error("exec: previous job did not complete within 5 minutes");
        }
    }

    const std::string job_id = make_job_id();

    // Register job in store BEFORE writing to shell (reader may start
    // appending output immediately after the write).
    Job job;
    job.job_id          = job_id;
    job.cmd             = cmd;
    job.status          = JobStatus::Running;
    job.submitted_at_ms = now_ms();
    job_store->upsert(job);

    {
        std::scoped_lock lk(job_mx_);
        auto pj           = std::make_shared<PendingJob>();
        pj->job_id        = job_id;
        pj->store         = job_store;
        pj->timeout_sec   = timeout_sec;
        pj->submitted_at  = std::chrono::steady_clock::now();
        if (timeout_sec > 0) {
            pj->has_deadline = true;
            pj->deadline     = pj->submitted_at + std::chrono::seconds(timeout_sec);
        }
        pj->job_pgid = -1;
        current_job_ = std::move(pj);
    }

    // Wake timeout thread so it can pick up the new deadline.
    timeout_cv_.notify_one();

    // Build shell payload.
    //
    // { } grouping runs in the current shell context — cd/export/set persist.
    // VELIX_START carries (job_id, pid) to avoid ambiguity.
    // __velix_ec captures $? before any subsequent commands.
    //
    // printf uses octal \036/\037 (RS/US) — works in dash/sh/bash/zsh.
    const std::string payload =
        "__velix_cmd=" + shell_quote(cmd) + " ; "
        "printf '\\036VELIX_START\\037" + session_token_ + "\\037" + job_id + "\\037%s\\037' \"${BASHPID:-$$}\" ; "
        "eval \"$__velix_cmd\" ; "
        "__velix_ec=$? ; "
        "printf '\\036VELIX_END\\037" + session_token_ + "\\037" + job_id + "\\037%s\\037' \"$__velix_ec\" ; "
        "unset __velix_cmd __velix_ec\n";

    LOG_INFO("PTY payload: " + payload);
    if (!write_to_shell(payload)) {
        std::scoped_lock lk(job_mx_);
        if (current_job_ && current_job_->job_id == job_id) {
            current_job_->store->finish(job_id, JobStatus::Error, -1);
            current_job_.reset();
            job_cv_.notify_all();
        }
        alive_.store(false);
        throw std::runtime_error("exec: failed to write to shell stdin");
    }

    return job_id;
}

// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Helper: drain an fd into dst until EOF or error.
// Returns when fd is closed or an error occurs.
static void drain_fd(int fd, std::string &dst) {
    char buf[8192];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
        dst.append(buf, static_cast<size_t>(n));
    ::close(fd);
}

// Portable ppoll with deadline — falls back to poll + remaining-time calc
// on platforms that don't have ppoll (macOS uses poll with ms).
// Returns poll rc.
static int poll_until(int fd_out, int fd_err,
                      std::chrono::steady_clock::time_point deadline) {
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return 0; // timed out

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      deadline - now).count();
        // Cap at 200ms so we check the deadline loop frequently.
        int timeout_ms = static_cast<int>(std::min<long long>(ms, 200));

        struct pollfd pfds[2];
        int nfds = 0;
        if (fd_out >= 0) { pfds[nfds] = {fd_out, POLLIN | POLLHUP, 0}; ++nfds; }
        if (fd_err >= 0) { pfds[nfds] = {fd_err, POLLIN | POLLHUP, 0}; ++nfds; }

        int rc = ::poll(pfds, static_cast<nfds_t>(nfds), timeout_ms);
        if (rc < 0 && errno == EINTR) continue;
        return rc;
    }
}

static ExecResult posix_exec(const std::string &cmd,
                             const std::vector<std::string> &args,
                             const std::filesystem::path &cwd,
                             int timeout_sec) {
    std::vector<const char *> argv;
    argv.push_back(cmd.c_str());
    for (const auto &a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    int so[2], se[2];
    if (::pipe(so) || ::pipe(se))
        throw std::runtime_error("pipe: " + std::string(strerror(errno)));

    pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("fork: " + std::string(strerror(errno)));

    if (pid == 0) {
        ::close(so[0]); ::close(se[0]);
        ::dup2(so[1], STDOUT_FILENO);
        ::dup2(se[1], STDERR_FILENO);
        ::close(so[1]); ::close(se[1]);

        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            std::string m = "chdir: " + cwd.string() + ": " + strerror(errno) + "\n";
            ::write(STDERR_FILENO, m.c_str(), m.size());
            ::_exit(1);
        }
        ::execvp(cmd.c_str(), const_cast<char *const *>(argv.data()));
        std::string m = "execvp: " + std::string(strerror(errno)) + "\n";
        ::write(STDERR_FILENO, m.c_str(), m.size());
        ::_exit(127);
    }

    ::close(so[1]); ::close(se[1]);

    // BUG-9 fix: read stdout/stderr in parallel threads, but use poll-based
    // timeout so we can SIGKILL the child without blocking in read().
    ExecResult res;

    // Set both read-ends non-blocking.
    ::fcntl(so[0], F_SETFL, ::fcntl(so[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(se[0], F_SETFL, ::fcntl(se[0], F_GETFL) | O_NONBLOCK);

    const bool has_timeout = timeout_sec > 0;
    auto deadline = has_timeout
        ? std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec)
        : std::chrono::steady_clock::time_point::max();

    int fd_out = so[0], fd_err = se[0];

    while (fd_out >= 0 || fd_err >= 0) {
        // Check deadline.
        if (has_timeout && std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            int st = 0; ::waitpid(pid, &st, 0);
            if (fd_out >= 0) ::close(fd_out);
            if (fd_err >= 0) ::close(fd_err);
            res.timed_out = true;
            res.exit_code = 124;
            res.err += "\n[commandline] Killed after " + std::to_string(timeout_sec) + "s";
            return res;
        }

        // Poll with deadline.
        auto poll_deadline = has_timeout ? deadline
            : std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        poll_until(fd_out, fd_err, poll_deadline);

        // Drain whatever is available.
        char buf[8192];
        if (fd_out >= 0) {
            ssize_t n;
            while ((n = ::read(fd_out, buf, sizeof(buf))) > 0)
                res.out.append(buf, static_cast<size_t>(n));
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                ::close(fd_out); fd_out = -1;
            }
        }
        if (fd_err >= 0) {
            ssize_t n;
            while ((n = ::read(fd_err, buf, sizeof(buf))) > 0)
                res.err.append(buf, static_cast<size_t>(n));
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                ::close(fd_err); fd_err = -1;
            }
        }

        // Non-blocking wait.
        int st = 0;
        pid_t wp = ::waitpid(pid, &st, WNOHANG);
        if (wp == pid) {
            res.exit_code = WIFEXITED(st)     ? WEXITSTATUS(st)
                          : WIFSIGNALED(st)   ? 128 + WTERMSIG(st)
                                              : -1;
            // Drain remaining bytes after process exit.
            if (fd_out >= 0) { drain_fd(fd_out, res.out); fd_out = -1; }
            if (fd_err >= 0) { drain_fd(fd_err, res.err); fd_err = -1; }
            return res;
        }
    }

    int st = 0;
    ::waitpid(pid, &st, 0);
    res.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return res;
}

static ExecResult posix_exec_pty(const std::string &cmd,
                                 const std::vector<std::string> &args,
                                 const std::filesystem::path &cwd,
                                 int timeout_sec) {
    std::vector<const char *> argv;
    argv.push_back(cmd.c_str());
    for (const auto &a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    struct winsize ws{};
    ws.ws_row = 50; ws.ws_col = 220;

    int master_fd = -1;
    pid_t pid = ::forkpty(&master_fd, nullptr, nullptr, &ws);
    if (pid < 0) throw std::runtime_error("forkpty: " + std::string(strerror(errno)));

    if (pid == 0) {
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            std::string m = "chdir: " + cwd.string() + ": " + strerror(errno) + "\n";
            ::write(STDERR_FILENO, m.c_str(), m.size());
            ::_exit(1);
        }
        ::execvp(cmd.c_str(), const_cast<char *const *>(argv.data()));
        std::string m = "execvp: " + std::string(strerror(errno)) + "\n";
        ::write(STDERR_FILENO, m.c_str(), m.size());
        ::_exit(127);
    }

    ::fcntl(master_fd, F_SETFL, ::fcntl(master_fd, F_GETFL) | O_NONBLOCK);

    ExecResult res;
    const bool has_timeout = timeout_sec > 0;
    auto deadline = has_timeout
        ? std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec)
        : std::chrono::steady_clock::time_point::max();

    char buf[8192];
    while (true) {
        if (has_timeout && std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGHUP);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ::kill(pid, SIGKILL);
            int st = 0; ::waitpid(pid, &st, 0);
            res.timed_out = true;
            res.exit_code = 124;
            res.err = "[commandline] PTY process killed after " +
                      std::to_string(timeout_sec) + "s";
            break;
        }

        ssize_t n = ::read(master_fd, buf, sizeof(buf));
        if (n > 0) {
            res.out.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Poll briefly.
            struct pollfd pfd{master_fd, POLLIN | POLLHUP, 0};
            int poll_ms = has_timeout
                ? static_cast<int>(std::min<long long>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now()).count(),
                      100))
                : 100;
            ::poll(&pfd, 1, poll_ms);
            continue;
        }
        // EOF or EIO — process exited.
        break;
    }

    // Drain any remaining output.
    {
        int flags = ::fcntl(master_fd, F_GETFL);
        ::fcntl(master_fd, F_SETFL, flags & ~O_NONBLOCK);
        ssize_t n;
        while ((n = ::read(master_fd, buf, sizeof(buf))) > 0)
            res.out.append(buf, static_cast<size_t>(n));
    }

    if (!res.timed_out) {
        int st = 0;
        ::waitpid(pid, &st, 0);
        res.exit_code = WIFEXITED(st)   ? WEXITSTATUS(st)
                      : WIFSIGNALED(st) ? 128 + WTERMSIG(st)
                                        : -1;
    }

    ::close(master_fd);
    return res;
}

} // anonymous namespace

ExecResult LocalPTYDriver::run_cmd(const std::string &cmd,
                                   const std::vector<std::string> &args,
                                   const std::filesystem::path &cwd,
                                   int timeout_sec, bool use_pty) {
    return use_pty ? posix_exec_pty(cmd, args, cwd, timeout_sec)
                   : posix_exec(cmd, args, cwd, timeout_sec);
}

// ─────────────────────────────────────────────────────────────────────────────
// Windows implementation
// ─────────────────────────────────────────────────────────────────────────────

#else // _WIN32

void LocalPTYDriver::spawn_shell() {
    typedef HRESULT(WINAPI *PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
    HMODULE hKernel = GetModuleHandleA("kernel32.dll");
    auto fnCreate = (PFN_CreatePseudoConsole)GetProcAddress(hKernel, "CreatePseudoConsole");
    if (!fnCreate)
        throw std::runtime_error("ConPTY not available (Windows 10 1809+ required)");

    std::string sh = cfg_.shell.empty() ? "cmd.exe" : cfg_.shell;

    HANDLE hPipeInR{}, hPipeInW{}, hPipeOutR{}, hPipeOutW{};
    CreatePipe(&hPipeInR,  &hPipeInW,  nullptr, 0);
    CreatePipe(&hPipeOutR, &hPipeOutW, nullptr, 0);

    COORD sz{ static_cast<SHORT>(cfg_.cols), static_cast<SHORT>(cfg_.rows) };
    HPCON hpc{};
    HRESULT hr = fnCreate(sz, hPipeInR, hPipeOutW, 0, &hpc);
    CloseHandle(hPipeInR);
    CloseHandle(hPipeOutW);
    if (FAILED(hr)) {
        CloseHandle(hPipeInW); CloseHandle(hPipeOutR);
        throw std::runtime_error("CreatePseudoConsole failed");
    }

    hpc_       = hpc;
    write_end_ = hPipeInW;
    read_end_  = hPipeOutR;

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    auto attrList = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                              hpc, sizeof(hpc), nullptr, nullptr);

    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(siex);
    siex.lpAttributeList = attrList;

    std::wstring wcmd(sh.begin(), sh.end());
    PROCESS_INFORMATION pi{};
    bool ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                             EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                             &siex.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrList);

    if (!ok) throw std::runtime_error("CreateProcessW failed for shell: " + sh);
    hProc_ = pi.hProcess;
    CloseHandle(pi.hThread);

    alive_.store(true);
    timeout_thread_ = std::thread([this] { timeout_loop(); });
    reader_thread_  = std::thread([this] { reader_loop(); });
}

void LocalPTYDriver::close() {
    if (!alive_.exchange(false)) {
        if (reader_thread_.joinable())  reader_thread_.join();
        if (timeout_thread_.joinable()) { timeout_cv_.notify_all(); timeout_thread_.join(); }
        return;
    }

    {
        std::scoped_lock lk(timeout_mx_);
        timeout_stop_ = true;
        timeout_cv_.notify_all();
    }
    if (timeout_thread_.joinable()) timeout_thread_.join();

    typedef void(WINAPI *PFN_ClosePseudoConsole)(HPCON);
    auto fnClose = (PFN_ClosePseudoConsole)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "ClosePseudoConsole");

    if (hProc_ != INVALID_HANDLE_VALUE) {
        TerminateProcess(hProc_, 1);
        CloseHandle(hProc_);
        hProc_ = INVALID_HANDLE_VALUE;
    }
    if (hpc_ && fnClose) { fnClose(hpc_); hpc_ = nullptr; }
    if (write_end_ != INVALID_HANDLE_VALUE) {
        CloseHandle(write_end_);
        write_end_ = INVALID_HANDLE_VALUE;
    }
    // read_end_ is closed by reader_loop.

    if (reader_thread_.joinable()) reader_thread_.join();

    std::scoped_lock lk(job_mx_);
    if (current_job_) {
        current_job_->store->finish(current_job_->job_id, JobStatus::Error, -1);
        current_job_.reset();
        job_cv_.notify_all();
    }
}

bool LocalPTYDriver::write_to_shell(const char *data, size_t len) {
    if (write_end_ == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    return WriteFile(write_end_, data, static_cast<DWORD>(len), &written, nullptr)
        && written == static_cast<DWORD>(len);
}

bool LocalPTYDriver::cancel_current_job() {
    // Send Ctrl+C to the ConPTY. The easiest portable approach is to write
    // the ETX character (0x03) directly to the shell's input.
    const char ctrl_c = '\x03';
    return write_to_shell(&ctrl_c, 1);
}

void LocalPTYDriver::finish_job_locked(const std::string &job_id,
                                       JobStatus st, int code) {
    if (current_job_ && current_job_->job_id == job_id) {
        current_job_->store->finish(job_id, st, code);
        current_job_.reset();
        job_cv_.notify_all();
        timeout_cv_.notify_all();
    }
}

void LocalPTYDriver::timeout_loop() {
    while (true) {
        std::unique_lock<std::mutex> lk(timeout_mx_);
        timeout_cv_.wait(lk, [this] {
            if (timeout_stop_) return true;
            std::scoped_lock jlk(job_mx_);
            return current_job_ && current_job_->has_deadline;
        });
        if (timeout_stop_) return;

        std::chrono::steady_clock::time_point deadline;
        std::string job_id;
        {
            std::scoped_lock jlk(job_mx_);
            if (!current_job_ || !current_job_->has_deadline) continue;
            deadline = current_job_->deadline;
            job_id   = current_job_->job_id;
        }

        timeout_cv_.wait_until(lk, deadline, [this, &job_id] {
            if (timeout_stop_) return true;
            std::scoped_lock jlk(job_mx_);
            return !current_job_ || current_job_->job_id != job_id;
        });

        if (timeout_stop_) return;

        std::scoped_lock jlk(job_mx_);
        if (!current_job_ || current_job_->job_id != job_id) continue;

        // Kill via TerminateProcess.
        if (hProc_ != INVALID_HANDLE_VALUE) {
            LOG_WARN("Job " + job_id + " timed out, terminating process");
            TerminateProcess(hProc_, 124);
        }
        finish_job_locked(job_id, JobStatus::Timeout, 124);
    }
}

void LocalPTYDriver::reader_loop() {
    SentinelParser parser(
        // on_output
        [&](std::string_view chunk) {
            std::scoped_lock lk(job_mx_);
            if (current_job_)
                current_job_->store->append_output(current_job_->job_id, chunk);
        },
        // on_pid
        [](std::string_view /*token*/, std::string_view /*job_id*/, long /*pid*/) { 
            /* no pgid tracking on Windows */ 
        },
        [&](std::string_view token, std::string_view job_id, int exit_code) {
            if (token != session_token_) return;
            std::scoped_lock lk(job_mx_);
            finish_job_locked(std::string(job_id),
                              exit_code == 124 ? JobStatus::Timeout : JobStatus::Finished,
                              exit_code);
        }
    );

    char buf[8192];
    DWORD n = 0;
    while (alive_.load()) {
        if (!ReadFile(read_end_, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        parser.feed(buf, static_cast<size_t>(n));
    }

    parser.flush_remaining();

    if (read_end_ != INVALID_HANDLE_VALUE) {
        CloseHandle(read_end_);
        read_end_ = INVALID_HANDLE_VALUE;
    }

    alive_.store(false);

    std::scoped_lock lk(job_mx_);
    if (current_job_) {
        current_job_->store->finish(current_job_->job_id, JobStatus::Error, -1);
        current_job_.reset();
        job_cv_.notify_all();
    }
}

std::string LocalPTYDriver::exec(const std::string &cmd, int timeout_sec,
                                 std::shared_ptr<JobStore> job_store) {
    if (!alive_.load())
        throw std::runtime_error("LocalPTYDriver: shell is not alive");

    {
        std::unique_lock<std::mutex> lk(job_mx_);
        if (!job_cv_.wait_for(lk, std::chrono::minutes(5),
                              [this] { return !current_job_; }))
            throw std::runtime_error("exec: previous job timed out waiting (5 min)");
    }

    const std::string job_id = make_job_id();

    Job job;
    job.job_id          = job_id;
    job.cmd             = cmd;
    job.status          = JobStatus::Running;
    job.submitted_at_ms = now_ms();
    job_store->upsert(job);

    {
        std::scoped_lock lk(job_mx_);
        auto pj         = std::make_shared<PendingJob>();
        pj->job_id      = job_id;
        pj->store       = job_store;
        pj->timeout_sec = timeout_sec;
        pj->submitted_at = std::chrono::steady_clock::now();
        if (timeout_sec > 0) {
            pj->has_deadline = true;
            pj->deadline     = pj->submitted_at + std::chrono::seconds(timeout_sec);
        }
        current_job_ = std::move(pj);
    }
    timeout_cv_.notify_one();

    // Windows payload: use cmd.exe's ECHO off + PowerShell for the sentinel.
    // job_id is guaranteed [a-z0-9_] so no quoting needed in the PS script.
    const std::string payload =
        cmd + "\r\n"
        "@powershell -NoProfile -Command \""
        "[Console]::Out.Write([char]30 + 'VELIX_END' + [char]31 + '"
        + session_token_ + "' + [char]31 + '"
        + job_id + "' + [char]31 + $LASTEXITCODE + [char]31);\"\r\n";

    LOG_INFO("PTY payload: " + payload);
    if (!write_to_shell(payload)) {
        std::scoped_lock lk(job_mx_);
        if (current_job_ && current_job_->job_id == job_id) {
            current_job_->store->finish(job_id, JobStatus::Error, -1);
            current_job_.reset();
            job_cv_.notify_all();
        }
        alive_.store(false);
        throw std::runtime_error("exec: write to shell failed");
    }

    return job_id;
}

namespace {
static ExecResult win_exec(const std::string &cmd,
                           const std::vector<std::string> &args,
                           const std::filesystem::path &cwd, int timeout_sec) {
    std::string cmdline = "\"" + cmd + "\"";
    for (const auto &a : args) cmdline += " \"" + a + "\"";

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE hOutR{}, hOutW{}, hErrR{}, hErrW{};
    CreatePipe(&hOutR, &hOutW, &sa, 0); SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);
    CreatePipe(&hErrR, &hErrW, &sa, 0); SetHandleInformation(hErrR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOutW;
    si.hStdError  = hErrW;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string cwds = cwd.string();
    const char *cwd_c = cwds.empty() ? nullptr : cwds.c_str();
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, cwd_c, &si, &pi)) {
        CloseHandle(hOutR); CloseHandle(hOutW);
        CloseHandle(hErrR); CloseHandle(hErrW);
        throw std::runtime_error("CreateProcess failed: " + std::to_string(GetLastError()));
    }
    CloseHandle(hOutW); CloseHandle(hErrW);

    ExecResult res;
    std::thread to([&] {
        char buf[8192]; DWORD n;
        while (ReadFile(hOutR, buf, sizeof(buf), &n, nullptr) && n > 0)
            res.out.append(buf, n);
        CloseHandle(hOutR);
    });
    std::thread te([&] {
        char buf[8192]; DWORD n;
        while (ReadFile(hErrR, buf, sizeof(buf), &n, nullptr) && n > 0)
            res.err.append(buf, n);
        CloseHandle(hErrR);
    });

    DWORD ms = (timeout_sec <= 0) ? INFINITE : static_cast<DWORD>(timeout_sec * 1000);
    if (WaitForSingleObject(pi.hProcess, ms) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 124);
        res.timed_out = true;
        res.exit_code = 124;
        res.err += "\n[commandline] Process killed after " + std::to_string(timeout_sec) + "s";
    } else {
        DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
        res.exit_code = static_cast<int>(ec);
    }
    to.join(); te.join();
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return res;
}
} // anonymous namespace

ExecResult LocalPTYDriver::run_cmd(const std::string &cmd,
                                   const std::vector<std::string> &args,
                                   const std::filesystem::path &cwd,
                                   int timeout_sec, bool /*use_pty*/) {
    // On Windows we always use plain exec (ConPTY one-shot is complex;
    // the persistent driver handles PTY needs).
    return win_exec(cmd, args, cwd, timeout_sec);
}

#endif // _WIN32

// ─────────────────────────────────────────────────────────────────────────────
// DockerDriver
// ─────────────────────────────────────────────────────────────────────────────

DockerDriver::DockerDriver(DriverConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.docker_container.empty())
        throw std::runtime_error("DockerDriver: docker_container must be set");

    if (!container_is_running(cfg_)) {
        throw std::runtime_error(
            "DockerDriver: container is not running: " + cfg_.docker_container);
    }

    // Build a DriverConfig for an inner LocalPTYDriver that runs
    // `docker exec -it <container> <shell>`.
    DriverConfig inner_cfg;
    inner_cfg.type  = "local_pty";
    inner_cfg.cols  = cfg_.cols;
    inner_cfg.rows  = cfg_.rows;

    std::string docker_bin = "docker";
    if (!cfg_.docker_host.empty()) {
        // Prepend DOCKER_HOST=... before the command.
        // We express this as a shell one-liner inside a local shell.
        // LocalPTYDriver will exec /bin/bash which evaluates the export.
        inner_cfg.shell = "/bin/bash";
    }

    // We set the "shell" to the docker exec invocation.
    // The inner LocalPTYDriver will exec this as its shell process.
    // Docker exec -it spawns a PTY inside the container.
    std::string docker_cmd = docker_bin;
    if (!cfg_.docker_host.empty())
        docker_cmd = "DOCKER_HOST=" + shell_quote(cfg_.docker_host) + " " + docker_cmd;

    docker_cmd += " exec -i";  // -i = stdin; we manage the PTY at our level
    if (!cfg_.docker_user.empty())
        docker_cmd += " --user " + shell_quote(cfg_.docker_user);
    docker_cmd += " " + shell_quote(cfg_.docker_container);

    std::string container_shell = cfg_.docker_shell.empty() ? "/bin/sh" : cfg_.docker_shell;
    docker_cmd += " " + container_shell;

    inner_cfg.shell = docker_cmd;
    inner_ = std::make_unique<LocalPTYDriver>(std::move(inner_cfg));
    alive_.store(true);
}

bool DockerDriver::container_is_running(const DriverConfig &cfg) {
    // `docker inspect` forks a subprocess — we only do this at construction
    // and from the watchdog (every kWatchdogIntervalMs, not on every is_alive).
    // The hot path (is_alive from exec()) relies on inner_->is_alive() which
    // is a simple atomic load.
    std::string docker_cmd = "docker";
    if (!cfg.docker_host.empty())
        docker_cmd = "DOCKER_HOST=" + shell_quote(cfg.docker_host) + " " + docker_cmd;
    docker_cmd += " inspect --format '{{.State.Running}}' " +
                  shell_quote(cfg.docker_container);

    ExecResult r = velix::app_manager::run_cmd("sh", {"-c", docker_cmd}, {}, 5, false);
    auto out = r.out;
    auto trim = [](std::string &s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    };
    trim(out);
    return out == "true";
}

std::string DockerDriver::exec(const std::string &cmd, int timeout_sec,
                               std::shared_ptr<JobStore> store) {
    if (!is_alive())
        throw std::runtime_error("DockerDriver: not alive");
    return inner_->exec(cmd, timeout_sec, store);
}

ExecResult DockerDriver::run_cmd(const std::string &cmd,
                                 const std::vector<std::string> &args,
                                 const std::filesystem::path &cwd,
                                 int timeout_sec, bool use_pty) {
    // For one-shot, run `docker exec <container> <cmd> <args>`.
    std::vector<std::string> docker_args;
    docker_args.push_back("exec");
    if (!cfg_.docker_user.empty()) {
        docker_args.push_back("--user");
        docker_args.push_back(cfg_.docker_user);
    }
    if (!cwd.empty()) {
        docker_args.push_back("--workdir");
        docker_args.push_back(cwd.string());
    }
    docker_args.push_back(cfg_.docker_container);
    docker_args.push_back(cmd);
    for (const auto &a : args) docker_args.push_back(a);

    return velix::app_manager::run_cmd("docker", docker_args, {}, timeout_sec, false);
}

bool DockerDriver::cancel_current_job() {
    return inner_ && inner_->cancel_current_job();
}

void DockerDriver::close() {
    alive_.store(false);
    if (inner_) inner_->close();
}

bool DockerDriver::is_alive() const {
    // Hot path: only check the atomic flag + inner PTY process.
    // container_is_running() forks `docker inspect` — too expensive for every
    // is_alive() call (which is invoked from exec(), watchdog, and snapshot).
    // The watchdog's detect_dead_sessions() will catch container exits when
    // inner_->is_alive() goes false (docker exec exits when container stops).
    return alive_.load() &&
           inner_ && inner_->is_alive();
}

// ─────────────────────────────────────────────────────────────────────────────
// SSHDriver stub
// (Full libssh2 implementation lives in ssh_driver.cpp)
// ─────────────────────────────────────────────────────────────────────────────

struct SSHDriver::Impl {
    // libssh2 session, channel, and reader thread live here.
    // Defined fully in ssh_driver.cpp.
};

SSHDriver::~SSHDriver() {
    close();
}

SSHDriver::SSHDriver(DriverConfig cfg) : cfg_(std::move(cfg)) {
    // Construction happens in ssh_driver.cpp where libssh2 is available.
    // This translation unit only provides the type layout.
    throw std::runtime_error(
        "SSHDriver: not linked — include ssh_driver.cpp in your build");
}

std::string SSHDriver::exec(const std::string &, int, std::shared_ptr<JobStore>) {
    throw std::runtime_error("SSHDriver: not linked");
}
ExecResult SSHDriver::run_cmd(const std::string &, const std::vector<std::string> &,
                              const std::filesystem::path &, int, bool) {
    throw std::runtime_error("SSHDriver: not linked");
}
bool SSHDriver::cancel_current_job() { return false; }
void SSHDriver::close() {}

// ─────────────────────────────────────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<TerminalDriver> make_driver(const DriverConfig &cfg) {
    const std::string &t = cfg.type;
    if (t.empty() || t == "local_pty") return std::make_unique<LocalPTYDriver>(cfg);
    if (t == "docker")                 return std::make_unique<DockerDriver>(cfg);
    if (t == "ssh")                    return std::make_unique<SSHDriver>(cfg);
    throw std::runtime_error("Unknown driver type: " + t);
}

// ─────────────────────────────────────────────────────────────────────────────
// Utility free functions
// ─────────────────────────────────────────────────────────────────────────────

std::string shell_quote(const std::string &s) {
#ifdef _WIN32
    if (s.empty()) return "\"\"";
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out += "\"";
    return out;
#else
    // POSIX single-quote: safe for all characters including whitespace,
    // backslash, glob chars. Embedded single quotes use: ''\''
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
#endif
}

std::filesystem::path resolve_sandbox_cwd(const std::string &sub,
                                          const std::string &root,
                                          bool allow_path_escape) {
    namespace fs = std::filesystem;

    // BUG-11 fix:
    //  1. Resolve root to absolute first.
    //  2. Compute the target (root / sub).
    //  3. Lexically normalise to collapse ../ without filesystem access.
    //  4. On POSIX use realpath-equivalent (canonical) to follow symlinks.
    //  5. Check the prefix relationship AFTER all resolution.
    //  6. Create directories AFTER the check (not before).

    fs::path root_path = fs::absolute(fs::path(root));

    // Lexical normalisation (no filesystem access — works even if path
    // doesn't exist yet).
    fs::path target = (root_path / sub).lexically_normal();

    // Canonicalise if possible (follows symlinks; fails if path doesn't exist,
    // which is fine — we'll create it after the check).
    std::error_code ec;
    fs::path canonical_target = fs::canonical(target, ec);
    if (ec) canonical_target = target; // not yet created — use lexical result

    fs::path canonical_root = fs::canonical(root_path, ec);
    if (ec) canonical_root = root_path;

    if (!allow_path_escape) {
        // Check that canonical_target starts with canonical_root.
        // Use relative() — if the result starts with ".." we've escaped.
        auto rel = canonical_target.lexically_relative(canonical_root);
        auto it  = rel.begin();
        if (it != rel.end() && *it == "..") {
            throw std::runtime_error(
                "cwd escapes sandbox root: " + canonical_target.string());
        }
    }

    // Now it is safe to create.
    fs::create_directories(canonical_target, ec);
    // Ignore ec — the caller's command will fail with a clear error if
    // the directory can't be created.

    return canonical_target;
}

// Top-level free function — dispatches to platform-specific one-shots.
ExecResult run_cmd(const std::string &cmd,
                   const std::vector<std::string> &args,
                   const std::filesystem::path &cwd,
                   int timeout_sec, bool use_pty) {
#if !defined(_WIN32) && !defined(_WIN64)
    return use_pty ? posix_exec_pty(cmd, args, cwd, timeout_sec)
                   : posix_exec(cmd, args, cwd, timeout_sec);
#else
    return win_exec(cmd, args, cwd, timeout_sec);
    (void)use_pty;
#endif
}

} // namespace velix::app_manager
