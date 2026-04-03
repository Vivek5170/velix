#include "../../runtime/sdk/cpp/velix_process.hpp"
#include "../../utils/process_spawner.hpp"
#include <algorithm>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <map>
#include <vector>

/**
 * @brief Simple C++ Skill to execute commandline tasks.
 * Inherits from VelixProcess to gain OS connectivity and IPC.
 */
class CommandLineSkill : public velix::core::VelixProcess {
public:
  CommandLineSkill() : velix::core::VelixProcess("commandline", "skill") {}

  /**
   * @brief The core execution logic.
   * Injected parameters (params) are used to determine which command to run.
   */
  void run() override {
    // 1. Extract Validated Parameters
    if (!params.contains("cmd")) {
      report_error("Missing required parameter: cmd");
      return;
    }

    std::string cmd = params["cmd"].get<std::string>();
    std::vector<std::string> args;
    if (params.contains("args") && params["args"].is_array()) {
      for (const auto &arg : params["args"]) {
        if (arg.is_string())
          args.push_back(arg.get<std::string>());
      }
    }

    if (is_blocked_command(cmd)) {
      report_error("Command blocked by policy: " + cmd);
      return;
    }

    const std::filesystem::path repo_root = detect_repo_root();
    const std::filesystem::path playground_root =
        std::filesystem::current_path() / "agent_playground";
    const std::filesystem::path temp_root = playground_root / "tmp";

    std::error_code ec;
    std::filesystem::create_directories(playground_root, ec);
    std::filesystem::create_directories(temp_root, ec);

    const std::filesystem::path start_cwd =
        resolve_start_cwd(repo_root, playground_root, temp_root);

    if (!std::filesystem::exists(start_cwd) ||
        !std::filesystem::is_directory(start_cwd)) {
      report_error("Invalid working directory: " + start_cwd.string());
      return;
    }

    if (is_mutating_command(cmd)) {
      if (!is_path_allowed_for_write(start_cwd, repo_root, playground_root,
                                     temp_root)) {
        report_error("Write operations are not allowed from cwd: " +
                     start_cwd.string());
        return;
      }

      // Mutating commands may write to cwd implicitly.
      std::vector<std::filesystem::path> write_targets;
      write_targets.push_back(start_cwd);

      for (const auto &arg : args) {
        if (!arg.empty() && arg[0] != '-') {
          write_targets.push_back(resolve_user_path(arg, start_cwd));
        }
      }

      for (const auto &target : write_targets) {
        if (!is_path_allowed_for_write(target, repo_root, playground_root,
                                       temp_root)) {
          report_error("Write path blocked by policy: " + target.string());
          return;
        }
      }
    }

    int timeout_sec = params.value("timeout_sec", 120);
    timeout_sec = std::max(1, std::min(timeout_sec, 600));

    velix::utils::ProcessResult exec_res;
    try {
      exec_res = velix::utils::ProcessSpawner::run_sync_with_timeout(
          cmd, args, {}, timeout_sec * 1000, start_cwd.string());
    } catch (const std::exception &e) {
      report_error(std::string("Failed to run command: ") + e.what());
      return;
    }

    std::stringstream rendered;
    rendered << cmd;
    for (const auto &arg : args) {
      rendered << " \"" << arg << "\"";
    }

    // 4. Report Final Result back to Parent
    json result = {
        {"status", exec_res.success ? "success" : "error"},
        {"exit_code", exec_res.exit_code},
        {"output", exec_res.stdout_content},
        {"timed_out", exec_res.timed_out},
        {"command", rendered.str()},
        {"cwd", start_cwd.string()},
        {"policy",
         {{"write_allowed_roots",
           {"config", "memory", "skills", "build/skills/commandline/agent_playground",
            "build/skills/commandline/agent_playground/tmp"}},
          {"blocked_commands", blocked_commands_list()}}}};

    // Dispatch correctly to the parent requester via the Velix Bus
    report_result(parent_pid, result, entry_trace_id);
    result_reported = true; // Prevents the SDK ResultGuard from sending a
                            // duplicate generic 'completed'
  }

private:
  static std::vector<std::string> blocked_commands_list() {
    return {"bash", "sh",  "zsh",   "fish", "python", "python3",
            "node", "perl", "ruby", "sudo", "curl",   "wget",
            "ssh",  "scp",  "nc",    "ncat", "socat",  "docker",
            "chmod", "chown", "mount", "umount", "dd",  "mkfs"};
  }

  static bool is_blocked_command(const std::string &cmd) {
    const auto blocked = blocked_commands_list();
    return std::find(blocked.begin(), blocked.end(), cmd) != blocked.end();
  }

  static bool is_mutating_command(const std::string &cmd) {
    static const std::set<std::string> mutating = {
        "mkdir", "touch", "rm",   "rmdir", "mv",     "cp",
        "tee",   "sed",   "awk",  "truncate", "git", "npm",
        "pnpm",  "yarn",  "cargo", "go",      "cmake", "make"};
    return mutating.count(cmd) > 0;
  }

  static std::filesystem::path detect_repo_root() {
    auto cur = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
      if (std::filesystem::exists(cur / "skills") &&
          std::filesystem::exists(cur / "config") &&
          std::filesystem::exists(cur / "memory")) {
        return cur;
      }
      if (!cur.has_parent_path()) {
        break;
      }
      cur = cur.parent_path();
    }
    // Fallback for this repo layout when workdir is build/skills/commandline.
    return std::filesystem::current_path().parent_path().parent_path().parent_path();
  }

  static std::filesystem::path resolve_user_path(const std::string &raw,
                                                 const std::filesystem::path &cwd) {
    std::filesystem::path p(raw);
    if (p.is_absolute()) {
      return p.lexically_normal();
    }
    return (cwd / p).lexically_normal();
  }

  static bool is_within(const std::filesystem::path &child,
                        const std::filesystem::path &parent) {
    const std::string c = child.lexically_normal().string();
    const std::string p = parent.lexically_normal().string();
    if (c == p) {
      return true;
    }
    if (c.size() <= p.size()) {
      return false;
    }
    return c.rfind(p + std::string(1, std::filesystem::path::preferred_separator), 0) == 0;
  }

  static bool is_path_allowed_for_write(const std::filesystem::path &candidate,
                                        const std::filesystem::path &repo_root,
                                        const std::filesystem::path &playground_root,
                                        const std::filesystem::path &temp_root) {
    const auto cfg = repo_root / "config";
    const auto mem = repo_root / "memory";
    const auto skills = repo_root / "skills";
    return is_within(candidate, cfg) || is_within(candidate, mem) ||
           is_within(candidate, skills) || is_within(candidate, playground_root) ||
           is_within(candidate, temp_root);
  }

  std::filesystem::path resolve_start_cwd(const std::filesystem::path &repo_root,
                                          const std::filesystem::path &playground_root,
                                          const std::filesystem::path &temp_root) {
    const std::string mode = params.value("cwd_mode", std::string("playground"));
    if (mode == "tmp") {
      return temp_root;
    }
    if (mode == "repo") {
      return repo_root;
    }
    if (params.contains("cwd") && params["cwd"].is_string()) {
      auto p = resolve_user_path(params["cwd"].get<std::string>(), repo_root);
      if (is_within(p, repo_root)) {
        return p;
      }
    }
    return playground_root;
  }

  void report_error(const std::string &message) {
    json err = {{"status", "error"}, {"error", message}};
    report_result(parent_pid, err, entry_trace_id);
    result_reported = true;
  }
};

int main() {
  CommandLineSkill skill;
  // start() performs:
  // 1. REGISTER_PID with Supervisor
  // 2. Spawns heartbeat & Bus listeners
  // 3. Blocks until run() completes
  skill.start();
  return 0;
}
