/**
 * file_tools.cpp — Implementation of agent Read / Write / Patch utilities
 * ========================================================================
 * See file_tools.hpp for the full API contract.
 */

#include "file_tools.hpp"
#include "file_lock.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdlib>

// ── Platform-specific open-for-locking ──────────────────────────────────────
#if defined(_WIN32)
#   include <windows.h>
#   include <io.h>
#   include <fcntl.h>
#   include <sys/stat.h>
#else
#   include <sys/file.h>
#   include <sys/stat.h>
#   include <fcntl.h>
#   include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace agent_tools {

// ═══════════════════════════════════════════════════════════════════════════
// §1  Path Normalisation
// ═══════════════════════════════════════════════════════════════════════════

fs::path normalise_path(const std::string& raw, bool strict_symlinks) {
    if (raw.empty()) return {};

    std::string s = raw;

    // ── Trim surrounding whitespace / quotes the LLM sometimes adds ──────
    while (!s.empty() && (s.front() == ' ' || s.front() == '\'' || s.front() == '"'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\'' || s.back() == '"'))
        s.pop_back();

#if defined(_WIN32)
    // Normalise forward-slash → backslash on Windows
    std::replace(s.begin(), s.end(), '/', '\\');
#else
    // Normalise backslash → forward-slash on POSIX
    std::replace(s.begin(), s.end(), '\\', '/');
#endif

    // ── Home directory expansion  (~  or  ~/foo) ─────────────────────────
    if (!s.empty() && s[0] == '~') {
        const char* home = nullptr;
#if defined(_WIN32)
        home = std::getenv("USERPROFILE");
        if (!home) home = std::getenv("HOMEPATH");
#else
        home = std::getenv("HOME");
#endif
        if (home) {
            s = std::string(home) + s.substr(1);
        }
    }

#if !defined(_WIN32)
    // ── Environment variable expansion  ($VAR  or  ${VAR}) ─────────────
    {
        std::string expanded;
        expanded.reserve(s.size() * 2);
        for (size_t i = 0; i < s.size(); ) {
            if (s[i] == '$') {
                size_t start = i + 1;
                bool braced = (start < s.size() && s[start] == '{');
                if (braced) ++start;
                size_t end = start;
                while (end < s.size() && (std::isalnum(s[end]) || s[end] == '_'))
                    ++end;
                std::string var_name = s.substr(start, end - start);
                const char* val = std::getenv(var_name.c_str());
                if (val) expanded += val;
                else     expanded += s.substr(i, (end - i) + (braced ? 1 : 0));
                i = end + (braced && end < s.size() && s[end] == '}' ? 1 : 0);
            } else {
                expanded += s[i++];
            }
        }
        s = std::move(expanded);
    }
#else
    // ── Windows %VAR% expansion ──────────────────────────────────────────
    {
        std::string expanded;
        for (size_t i = 0; i < s.size(); ) {
            if (s[i] == '%') {
                size_t end = s.find('%', i + 1);
                if (end != std::string::npos) {
                    std::string var_name = s.substr(i + 1, end - i - 1);
                    const char* val = std::getenv(var_name.c_str());
                    if (val) expanded += val;
                    else     expanded += s.substr(i, end - i + 1);
                    i = end + 1;
                } else {
                    expanded += s[i++];
                }
            } else {
                expanded += s[i++];
            }
        }
        s = std::move(expanded);
    }
#endif

    fs::path p(s);

    // ── Make absolute (relative to cwd) ──────────────────────────────────
    if (p.is_relative()) {
        std::error_code ec;
        auto abs = fs::absolute(p, ec);
        if (!ec) p = abs;
    }

    // ── Lexical normalisation (./ ../ double-slashes) ────────────────────
    p = p.lexically_normal();

    // ── Optional strict mode: resolve symlinks to canonical path ──────────
    if (strict_symlinks) {
        std::error_code ec;
        p = fs::canonical(p, ec);
        // If canonical fails, keep the normalized path anyway
        // (canonical will fail if path doesn't exist)
    }

    return p;
}


// ═══════════════════════════════════════════════════════════════════════════
// §2  Fuzzy Path Matching
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Simple edit-distance (Levenshtein) on filename strings — O(n*m)
int edit_distance(const std::string& a, const std::string& b) {
    size_t m = a.size(), n = b.size();
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (size_t i = 0; i <= m; ++i) dp[i][0] = (int)i;
    for (size_t j = 0; j <= n; ++j) dp[0][j] = (int)j;
    for (size_t i = 1; i <= m; ++i)
        for (size_t j = 1; j <= n; ++j) {
            int cost = (std::tolower(a[i-1]) == std::tolower(b[j-1])) ? 0 : 1;
            dp[i][j] = std::min({dp[i-1][j]+1, dp[i][j-1]+1, dp[i-1][j-1]+cost});
        }
    return dp[m][n];
}

double filename_score(const std::string& candidate, const std::string& target) {
    if (target.empty()) return 0.0;
    int d = edit_distance(candidate, target);
    double max_len = (double)std::max(candidate.size(), target.size());
    return 1.0 - d / max_len;
}

} // anonymous namespace

std::vector<FuzzyMatch> find_closest_paths(const fs::path& target, int max_results) {
    std::vector<FuzzyMatch> results;

    std::string target_name = target.filename().string();
    fs::path    search_dir  = target.parent_path();

    // ── Strategy 1: scan the target's parent directory ───────────────────
    auto try_dir = [&](const fs::path& dir) {
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            double s = filename_score(entry.path().filename().string(), target_name);
            if (s > 0.4) results.push_back({entry.path(), s});
        }
    };

    if (!search_dir.empty()) try_dir(search_dir);

    // ── Strategy 2: walk up one level if the directory itself is wrong ───
    if (results.size() < 3 && search_dir.has_parent_path()) {
        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(
                               search_dir.parent_path(), ec)) {
            if (ec) { ec.clear(); continue; }
            if (!entry.is_regular_file()) continue;
            double s = filename_score(entry.path().filename().string(), target_name);
            if (s > 0.55) results.push_back({entry.path(), s});
        }
    }

    // ── Deduplicate, sort, truncate ───────────────────────────────────────
    std::sort(results.begin(), results.end(),
              [](const FuzzyMatch& a, const FuzzyMatch& b){ return a.score > b.score; });
    results.erase(std::unique(results.begin(), results.end(),
                              [](const FuzzyMatch& a, const FuzzyMatch& b){
                                  return a.path == b.path; }),
                  results.end());
    if ((int)results.size() > max_results) results.resize(max_results);
    return results;
}


// ═══════════════════════════════════════════════════════════════════════════
// §3  Internal Helpers
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Produce the "suggestion / candidates" portion of an error JSON when
// a path was not found.
json not_found_error(const std::string& msg, const fs::path& target) {
    auto matches = find_closest_paths(target);
    json candidates = json::array();
    for (auto& m : matches)
        candidates.push_back(m.path.string());
    json result = {
        {"status", "error"},
        {"error",  msg}
    };
    if (!candidates.empty()) {
        result["suggestion"]  = matches.front().path.string();
        result["candidates"]  = candidates;
    }
    return result;
}

// Detect if data is likely binary (null-byte heuristic).
// Samples first 8 KiB; if >2% null bytes, flags as binary.
bool is_likely_binary(const std::string& data) {
    if (data.empty()) return false;
    size_t sample_size = std::min(data.size(), (size_t)8192);
    int null_count = 0;
    for (size_t i = 0; i < sample_size; ++i) {
        if (data[i] == '\0') ++null_count;
    }
    // If >2% of sample is null bytes, likely binary
    return (null_count * 100) > (int)(sample_size * 2);
}

// Validate UTF-8 encoding. Returns number of invalid bytes found.
// Returns 0 if valid UTF-8.
int validate_utf8(const std::string& data) {
    int errors = 0;
    for (size_t i = 0; i < data.size(); ) {
        unsigned char byte = (unsigned char)data[i];
        
        if ((byte & 0x80) == 0) {
            // Single-byte ASCII (0xxxxxxx)
            ++i;
        } else if ((byte & 0xE0) == 0xC0) {
            // Two-byte sequence (110xxxxx 10xxxxxx)
            if (i + 1 >= data.size() || (data[i+1] & 0xC0) != 0x80) {
                ++errors;
                ++i;
                continue;
            }
            i += 2;
        } else if ((byte & 0xF0) == 0xE0) {
            // Three-byte sequence (1110xxxx 10xxxxxx 10xxxxxx)
            if (i + 2 >= data.size() || 
                (data[i+1] & 0xC0) != 0x80 ||
                (data[i+2] & 0xC0) != 0x80) {
                ++errors;
                ++i;
                continue;
            }
            i += 3;
        } else if ((byte & 0xF8) == 0xF0) {
            // Four-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
            if (i + 3 >= data.size() ||
                (data[i+1] & 0xC0) != 0x80 ||
                (data[i+2] & 0xC0) != 0x80 ||
                (data[i+3] & 0xC0) != 0x80) {
                ++errors;
                ++i;
                continue;
            }
            i += 4;
        } else {
            // Invalid UTF-8 start byte
            ++errors;
            ++i;
        }
    }
    return errors;
}

// Decode UTF-8 with replacement for invalid sequences.
// Replaces invalid bytes with U+FFFD (replacement character).
std::string decode_utf8_with_replacement(const std::string& data) {
    std::string result;
    result.reserve(data.size());
    
    for (size_t i = 0; i < data.size(); ) {
        unsigned char byte = (unsigned char)data[i];
        
        if ((byte & 0x80) == 0) {
            // Single-byte ASCII
            result += byte;
            ++i;
        } else if ((byte & 0xE0) == 0xC0 && i + 1 < data.size() && 
                   (data[i+1] & 0xC0) == 0x80) {
            // Valid two-byte sequence
            result += byte;
            result += data[i+1];
            i += 2;
        } else if ((byte & 0xF0) == 0xE0 && i + 2 < data.size() &&
                   (data[i+1] & 0xC0) == 0x80 && (data[i+2] & 0xC0) == 0x80) {
            // Valid three-byte sequence
            result += byte;
            result += data[i+1];
            result += data[i+2];
            i += 3;
        } else if ((byte & 0xF8) == 0xF0 && i + 3 < data.size() &&
                   (data[i+1] & 0xC0) == 0x80 && (data[i+2] & 0xC0) == 0x80 &&
                   (data[i+3] & 0xC0) == 0x80) {
            // Valid four-byte sequence
            result += byte;
            result += data[i+1];
            result += data[i+2];
            result += data[i+3];
            i += 4;
        } else {
            // Invalid byte sequence → replace with U+FFFD (UTF-8: EF BF BD)
            result += '\xEF';
            result += '\xBF';
            result += '\xBD';
            ++i;
        }
    }
    return result;
}

// Extract context around a position (N lines before/after)
struct PatchContext {
    std::string before;  // Up to 150 chars before position
    std::string match;   // The matched text
    std::string after;   // Up to 150 chars after match
};

PatchContext extract_patch_context(const std::string& content, 
                                    size_t pos, 
                                    size_t match_len) {
    PatchContext ctx;
    
    // Context before: up to 150 chars, but stop at newline if found
    size_t before_start = (pos > 150) ? pos - 150 : 0;
    ctx.before = content.substr(before_start, pos - before_start);
    // Trim leading partial lines
    size_t first_newline = ctx.before.rfind('\n');
    if (first_newline != std::string::npos && first_newline > 0) {
        ctx.before = ctx.before.substr(first_newline + 1);
    }
    
    // The match itself
    ctx.match = content.substr(pos, match_len);
    
    // Context after: up to 150 chars
    size_t after_end = std::min(pos + match_len + 150, content.size());
    ctx.after = content.substr(pos + match_len, after_end - pos - match_len);
    // Trim trailing partial lines
    size_t last_newline = ctx.after.find('\n');
    if (last_newline != std::string::npos) {
        ctx.after = ctx.after.substr(0, last_newline);
    }
    
    return ctx;
}

// Write string atomically: write to .tmp then rename.
// Returns empty string on success, error message on failure.
std::string atomic_write(const fs::path& dest, const std::string& content) {
    fs::path tmp = dest;
    tmp += ".tmp";

    // Ensure parent directory exists
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) return "Cannot create directories: " + ec.message();

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return "Cannot open temp file for writing: " + tmp.string();
        f.write(content.data(), (std::streamsize)content.size());
        if (!f) return "Write failed on temp file: " + tmp.string();
    } // file closed here — data is flushed

    // Atomic rename
#if defined(_WIN32)
    // MoveFileExW with REPLACE is atomic on NTFS
    BOOL ok = MoveFileExW(tmp.wstring().c_str(),
                           dest.wstring().c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        DWORD err = GetLastError();
        char buf[256];
        snprintf(buf, sizeof(buf), "Atomic rename failed (Win32 error %lu)", err);
        fs::remove(tmp, ec);
        return buf;
    }
#else
    // POSIX rename(2) is atomic within the same filesystem
    if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
        std::string err = "Atomic rename failed: ";
        err += std::strerror(errno);
        fs::remove(tmp, ec);
        return err;
    }
#endif
    return {};
}

// Read entire file, but abort if it exceeds max_bytes.
// Returns: true on success, false on failure. Sets `too_large` if file
// exceeds the limit (content will contain partial data up to max_bytes).
bool read_bounded(const fs::path& p, std::string& out,
                  size_t max_bytes, bool& too_large) {
    too_large = false;

    std::error_code ec;
    auto file_size = fs::file_size(p, ec);
    if (ec) {
        // Fallback: try to open and read
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return f.good() || f.eof();
    }

    if (max_bytes > 0 && file_size > max_bytes) {
        too_large = true;
        // Read only max_bytes
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        out.resize(max_bytes);
        f.read(out.data(), (std::streamsize)max_bytes);
        auto got = f.gcount();
        out.resize((size_t)got);
        return true;
    }

    // Read whole file
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.resize((size_t)file_size);
    f.read(out.data(), (std::streamsize)file_size);
    auto got = f.gcount();
    out.resize((size_t)got);
    return true;
}

// Read entire file into string (unbounded), returns false on failure.
bool read_all(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return f.good() || f.eof();
}

} // anonymous namespace


// ═══════════════════════════════════════════════════════════════════════════
// §4  Public API — agent_tools::read_file
// ═══════════════════════════════════════════════════════════════════════════

json read_file(const std::string& raw_path, const ReadOptions& opts) {
    if (raw_path.empty())
        return {{"status", "error"}, {"error", "path is empty"}};

    fs::path p = normalise_path(raw_path, opts.resolve_symlinks);

    std::error_code ec;
    if (!fs::exists(p, ec) || ec)
        return not_found_error("File not found: " + p.string(), p);

    if (!fs::is_regular_file(p, ec) || ec)
        return {{"status", "error"},
                {"error", "Path exists but is not a regular file: " + p.string()}};

    // ── Get file size for metadata ───────────────────────────────────────
    auto total_bytes = fs::file_size(p, ec);
    if (ec) total_bytes = 0;

    // ── Safety gate: reject obviously binary / huge files ────────────────
    // max_bytes=0 means unlimited (agent explicitly opted out of safety)
    const size_t effective_max = opts.max_bytes;

    // ── Read the file with bounds ────────────────────────────────────────
    std::string raw_content;
    bool too_large = false;

    if (effective_max > 0) {
        if (!read_bounded(p, raw_content, effective_max, too_large))
            return {{"status", "error"},
                    {"error", "Failed to read file (permissions?): " + p.string()}};
    } else {
        if (!read_all(p, raw_content))
            return {{"status", "error"},
                    {"error", "Failed to read file (permissions?): " + p.string()}};
    }

    // ── Detect binary content (null-byte heuristic) ────────────────────────
    bool is_binary = is_likely_binary(raw_content);
    if (is_binary) {
        return {
            {"status",      "error"},
            {"error",       "File appears to be binary (contains null bytes). Cannot read as text."},
            {"path",        p.string()},
            {"total_bytes", (long long)total_bytes},
            {"is_binary",   true}
        };
    }

    // ── Handle mid-line truncation (if max_bytes boundary cuts line) ────────
    bool truncated_mid_line = false;
    if (too_large && raw_content.size() > 0) {
        // Check if we're in the middle of a line
        if (raw_content.back() != '\n') {
            truncated_mid_line = true;
            // Find last complete line and truncate there
            size_t last_newline = raw_content.rfind('\n');
            if (last_newline != std::string::npos) {
                raw_content.resize(last_newline + 1);
            }
        }
    }

    // ── UTF-8 validation (validate but always decode with replacement) ─────
    int utf8_errors = validate_utf8(raw_content);
    if (utf8_errors > 0) {
        // Decode with replacement chars for invalid sequences
        raw_content = decode_utf8_with_replacement(raw_content);
    }

    // ── Split into lines and apply offset + limit ────────────────────────
    // We split lines from the raw content, then select the window.
    std::vector<std::string> all_lines;
    {
        std::istringstream stream(raw_content);
        std::string line;
        while (std::getline(stream, line))
            all_lines.push_back(std::move(line));
    }

    int total_lines = (int)all_lines.size();

    // Clamp offset_line to valid range (1-based)
    int start = std::max(1, opts.offset_line);
    if (start > total_lines) {
        return json{
            {"status",      "error"},
            {"error",       "offset_line (" + std::to_string(start) +
                            ") exceeds total lines (" +
                            std::to_string(total_lines) + ")"},
            {"total_lines", total_lines},
            {"total_bytes", (long long)total_bytes}
        };
    }

    int end_line;
    if (opts.max_lines > 0) {
        end_line = std::min(start + opts.max_lines - 1, total_lines);
    } else {
        end_line = total_lines;
    }

    // Build the output content from the selected line range
    std::string content;
    {
        size_t approx = 0;
        for (int i = start - 1; i < end_line; ++i)
            approx += all_lines[i].size() + 1;
        content.reserve(approx);
        for (int i = start - 1; i < end_line; ++i) {
            content += all_lines[i];
            content += '\n';
        }
    }

    int lines_read = end_line - start + 1;
    bool truncated = too_large || (end_line < total_lines);

    json result = {
        {"status",      "ok"},
        {"path",        p.string()},
        {"content",     content},
        {"total_lines", total_lines},
        {"lines_read",  lines_read},
        {"start_line",  start},
        {"end_line",    end_line},
        {"truncated",   truncated},
        {"total_bytes", (long long)total_bytes}
    };

    // If truncated mid-line (max_bytes cut in middle), add flag
    if (truncated_mid_line) {
        result["truncated_mid_line"] = true;
    }

    // If UTF-8 errors were found and corrected, add warning flag
    if (utf8_errors > 0) {
        result["invalid_utf8"] = true;
        result["utf8_errors"] = utf8_errors;
    }

    // If truncated, tell the LLM how to read the next chunk
    if (truncated && end_line < total_lines) {
        result["next_offset_line"] = end_line + 1;
        result["remaining_lines"]  = total_lines - end_line;
    }

    return result;
}


// ═══════════════════════════════════════════════════════════════════════════
// §5  Public API — agent_tools::write_file
// ═══════════════════════════════════════════════════════════════════════════

json write_file(const std::string& raw_path, const std::string& content) {
    if (raw_path.empty())
        return {{"status", "error"}, {"error", "path is empty"}};

    fs::path p = normalise_path(raw_path);

    std::string err = atomic_write(p, content);
    if (!err.empty())
        return {{"status", "error"}, {"error", err}};

    return {
        {"status",        "ok"},
        {"path",          p.string()},
        {"bytes_written", (long long)content.size()}
    };
}


// ═══════════════════════════════════════════════════════════════════════════
// §6  Public API — agent_tools::append_file
// ═══════════════════════════════════════════════════════════════════════════

json append_file(const std::string& raw_path,
                 const std::string& content,
                 int lock_timeout_ms)
{
    if (raw_path.empty())
        return {{"status", "error"}, {"error", "path is empty"}};

    fs::path p = normalise_path(raw_path);

    // ── Enforce max append size ───────────────────────────────────────────
    if (content.size() > DEFAULT_MAX_APPEND_SIZE) {
        return {{"status", "error"},
                {"error", "Append size (" + std::to_string(content.size()) +
                         " bytes) exceeds limit (" +
                         std::to_string(DEFAULT_MAX_APPEND_SIZE) + " bytes). " +
                         "Maximum append size is 10 MiB."},
                {"requested_size", (long long)content.size()},
                {"max_allowed",    (long long)DEFAULT_MAX_APPEND_SIZE}};
    }

    // ── Ensure parent directory exists ────────────────────────────────────
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec)
        return {{"status", "error"},
                {"error", "Cannot create directories: " + ec.message()}};

    // ── Open (or create) the file for appending ───────────────────────────
#if defined(_WIN32)
    HANDLE h = CreateFileW(
        p.wstring().c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,          // create if not exists, open if exists
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {{"status", "error"},
                {"error", "Cannot open file for appending: " + p.string()}};
    FileLock lk(h);
#else
    int fd = ::open(p.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return {{"status", "error"},
                {"error", std::string("Cannot open file for appending: ") + std::strerror(errno)}};
    FileLock lk(fd);
#endif

    // ── Acquire exclusive lock before touching the file ───────────────────
    if (!lk.acquire(lock_timeout_ms)) {
#if defined(_WIN32)
        CloseHandle(h);
#else
        ::close(fd);
#endif
        return {{"status", "error"},
                {"error", "Could not acquire file lock within " +
                          std::to_string(lock_timeout_ms) +
                          "ms — another agent may be writing this file."},
                {"retryable", true},
                {"retry_after_ms", lock_timeout_ms / 2}};
    }

    // ── Seek to end and write ─────────────────────────────────────────────
#if defined(_WIN32)
    LARGE_INTEGER offset{};
    SetFilePointerEx(h, offset, nullptr, FILE_END);
    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data(),
                        static_cast<DWORD>(content.size()), &written, nullptr);
    long long file_size_after = 0;
    {
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        file_size_after = sz.QuadPart;
    }
    lk.release();
    CloseHandle(h);
    if (!ok)
        return {{"status", "error"},
                {"error", "Write failed during append on: " + p.string()}};
    long long bytes_appended = static_cast<long long>(written);
#else
    // O_APPEND-style: lseek to end then write (lock guarantees atomicity)
    if (::lseek(fd, 0, SEEK_END) < 0) {
        lk.release();
        ::close(fd);
        return {{"status", "error"},
                {"error", std::string("lseek failed: ") + std::strerror(errno)}};
    }
    ssize_t written = ::write(fd, content.data(), content.size());
    long long file_size_after = static_cast<long long>(::lseek(fd, 0, SEEK_END));
    lk.release();
    ::close(fd);
    if (written < 0)
        return {{"status", "error"},
                {"error", std::string("Write failed during append: ") + std::strerror(errno)}};
    long long bytes_appended = static_cast<long long>(written);
#endif

    return {
        {"status",          "ok"},
        {"path",            p.string()},
        {"bytes_appended",  bytes_appended},
        {"file_size_after", file_size_after}
    };
}


// ═══════════════════════════════════════════════════════════════════════════
// §7  Public API — agent_tools::patch_file
// ═══════════════════════════════════════════════════════════════════════════

json patch_file(const std::string& raw_path,
                const std::string& old_text,
                const std::string& new_text,
                int lock_timeout_ms)
{
    if (raw_path.empty())
        return {{"status", "error"}, {"error", "path is empty"}};
    if (old_text.empty())
        return {{"status", "error"}, {"error", "old_text is empty — nothing to replace"}};

    fs::path p = normalise_path(raw_path);

    std::error_code ec;
    if (!fs::exists(p, ec) || ec)
        return not_found_error("File not found: " + p.string(), p);

    if (!fs::is_regular_file(p, ec) || ec)
        return {{"status", "error"},
                {"error", "Path is not a regular file: " + p.string()}};

    // ── Open file for locking ─────────────────────────────────────────────
#if defined(_WIN32)
    HANDLE h = CreateFileW(
        p.wstring().c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {{"status", "error"},
                {"error", "Cannot open file for patching (access denied?): " + p.string()}};
    }
    FileLock lk(h);
#else
    int fd = ::open(p.c_str(), O_RDWR);
    if (fd < 0) {
        return {{"status", "error"},
                {"error", std::string("Cannot open file for patching: ") + std::strerror(errno)}};
    }
    FileLock lk(fd);
#endif

    // ── Acquire advisory exclusive lock ───────────────────────────────────
    if (!lk.acquire(lock_timeout_ms)) {
#if defined(_WIN32)
        CloseHandle(h);
#else
        ::close(fd);
#endif
        return {{"status", "error"},
                {"error", "Could not acquire file lock within " +
                          std::to_string(lock_timeout_ms) +
                          "ms — another agent may be patching this file."},
                {"retryable", true},
                {"retry_after_ms", lock_timeout_ms / 2}};
    }

    // ── Read content ──────────────────────────────────────────────────────
    std::string content;
    if (!read_all(p, content)) {
#if defined(_WIN32)
        CloseHandle(h);
#else
        ::close(fd);
#endif
        return {{"status", "error"},
                {"error", "Failed to read file content for patching: " + p.string()}};
    }

    // ── Find occurrences ──────────────────────────────────────────────────
    size_t pos = content.find(old_text);
    if (pos == std::string::npos) {
#if defined(_WIN32)
        CloseHandle(h);
#else
        ::close(fd);
#endif
        // Give the agent a helpful hint — show the first 200 chars of the file
        std::string hint = content.substr(0, std::min(content.size(), (size_t)200));
        return {
            {"status",       "error"},
            {"error",        "old_text not found in file. "
                             "Ensure the text matches exactly (whitespace, line endings)."},
            {"file_preview", hint},
            {"path",         p.string()}
        };
    }

    // ── Reject ambiguous patches (more than one occurrence) ───────────────
    size_t second = content.find(old_text, pos + 1);
    if (second != std::string::npos) {
#if defined(_WIN32)
        CloseHandle(h);
#else
        ::close(fd);
#endif
        return {{"status", "error"},
                {"error", "old_text appears more than once in the file. "
                          "Add more surrounding context to make the match unique."}};
    }

    // ── Apply patch ───────────────────────────────────────────────────────
    std::string patched;
    patched.reserve(content.size() - old_text.size() + new_text.size());
    patched.append(content, 0, pos);
    patched.append(new_text);
    patched.append(content, pos + old_text.size(), std::string::npos);

    // ── Atomic write (tmp + rename) while lock is still held ─────────────
    std::string write_err = atomic_write(p, patched);

    // ── Release lock ─────────────────────────────────────────────────────
    lk.release();
#if defined(_WIN32)
    CloseHandle(h);
#else
    ::close(fd);
#endif

    if (!write_err.empty())
        return {{"status", "error"},
                {"error", "Patch applied but write failed: " + write_err}};

    // ── Extract context for debugging/audit ────────────────────────────────
    PatchContext ctx = extract_patch_context(content, pos, old_text.size());

    return {
        {"status",              "ok"},
        {"path",                p.string()},
        {"occurrences_replaced", 1},
        {"bytes_before",        (long long)content.size()},
        {"bytes_after",         (long long)patched.size()},
        {"context", {
            {"before",  ctx.before},
            {"match",   ctx.match},
            {"after",   ctx.after}
        }}
    };
}

} // namespace agent_tools
