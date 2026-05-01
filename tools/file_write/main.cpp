/**
 * file_write — Velix Tool
 *
 * Create or overwrite a file atomically (tmp + rename).
 * Parent directories are created automatically.
 * Uses agent_tools::write_file() from utils/file_tools.
 */

#include "../../runtime/sdk/cpp/velix_process.hpp"
#include "../../utils/file_tools.hpp"

using namespace velix::core;

class FileWriteTool : public VelixProcess {
public:
    FileWriteTool() : VelixProcess("file_write", "tool") {}

    void run() override {
        const std::string path = params.value("path", "");
        if (path.empty()) {
            return done_error("Parameter 'path' is required.");
        }

        if (!params.contains("content")) {
            return done_error("Parameter 'content' is required.");
        }
        const std::string content = params.value("content", "");
        const bool append         = params.value("append", false);
        const int  lock_timeout   = params.value("lock_timeout_ms", 5000);

        json result = append
            ? agent_tools::append_file(path, content, lock_timeout)
            : agent_tools::write_file(path, content);

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
    FileWriteTool tool;
    try {
        tool.start();
    } catch (const std::exception&) {
        return 1;
    }
    return 0;
}
