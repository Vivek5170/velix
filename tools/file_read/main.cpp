/**
 * file_read — Velix Tool
 *
 * Read file content with large-file safety and line-offset pagination.
 * Uses agent_tools::read_file() from utils/file_tools.
 */

#include "../../runtime/sdk/cpp/velix_process.hpp"
#include "../../utils/file_tools.hpp"

#include <cstdlib>

namespace {

size_t max_file_read_bytes_from_env() {
    if (const char *value = std::getenv("VELIX_MAX_FILE_READ_BYTES"); value && *value) {
        try {
            const auto parsed = static_cast<size_t>(std::stoull(value));
            if (parsed > 0) return parsed;
        } catch (...) {
        }
    }
    return 32768;
}

} // namespace

using namespace velix::core;

class FileReadTool : public VelixProcess {
public:
    FileReadTool() : VelixProcess("file_read", "tool") {}

    void run() override {
        const std::string path = params.value("path", "");
        if (path.empty()) {
            return done_error("Parameter 'path' is required.");
        }

        // Build read options from params
        agent_tools::ReadOptions opts;
        opts.offset_line = params.value("offset_line", 1);
        opts.max_lines   = params.value("max_lines", 150);
        const size_t max_bytes_cap = max_file_read_bytes_from_env();
        opts.max_bytes   = params.value("max_bytes", max_bytes_cap);
        opts.resolve_symlinks = params.value("resolve_symlinks", false);

        // Clamp to sane ranges
        if (opts.offset_line < 1) opts.offset_line = 1;
        if (opts.max_lines < 0) opts.max_lines = 0;
        if (opts.max_bytes <= 0 || opts.max_bytes > max_bytes_cap) {
            opts.max_bytes = max_bytes_cap;
        }

        json result = agent_tools::read_file(path, opts);
        report_result(parent_pid, result, entry_trace_id);
    }

private:
    void done_error(const std::string& msg) {
        report_result(parent_pid,
                      {{"status", "error"}, {"error", msg}},
                      entry_trace_id);
    }
};

int main() {
    FileReadTool tool;
    try {
        tool.start();
    } catch (const std::exception&) {
        return 1;
    }
    return 0;
}
