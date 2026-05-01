/**
 * file_read — Velix Tool
 *
 * Read file content with large-file safety and line-offset pagination.
 * Uses agent_tools::read_file() from utils/file_tools.
 */

#include "../../runtime/sdk/cpp/velix_process.hpp"
#include "../../utils/file_tools.hpp"

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
        opts.max_lines   = params.value("max_lines", 300);
        opts.max_bytes   = params.value("max_bytes", 512 * 1024); // 512 KiB default
        opts.resolve_symlinks = params.value("resolve_symlinks", false);

        // Clamp to sane ranges
        if (opts.offset_line < 1) opts.offset_line = 1;
        if (opts.max_lines < 0) opts.max_lines = 0;

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
