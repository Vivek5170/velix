/**
 * file_patch — Velix Tool
 *
 * Replace an exact substring in a file, atomically and under an advisory
 * exclusive lock. Rejects ambiguous patches (old_text appears more than once).
 * Uses agent_tools::patch_file() from utils/file_tools.
 */

#include "../../runtime/sdk/cpp/velix_process.hpp"
#include "../../utils/file_tools.hpp"

using namespace velix::core;

class FilePatchTool : public VelixProcess {
public:
    FilePatchTool() : VelixProcess("file_patch", "tool") {}

    void run() override {
        const std::string path = params.value("path", "");
        if (path.empty()) {
            return done_error("Parameter 'path' is required.");
        }

        if (!params.contains("old_text") || !params["old_text"].is_string()) {
            return done_error("Parameter 'old_text' is required.");
        }
        const std::string old_text = params["old_text"].get<std::string>();

        if (!params.contains("new_text") || !params["new_text"].is_string()) {
            return done_error("Parameter 'new_text' is required.");
        }
        const std::string new_text = params["new_text"].get<std::string>();

        int lock_timeout_ms = params.value("lock_timeout_ms", 5000);
        if (lock_timeout_ms < 0) lock_timeout_ms = 0;

        json result = agent_tools::patch_file(path, old_text, new_text, lock_timeout_ms);
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
    FilePatchTool tool;
    try {
        tool.start();
    } catch (const std::exception&) {
        return 1;
    }
    return 0;
}
