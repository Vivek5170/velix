/**
 * terminal_registry.cpp
 *
 * Key fixes from the original:
 *
 * BUG-17 — driver->close() is now ALWAYS called OUTSIDE the registry lock.
 *           Eviction: extract_locked() removes the session from the map and
 *           returns the driver; caller closes it after releasing the lock.
 *
 * BUG-18 — exec() drops the shared_lock before returning, not while
 *           driver->exec() is running. exec() now takes a shared_lock only
 *           to retrieve the driver pointer, then releases it before calling
 *           driver->exec(). This prevents holding the registry lock across
 *           an I/O operation.
 *
 * BUG-20 — get_or_create takes a full DriverConfig (not just a type string)
 *           so SSH, Docker, and K8s params all flow through cleanly.
 *
 *           The session key includes the driver type so a user can have
 *           concurrent local + docker sessions under different names.
 */

#include "terminal_registry.hpp"
#include "../../utils/logger.hpp"

#include <stdexcept>

namespace velix::app_manager {

// ─────────────────────────────────────────────────────────────────────────────
// get_or_create
// ─────────────────────────────────────────────────────────────────────────────

TerminalRegistry::GetOrCreateResult
TerminalRegistry::get_or_create(const std::string &user_id,
                                const std::string &session_name,
                                const DriverConfig &cfg) {
    if (user_id.empty())
        return {false, false, "", "user_id must not be empty"};

    const std::string norm_name  = normalize_session_name(session_name);
    const std::string session_id = make_session_key(user_id, norm_name);

    // ── Fast path: session exists and is healthy ──────────────────────────
    {
        std::shared_lock rd(sessions_mx_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            const Session &s = it->second;
            if (s.info.status == SessionStatus::Active && s.driver->is_alive()) {
                return {true, true, session_id, ""};
            }
            // Dead/error session — fall through to write path.
        }
    }

    // ── Write path ────────────────────────────────────────────────────────
    // We may need to evict a dead session and/or create a new one.
    // We do NOT call driver->close() under the write lock (BUG-17 fix).

    std::shared_ptr<TerminalDriver> old_driver; // closed after lock is released

    std::unique_lock wr(sessions_mx_);

    // Re-check after upgrading (another thread may have created it).
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        Session &s = it->second;
        if (s.info.status == SessionStatus::Active && s.driver->is_alive()) {
            return {true, true, session_id, ""};
        }
        // Extract the dead driver — close after releasing the write lock.
        old_driver = std::move(s.driver);
        sessions_.erase(it);
        LOG_INFO_CTX("Evicting dead/error session before recreation",
                     "app_manager", user_id, -1, "session_evict_recreate");
    }

    // Enforce limits.
    if (static_cast<int>(sessions_.size()) >= max_sessions_total) {
        wr.unlock();
        if (old_driver) old_driver->close(); // close outside lock
        return {false, false, "",
                "max_sessions_total limit reached (" +
                std::to_string(max_sessions_total) + ")"};
    }

    int user_count = 0;
    for (const auto &[id, s] : sessions_) {
        if (s.info.user_id == user_id && s.info.status == SessionStatus::Active)
            ++user_count;
    }
    if (user_count >= max_sessions_per_user) {
        wr.unlock();
        if (old_driver) old_driver->close();
        return {false, false, "",
                "max_sessions_per_user reached for " + user_id +
                " (limit " + std::to_string(max_sessions_per_user) + ")"};
    }

    // Create driver.
    std::shared_ptr<TerminalDriver> driver;
    try {
        driver = make_driver(cfg);
    } catch (const std::exception &e) {
        wr.unlock();
        if (old_driver) old_driver->close();
        return {false, false, "", std::string("driver spawn failed: ") + e.what()};
    }

    Session session;
    session.cfg           = cfg;
    session.driver        = driver;
    session.info.session_id   = session_id;
    session.info.user_id      = user_id;
    session.info.session_name = norm_name;
    session.info.driver_type  = driver->driver_type();
    session.info.status       = SessionStatus::Active;
    session.info.created_at_ms = now_ms();
    session.info.last_used_ms  = session.info.created_at_ms;

    sessions_.emplace(session_id, std::move(session));

    wr.unlock(); // release lock BEFORE closing the old driver

    if (old_driver) old_driver->close();

    LOG_INFO_CTX("Created terminal session (type=" + cfg.type + ")",
                 "app_manager", user_id, -1, "session_created");

    return {true, false, session_id, ""};
}

// ─────────────────────────────────────────────────────────────────────────────
// exec
// ─────────────────────────────────────────────────────────────────────────────

std::string TerminalRegistry::exec(const std::string &user_id,
                                   const std::string &session_name,
                                   const std::string &cmd,
                                   int timeout_sec) {
    const std::string norm_name  = normalize_session_name(session_name);
    const std::string session_id = make_session_key(user_id, norm_name);

    // BUG-18 fix: take a shared_lock only to retrieve the driver pointer,
    // then release it before calling driver->exec() (which may block on I/O).
    std::shared_ptr<TerminalDriver> driver;
    {
        std::shared_lock rd(sessions_mx_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            const std::string err = "No session: " + session_id;
            throw std::runtime_error(err);
        }
        if (!it->second.driver->is_alive()) {
            const std::string err = "Session shell is dead: " + session_id;
            throw std::runtime_error(err);
        }
        driver = it->second.driver;
        // Update last_used under shared lock — last_used_ms is not protected
        // by its own lock, but uint64_t writes are atomic on all supported
        // platforms and only ever increase, so a stale read is safe for
        // idle-eviction purposes.
        it->second.info.last_used_ms = now_ms();
    }

    return driver->exec(cmd, timeout_sec, job_store_);
}

// ─────────────────────────────────────────────────────────────────────────────
// poll_job
// ─────────────────────────────────────────────────────────────────────────────

std::shared_ptr<Job>
TerminalRegistry::poll_job(const std::string &job_id) const {
    return job_store_->get(job_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// cancel_job
// ─────────────────────────────────────────────────────────────────────────────

bool TerminalRegistry::cancel_job(const std::string &user_id,
                                  const std::string &session_name) {
    const std::string norm_name  = normalize_session_name(session_name);
    const std::string session_id = make_session_key(user_id, norm_name);
    std::shared_ptr<TerminalDriver> driver;
    {
        std::shared_lock rd(sessions_mx_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return false;
        driver = it->second.driver;
    }
    return driver && driver->cancel_current_job();
}

// ─────────────────────────────────────────────────────────────────────────────
// kill_session / kill_session_by_id
// ─────────────────────────────────────────────────────────────────────────────

std::shared_ptr<TerminalDriver>
TerminalRegistry::extract_locked(const std::string &session_id) {
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return nullptr;
    auto driver = std::move(it->second.driver);
    sessions_.erase(it);
    return driver;
}

void TerminalRegistry::kill_session(const std::string &user_id,
                                    const std::string &session_name) {
    std::vector<std::shared_ptr<TerminalDriver>> to_close;

    {
        std::unique_lock wr(sessions_mx_);
        if (session_name.empty()) {
            std::vector<std::string> keys;
            for (const auto &[id, s] : sessions_)
                if (s.info.user_id == user_id) keys.push_back(id);
            for (const auto &id : keys) {
                auto d = extract_locked(id);
                if (d) to_close.push_back(std::move(d));
            }
        } else {
            const std::string norm_name  = normalize_session_name(session_name);
            const std::string session_id = make_session_key(user_id, norm_name);
            auto d = extract_locked(session_id);
            if (d) to_close.push_back(std::move(d));
        }
    } // release lock

    for (auto &d : to_close) {
         LOG_INFO_CTX("Killing session (user=" + user_id + ")",
                      "app_manager", user_id, -1, "session_killed");
         try { d->close(); } 
         catch (...) {
             // Suppress errors from driver close to prevent iteration from crashing;
             // driver cleanup will be handled by shared_ptr destructor.
         }
    }
}

void TerminalRegistry::kill_session_by_id(const std::string &session_id) {
    std::shared_ptr<TerminalDriver> driver;
    {
        std::unique_lock wr(sessions_mx_);
        driver = extract_locked(session_id);
    }
    if (driver) {
        LOG_INFO_CTX("Killing session by id", "app_manager", session_id, -1, "session_killed");
        try { driver->close(); } 
        catch (...) {
            // Suppress errors from driver close; cleanup will be handled by shared_ptr destructor.
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// snapshot
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SessionInfo> TerminalRegistry::snapshot() const {
    std::shared_lock rd(sessions_mx_);
    std::vector<SessionInfo> out;
    out.reserve(sessions_.size());
    for (const auto &[id, s] : sessions_) out.push_back(s.info);
    return out;
}

std::vector<SessionInfo> TerminalRegistry::snapshot(const std::string &user_id) const {
    std::shared_lock rd(sessions_mx_);
    std::vector<SessionInfo> out;
    for (const auto &[id, s] : sessions_)
        if (s.info.user_id == user_id) out.push_back(s.info);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Watchdog helpers
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::string> TerminalRegistry::detect_dead_sessions() {
    std::vector<std::string> dead_ids;

    // Phase 1: read scan (shared lock — cheap).
    {
        std::shared_lock rd(sessions_mx_);
        for (const auto &[id, s] : sessions_) {
            if (s.info.status == SessionStatus::Active && !s.driver->is_alive())
                dead_ids.push_back(id);
        }
    }

    // Phase 2: mark dead (unique lock — only the IDs that changed).
    if (!dead_ids.empty()) {
        std::unique_lock wr(sessions_mx_);
        for (const auto &id : dead_ids) {
            auto it = sessions_.find(id);
            if (it != sessions_.end()) {
                it->second.info.status        = SessionStatus::Dead;
                it->second.info.last_error_msg = "shell process exited unexpectedly";
                LOG_WARN_CTX("Session marked dead", "app_manager", id, -1, "session_dead");
            }
        }
    }

    return dead_ids;
}

int TerminalRegistry::evict_dead_and_idle() {
    const uint64_t now = now_ms();
    std::vector<std::shared_ptr<TerminalDriver>> to_close;

    // Collect IDs and extract drivers under unique lock.
    {
        std::unique_lock wr(sessions_mx_);
        std::vector<std::string> to_evict;
        for (const auto &[id, s] : sessions_) {
            bool is_dead = (s.info.status != SessionStatus::Active);
            bool is_idle = (now - s.info.last_used_ms > idle_timeout_ms);
            if (is_dead || is_idle) to_evict.push_back(id);
        }
        for (const auto &id : to_evict) {
            auto d = extract_locked(id);
            if (d) {
                LOG_INFO_CTX("Watchdog evicting session", "app_manager", id, -1, "watchdog_evict");
                to_close.push_back(std::move(d));
            }
        }
    } // release lock

    // Close drivers outside the lock (BUG-17 fix).
    for (auto &d : to_close) {
        try { d->close(); } 
        catch (...) {
            // Suppress errors from driver close; cleanup will be handled by shared_ptr destructor.
        }
    }

    return static_cast<int>(to_close.size());
}

void TerminalRegistry::prune_jobs() {
    job_store_->prune(job_retention_ms);
}

} // namespace velix::app_manager