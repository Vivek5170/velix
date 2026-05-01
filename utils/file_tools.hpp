/**
 * file_tools.hpp — Agent Read / Write / Patch Utilities
 * =====================================================
 * Production-ready file I/O for LLM agent tools.
 * Cross-platform: Windows · Linux · macOS
 *
 * DESIGN PRINCIPLES
 * -----------------
 * 1. Every function returns nlohmann::json the tool can forward directly.
 * 2. Errors never throw — they are returned as JSON with status="error".
 * 3. Overwrite-writes use atomic rename; append-writes use flock + O_APPEND.
 * 4. Patches use advisory OS-level locking (flock / LockFileEx) so
 *    concurrent agents queue safely without a supervisor process.
 * 5. Fuzzy-path resolution: if the exact path is not found, the tool
 *    returns the closest match so the agent can self-correct.
 * 6. Large-file safety: reads are bounded by max_bytes (default 512 KiB)
 *    and support line-based offset pagination.
 * 7. Binary file detection: null-byte heuristic rejects binary files.
 * 8. Append size limit: appends are bounded by DEFAULT_MAX_APPEND_SIZE (10 MiB).
 * 9. UTF-8 validation: invalid UTF-8 → decode with replacement chars + warning flag.
 * 10. Symlink security: optional canonical path resolution (resolve_symlinks flag).
 *
 * QUICK REFERENCE
 * ---------------
 *   read_file    (path, offset_line, max_lines, max_bytes)  → content or error (detects binary)
 *   write_file   (path, content)                            → success / error  (atomic overwrite)
 *   append_file  (path, content, lock_timeout_ms)           → success / error  (locked append, size-limited)
 *   patch_file   (path, old, new, lock_timeout_ms)          → success / error
 */

#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <optional>

#include "../communication/json_include.hpp"

using json = nlohmann::json;

// ─── Platform detection ──────────────────────────────────────────────────────
#if defined(_WIN32)
#   define AGENT_TOOLS_WINDOWS 1
#elif defined(__APPLE__)
#   define AGENT_TOOLS_MACOS 1
#   define AGENT_TOOLS_POSIX 1
#else
#   define AGENT_TOOLS_LINUX 1
#   define AGENT_TOOLS_POSIX 1
#endif

// ─── Public API ──────────────────────────────────────────────────────────────

namespace agent_tools {

// ── Constants ────────────────────────────────────────────────────────────────
constexpr size_t DEFAULT_MAX_APPEND_SIZE = 10 * 1024 * 1024;  // 10 MiB

// ── Read options ─────────────────────────────────────────────────────────────

struct ReadOptions {
    int    offset_line = 1;         // 1-based line to start reading from
    int    max_lines   = 0;         // 0 = no line limit (bounded by max_bytes)
    size_t max_bytes   = 512*1024;  // 512 KiB safety cap (0 = unlimited)
    bool   resolve_symlinks = false; // If true, resolve symlinks to canonical path (strict mode)
};

/**
 * Read a file with large-file safety, binary detection, UTF-8 validation,
 * and line-offset pagination.
 *
 * Accepts any path format: absolute, relative, ~, $HOME, %USERPROFILE%,
 * Windows, UNC. Optional symlink resolution in strict mode.
 *
 * Validation pipeline:
 *   1. Binary detection (null-byte heuristic) → REJECT if binary
 *   2. UTF-8 validation → if invalid, decode with replacement chars + flag warning
 *   3. Return content with metadata (invalid_utf8=true if needed)
 *
 * Returns JSON:
 *   success:
 *   { "status": "ok", "path": "/abs/path",
 *     "content": "...",
 *     "total_lines": 1500, "lines_read": 300,
 *     "start_line": 1, "end_line": 300,
 *     "truncated": true,
 *     "total_bytes": 82350 }
 *
 *   with warning (invalid UTF-8):
 *   { "status": "ok", ..., "invalid_utf8": true,
 *     "utf8_errors": 3 }
 *
 *   error (binary file):
 *   { "status": "error", "error": "...", "is_binary": true,
 *     "suggestion": "/closest/match" }
 */
json read_file(const std::string& path,
               const ReadOptions& opts = {});

/**
 * Write (or create) a file atomically.
 * Writes to a .tmp sibling, then renames.
 * Intermediate directories are created automatically.
 *
 * Returns JSON:
 *   { "status": "ok", "path": "/abs/path", "bytes_written": 1234 }
 *   { "status": "error", "error": "..." }
 */
json write_file(const std::string& path, const std::string& content);

/**
 * Append content to a file under an advisory exclusive lock.
 * Creates the file (and parent dirs) if it does not exist.
 * Uses flock / LockFileEx — not atomic rename — so concurrent agents
 * queue behind the lock rather than overwriting each other.
 *
 * Max append size enforced: DEFAULT_MAX_APPEND_SIZE (10 MiB).
 * Rejects larger appends with error status.
 *
 * Returns JSON:
 *   { "status": "ok", "path": "/abs/path", "bytes_appended": 42,
 *     "file_size_after": 1234 }
 *   { "status": "error", "error": "..." }
 */
json append_file(const std::string& path,
                 const std::string& content,
                 int lock_timeout_ms = 5000);

/**
 * Patch a file — find `old_text`, replace with `new_text`, atomically.
 *
 * Lock strategy (per-file, advisory):
 *   POSIX  — flock(LOCK_EX)
 *   Windows — LockFileEx(LOCKFILE_EXCLUSIVE_LOCK)
 *
 * Rejects if old_text appears more than once — agent must add context.
 *
 * Returns JSON:
 *   { "status": "ok", "path": "...", "occurrences_replaced": 1 }
 *   { "status": "error", "error": "...", "file_preview": "..." }
 */
json patch_file(const std::string& path,
                const std::string& old_text,
                const std::string& new_text,
                int lock_timeout_ms = 5000);

// ─── Helper types ────────────────────────────────────────────────────────────

struct FuzzyMatch {
    std::filesystem::path path;
    double score;           // 0.0–1.0, higher = closer match
};

/**
 * Find the closest existing paths to the given (possibly non-existent) path.
 * Returns up to `max_results` matches sorted by descending score.
 */
std::vector<FuzzyMatch> find_closest_paths(const std::filesystem::path& target,
                                            int max_results = 5);

/**
 * Normalise a raw path string from an LLM into a canonical absolute path.
 * Handles: ~, $VAR, %VAR%, mixed slashes, trailing slashes, ./ ../ chains.
 * 
 * Optional strict mode: if strict_symlinks=true, resolves symlinks to
 * canonical path (fails if symlink resolution fails).
 */
std::filesystem::path normalise_path(const std::string& raw,
                                      bool strict_symlinks = false);

} // namespace agent_tools
