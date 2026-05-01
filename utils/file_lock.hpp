/**
 * file_lock.hpp — Cross-platform advisory file lock
 * ==================================================
 * POSIX  : flock(2) — Linux & macOS
 * Windows: LockFileEx via Win32
 *
 * Usage:
 *   FileLock lk(fd_or_handle);
 *   if (!lk.acquire(5000)) { ... handle timeout ... }
 *   // ... modify file ...
 *   lk.release(); // also called by destructor
 */

#pragma once
#include <chrono>
#include <thread>

#if defined(_WIN32)
#   include <windows.h>
#   include <io.h>      // _get_osfhandle
#else
#   include <sys/file.h>
#   include <unistd.h>
#   include <fcntl.h>
#   include <errno.h>
#endif

namespace agent_tools {

class FileLock {
public:
#if defined(_WIN32)
    explicit FileLock(HANDLE h) : handle_(h), owned_(false) {}
#else
    explicit FileLock(int fd) : fd_(fd), owned_(false) {}
#endif

    ~FileLock() { if (owned_) release(); }

    // Non-copyable, movable
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&& o) noexcept
#if defined(_WIN32)
        : handle_(o.handle_), owned_(o.owned_) { o.owned_ = false; }
#else
        : fd_(o.fd_), owned_(o.owned_) { o.owned_ = false; }
#endif

    /**
     * Acquire exclusive lock.
     * @param timeout_ms  Maximum milliseconds to wait. 0 = non-blocking.
     * @return true if lock was acquired, false on timeout or error.
     */
    bool acquire(int timeout_ms = 5000) {
#if defined(_WIN32)
        OVERLAPPED ov = {};
        // Try non-blocking first, then poll until timeout
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(timeout_ms);
        while (true) {
            BOOL ok = LockFileEx(handle_,
                                  LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                                  0, MAXDWORD, MAXDWORD, &ov);
            if (ok) { owned_ = true; return true; }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
#else
        // POSIX: try LOCK_NB in a poll loop to respect timeout
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(timeout_ms);
        while (true) {
            int rc = flock(fd_, LOCK_EX | LOCK_NB);
            if (rc == 0) { owned_ = true; return true; }
            if (errno != EWOULDBLOCK && errno != EAGAIN) return false;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
#endif
    }

    void release() {
        if (!owned_) return;
#if defined(_WIN32)
        OVERLAPPED ov = {};
        UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &ov);
#else
        flock(fd_, LOCK_UN);
#endif
        owned_ = false;
    }

    bool is_owned() const { return owned_; }

private:
#if defined(_WIN32)
    HANDLE handle_;
#else
    int fd_;
#endif
    bool owned_;
};

} // namespace agent_tools
