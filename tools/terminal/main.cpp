/**
 * commandline — Velix Tool
 */

#include "../../runtime/sdk/cpp/velix_process.hpp"
#include "../../utils/logger.hpp"
#include "../../utils/string_utils.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32) && !defined(_WIN64)
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <termios.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <util.h>
#  else
#    include <pty.h>
#  endif
#endif

#if defined(_WIN32) || defined(_WIN64)
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#endif

#include "../../communication/network_config.hpp"
#include "../../communication/socket_wrapper.hpp"
#include "../../core/persistant_applications/terminal_driver.hpp"

namespace fs = std::filesystem;
using namespace velix::core;
using velix::app_manager::ExecResult;
using velix::app_manager::DriverConfig;
using velix::app_manager::TerminalDriver;

// ─────────────────────────────────────────────────────────────────────────────
// Config
// ─────────────────────────────────────────────────────────────────────────────

struct TerminalConfig {
    std::string approval_mode        = "smart";
    int         approval_timeout     = 30;
    int         default_timeout_sec  = 30;
    int         max_timeout_sec      = 600;
    int         poll_interval_ms     = 200;
    int         poll_grace_sec       = 30;  // extra seconds beyond job timeout for AM latency
    size_t      max_output_chars     = 50000;
    std::string playground_root      = "../../agent_playground";
    bool        allow_path_escape    = false;
    std::string permanent_path       = ".velix/terminal/allowlist.txt";
    std::vector<std::string> entries;
};

static TerminalConfig &skill_config() {
    static TerminalConfig cfg;
    return cfg;
}

static fs::path &config_root_dir() {
    static fs::path dir = fs::current_path();
    return dir;
}

static std::mutex &skill_log_mutex() { static std::mutex mx; return mx; }
static std::mutex &approval_mutex()  { static std::mutex mx; return mx; }

// read_env_var removed in favor of velix::utils::get_env_value

static void skill_log(const std::string &stage, const json &fields = json::object()) {
     try {
         std::scoped_lock lock(skill_log_mutex());
         fs::create_directories("logs");
         std::ofstream out("logs/ask_user_trace.log", std::ios::app);
         if (!out.is_open()) return;
         json record = {
             {"ts_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count()},
             {"stage", stage}, {"fields", fields}};
         out << record.dump() << "\n";
     } catch (const std::exception &) {
         // Logging failures are non-critical; silently ignore to avoid disrupting execution
     }
}

static std::string expand_home(const std::string &p) {
    if (p.empty() || p[0] != '~') return p;
#ifdef _WIN32
    std::string h = velix::utils::get_env_value("USERPROFILE");
#else
    std::string h = velix::utils::get_env_value("HOME");
#endif
    if (h.empty()) h = "/tmp";
    return h + p.substr(1);
}

static fs::path resolve_config_path(const std::string &p) {
    fs::path raw(expand_home(p));
    if (raw.is_absolute()) return raw;
    return fs::absolute(config_root_dir() / raw);
}

static TerminalConfig load_config(const std::string &path) {
    TerminalConfig cfg;
    try {
        config_root_dir() = fs::absolute(path).parent_path().parent_path();
    } catch (...) {
        config_root_dir() = fs::current_path();
    }

    std::ifstream f(path);
    if (!f.is_open()) return cfg;

    std::string raw((std::istreambuf_iterator<char>(f)), {});
    try {
        json j = json::parse(raw, nullptr, true, true);
        auto gs = [](const json &o, const char *k, const std::string &d) {
            return (o.contains(k) && o[k].is_string()) ? o[k].get<std::string>() : d;
        };
        auto gi = [](const json &o, const char *k, int d) {
            return (o.contains(k) && o[k].is_number()) ? o[k].get<int>() : d;
        };
        auto gb = [](const json &o, const char *k, bool d) {
            return (o.contains(k) && o[k].is_boolean()) ? o[k].get<bool>() : d;
        };
        auto gz = [](const json &o, const char *k, size_t d) {
            return (o.contains(k) && o[k].is_number()) ? o[k].get<size_t>() : d;
        };
        if (j.contains("approval") && j["approval"].is_object()) {
            auto &a = j["approval"];
            cfg.approval_mode    = gs(a, "mode",        cfg.approval_mode);
            cfg.approval_timeout = gi(a, "timeout_sec", cfg.approval_timeout);
        }
        if (j.contains("execution") && j["execution"].is_object()) {
            auto &e = j["execution"];
            cfg.default_timeout_sec = gi(e, "default_timeout_sec", cfg.default_timeout_sec);
            cfg.max_timeout_sec     = gi(e, "max_timeout_sec",     cfg.max_timeout_sec);
            cfg.max_output_chars    = gz(e, "max_output_chars",    cfg.max_output_chars);
            cfg.poll_interval_ms    = gi(e, "poll_interval_ms",    cfg.poll_interval_ms);
            cfg.poll_grace_sec      = gi(e, "poll_grace_sec",      cfg.poll_grace_sec);
        }
        if (j.contains("sandbox") && j["sandbox"].is_object()) {
            auto &s = j["sandbox"];
            cfg.playground_root   = gs(s, "playground_root",   cfg.playground_root);
            cfg.allow_path_escape = gb(s, "allow_path_escape", cfg.allow_path_escape);
        }
        if (j.contains("allowlist") && j["allowlist"].is_object()) {
             auto &al = j["allowlist"];
             cfg.permanent_path = gs(al, "permanent_path", cfg.permanent_path);
             if (al.contains("entries") && al["entries"].is_array())
                 for (const auto &e : al["entries"])
                     if (e.is_string()) cfg.entries.push_back(e.get<std::string>());
         }
     } catch (const std::exception &) {
         // Config parsing failed; use defaults
     }
     return cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
// Allowlist helpers
// ─────────────────────────────────────────────────────────────────────────────

static fs::path perm_al_path() {
    return resolve_config_path(skill_config().permanent_path);
}

static std::string exact_cmd_key(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::set<std::string, std::less<>> load_perm_al() {
    std::set<std::string, std::less<>> out;
    std::ifstream f(perm_al_path());
    std::string line;
    while (std::getline(f, line)) {
        size_t p = line.find_first_not_of(" \t\r\n");
        if (p != std::string::npos && line[p] != '#') out.insert(line.substr(p));
    }
    return out;
}

static void save_perm_al(const std::set<std::string, std::less<>> &keys) {
    auto p = perm_al_path();
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::trunc);
    f << "# Velix commandline permanent approval list\n";
    for (const auto &k : keys) f << k << "\n";
}

static bool is_approved(const std::string &key) {
    std::scoped_lock lk(approval_mutex());
    for (const auto &e : skill_config().entries) if (e == key) return true;
    return load_perm_al().count(key) > 0;
}

static void approve_permanent(const std::string &key) {
    std::scoped_lock lk(approval_mutex());
    auto p = load_perm_al();
    p.insert(key);
    save_perm_al(p);
}

// ─────────────────────────────────────────────────────────────────────────────
// Dangerous command detection
// ─────────────────────────────────────────────────────────────────────────────

struct DangerPattern {
    std::string re;
    std::string desc;
};

// BUG-29 fix: removed `env` and `printenv` — reading env vars is legitimate.
// BUG-28 fix: normalize_cmd now strips shell quotes so `rm -rf "/"` is caught.
static const std::vector<DangerPattern> DANGEROUS_PATTERNS = {
    // Destructive filesystem operations
    {R"(\brm\s+(-[-\w]*\s+)*-[-\w]*r[-\w]*\s+[/~])", "Recursive delete from root or home"},
    {R"(\brm\s+(-[-\w]*\s+)*-[-\w]*f[-\w]*\s+[/~])", "Force-delete from root or home"},
    {R"(\brm\s+(-[-\w]*\s+)*-[-\w]*r[-\w]*\s+\*)",   "Recursive delete with wildcard"},
    {R"(\bmkfs\b)",                                     "Format filesystem (mkfs)"},
    {R"(\bdd\b.*\bof=/dev/)",                           "Direct disk write via dd"},
    {R"(\bshred\b.*(/dev/|/etc/|/boot/|/sys/|/proc/))", "Shred on critical path"},
    {R"(\btruncate\b.*(/etc/|/boot/|/sys/))",           "Truncate system file"},
    {R"(>\s*/dev/sd[a-z])",                             "Overwrite raw block device"},

    // Process/system destruction
    {R"(:\s*\{[^}]*:\s*\|[^}]*:\s*\}[^;]*;)",          "Fork bomb"},
    {R"(\bkill\s+-9\s+-1\b)",                           "Kill all processes"},
    {R"(\breboot\b|\bshutdown\b|\bhalt\b|\bpoweroff\b)", "System reboot/shutdown"},
    {R"(\bsysctl\s+-w\b)",                              "Modify kernel parameters"},

    // Permission/ownership escalation
    {R"(\bchmod\s+[0-7]*777\s+(/etc|/bin|/usr|/sbin|/))", "chmod 777 on system directory"},
    {R"(\bchown\s+\S+\s+(/etc|/bin|/usr|/sbin|/))",       "chown on system directory"},
    {R"(\bvisudo\b)",                                      "Edit sudoers (visudo)"},
    {R"(\bsudo\s+su\b)",                                   "Escalate to root shell"},
    {R"(>\s*/etc/sudoers)",                                "Overwrite sudoers"},
    {R"(>\s*/etc/passwd)",                                 "Overwrite passwd"},
    {R"(>\s*/etc/shadow)",                                 "Overwrite shadow"},
    {R"(>\s*/etc/cron)",                                   "Overwrite cron config"},
    {R"(>\s*/etc/hosts)",                                  "Overwrite /etc/hosts"},

    // Package removal
    {R"(\bpip\s+uninstall\b.*-y)",                         "Force-uninstall Python packages"},
    {R"(\bapt(-get)?\s+purge\b)",                          "apt purge packages"},
    {R"(\byum\s+remove\b)",                                "yum remove packages"},

    // Sensitive file reads
    {R"(\bcat\s+/etc/shadow\b)",                           "Read shadow password file"},

    // Reverse shells / code execution via download
    {R"(\bnc\b.*(-e|-c)\b)",                               "Netcat reverse shell"},
    {R"(\bbash\b.*-i.*>&.*/dev/tcp/)",                     "Bash TCP reverse shell"},
    {R"(\bpython[23]?\b.*-c.*socket.*connect)",            "Python reverse shell"},
    {R"(\bcurl\b.*\|\s*(ba)?sh\b)",                        "Pipe curl to shell"},
    {R"(\bwget\b.*-O\s*-.*\|\s*(ba)?sh\b)",                "Pipe wget to shell"},
    {R"(\bcurl\b.*base64.*\|\s*(ba)?sh\b)",                "Pipe base64-encoded curl to shell"},
};

struct DetectResult {
    bool        is_dangerous  = false;
    std::string pattern_key;
    std::string description;
};

// BUG-28 fix: strip shell quotes before regex matching so `rm -rf "/"` is caught.
static std::string normalize_cmd(const std::string &s) {
    std::string out;
    bool sp = false;
    bool in_single_quote = false;
    bool in_double_quote = false;

    for (char c : s) {
        // Track quote state to strip quotes (but not their content).
        if (c == '\'' && !in_double_quote) { in_single_quote = !in_single_quote; continue; }
        if (c == '"'  && !in_single_quote) { in_double_quote = !in_double_quote; continue; }
        // Normalise whitespace.
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        if (c == ' ') {
            if (!sp) { out += ' '; sp = true; }
        } else {
            sp = false;
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    // Trim.
    size_t a = out.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = out.find_last_not_of(' ');
    return out.substr(a, b - a + 1);
}

static DetectResult detect_dangerous(const std::string &cmd) {
     const std::string norm = normalize_cmd(cmd);
     for (const auto &dp : DANGEROUS_PATTERNS) {
         try {
             std::regex re(dp.re, std::regex::icase | std::regex::ECMAScript);
             if (std::regex_search(norm, re))
                 return {true, dp.re, dp.desc};
         } catch (const std::regex_error &) {
             // Malformed regex pattern; skip to next pattern
         }
     }
     return {};
}

static std::string truncate_output(const std::string &s) {
     size_t mx = skill_config().max_output_chars;
     if (s.size() <= mx) return s;
     size_t head = mx * 2 / 5;
     size_t tail = mx - head;
     size_t omit = s.size() - head - tail;
     return s.substr(0, head) +
            "\n... [TRUNCATED " + std::to_string(omit) + " chars] ...\n" +
            s.substr(s.size() - tail);
}

static std::string trim_copy(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool is_interactive_shell_request(const std::string &cmd) {
    const std::string t = trim_copy(cmd);
    return t == "bash" || t == "sh" || t == "zsh" || t == "fish";
}

static std::string build_pty_exec_cmd(const std::string &full_cmd,
                                      const fs::path &sandbox_cwd,
                                      bool force_cwd_prefix) {
    // Priority order:
    // 1. If force_cwd_prefix is true (explicit cwd passed), always prepend cd
    // 2. Otherwise, use command as-is (preserve session state)
    if (!force_cwd_prefix) return full_cmd;
    return "cd " + velix::app_manager::shell_quote(sandbox_cwd.string()) +
           " && " + full_cmd;
}

// ─────────────────────────────────────────────────────────────────────────────
// ApplicationManager client helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string am_call(const json &request, const std::string &host, int port) {
    velix::communication::SocketWrapper sock;
    sock.create_tcp_socket();
    sock.connect(host, static_cast<uint16_t>(port));
    velix::communication::send_json(sock, request.dump());
    return velix::communication::recv_json(sock);
}

// New helper: returns parsed JSON from ApplicationManager to avoid double-parse
static json am_call_parsed(const json &request, const std::string &host, int port) {
    velix::communication::SocketWrapper sock;
    sock.create_tcp_socket();
    sock.connect(host, static_cast<uint16_t>(port));
    velix::communication::send_json(sock, request.dump());
    return velix::communication::recv_json_parsed(sock);
}

static std::string am_execute(const std::string &user_id,
                              const std::string &cmd,
                              int timeout_sec,
                              const std::string &host,
                              int port,
                              const std::string &session_name,
                              const DriverConfig &dcfg) {
    json driver_config_json = {
        {"type",             dcfg.type},
        {"shell",            dcfg.shell},
        {"ssh_host",         dcfg.ssh_host},
        {"ssh_port",         dcfg.ssh_port},
        {"ssh_user",         dcfg.ssh_user},
        {"ssh_key_path",     dcfg.ssh_key_path},
        {"docker_container", dcfg.docker_container},
        {"docker_user",      dcfg.docker_user},
        {"docker_shell",     dcfg.docker_shell},
    };
    json req = {
        {"message_type",  "EXECUTE"},
        {"user_id",       user_id},
        {"cmd",           cmd},
        {"timeout_sec",   timeout_sec},
        {"driver_config", driver_config_json}
    };
    if (!session_name.empty()) req["session_name"] = session_name;

    json resp = am_call_parsed(req, host, port);
     if (resp.value("status", "") != "ok") {
         throw std::runtime_error("ApplicationManager EXECUTE error: " +
                                  resp.value("error", std::string("unknown")));
     }
     return resp.value("job_id", "");
}

// New helper: returns full EXECUTE response to check session_created flag
static json am_execute_full(const std::string &user_id,
                            const std::string &cmd,
                            int timeout_sec,
                            const std::string &host,
                            int port,
                            const std::string &session_name,
                            const DriverConfig &dcfg) {
    json driver_config_json = {
        {"type",             dcfg.type},
        {"shell",            dcfg.shell},
        {"ssh_host",         dcfg.ssh_host},
        {"ssh_port",         dcfg.ssh_port},
        {"ssh_user",         dcfg.ssh_user},
        {"ssh_key_path",     dcfg.ssh_key_path},
        {"docker_container", dcfg.docker_container},
        {"docker_user",      dcfg.docker_user},
        {"docker_shell",     dcfg.docker_shell},
    };
    json req = {
        {"message_type",  "EXECUTE"},
        {"user_id",       user_id},
        {"cmd",           cmd},
        {"timeout_sec",   timeout_sec},
        {"driver_config", driver_config_json}
    };
    if (!session_name.empty()) req["session_name"] = session_name;

    json resp = am_call_parsed(req, host, port);
     if (resp.value("status", "") != "ok") {
         throw std::runtime_error("ApplicationManager EXECUTE error: " +
                                  resp.value("error", std::string("unknown")));
     }
     return resp;
}

// BUG-25 fix: poll with a hard deadline (job timeout + poll_grace_sec).
static json am_poll_until_done(const std::string &job_id,
                               int job_timeout_sec,
                               int poll_interval_ms,
                               int poll_grace_sec,
                               const std::string &host,
                               int port) {
    const int total_timeout_sec = (job_timeout_sec > 0)
        ? job_timeout_sec + poll_grace_sec
        : 0; // 0 = no deadline (infinite job)

    auto start = std::chrono::steady_clock::now();

    while (true) {
        if (total_timeout_sec > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start).count();
            if (elapsed >= total_timeout_sec) {
                LOG_WARN("am_poll_until_done: poll deadline elapsed for job " + job_id);
                 return {{"status",     "ok"},
                         {"job_status", "timeout"},
                         {"exit_code",  124},
                         {"output",     "[tool] Poll deadline exceeded — job may still be running"}};
             }
         }

          json req = {{"message_type", "POLL"}, {"job_id", job_id}};
          json resp;
          try {
              // Use the parsed variant to avoid allocating a string and parsing it
              // again at the call site.
              resp = am_call_parsed(req, host, port);
          } catch (const std::exception &e) {
              // AM unreachable — return an error rather than looping forever.
              return {{"status",     "ok"},
                      {"job_status", "error"},
                      {"exit_code",  -1},
                      {"output",     std::string("[tool] Poll failed: ") + e.what()}};
         }

         if (resp.value("status", "") != "ok") {
             resp["job_status"] = "error";
             return resp;
         }
         if (const std::string job_status = resp.value("job_status", "error");
             job_status != "running") return resp;

        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Approval types
// ─────────────────────────────────────────────────────────────────────────────

enum class ApprovalScope { Once, Permanent, Deny };

struct ApprovalResult {
    bool          approved     = false;
    ApprovalScope scope        = ApprovalScope::Deny;
    std::string   message;
    std::string   pattern_key;
    std::string   description;
};

static ApprovalResult approved_ok(const std::string &msg) {
    return {true, ApprovalScope::Once, msg, {}, {}};
}

// ─────────────────────────────────────────────────────────────────────────────
// Execution context
// ─────────────────────────────────────────────────────────────────────────────

struct ExecutionContext {
    std::string uid;
    std::string full_cmd;
    std::string cmd;
    std::vector<std::string> arg_list;
    std::string session_name;
    fs::path sandbox_cwd;
    bool force_cwd_prefix;
    int timeout_sec;
    bool use_pty;
    ApprovalResult approval;
    std::string am_host;
    int am_port;
    DriverConfig dcfg;
};

// ─────────────────────────────────────────────────────────────────────────────
// TerminalTool
// ─────────────────────────────────────────────────────────────────────────────

class TerminalTool : public VelixProcess {
public:
    TerminalTool() : VelixProcess("terminal", "tool") {}

    void run() override {
        skill_config() = load_config("../../config/terminal.json");

        const std::string cmd = params.value("cmd", "");
        if (cmd.empty()) return done_error("Parameter 'cmd' is required.");

        bool use_pty      = params.value("pty", false);
        bool background   = params.value("background", false);
        std::string cwd         = params.value("cwd", "");
        std::string session_name = params.value("session_name", "");
        const bool has_explicit_cwd = params.contains("cwd") && !cwd.empty();

         int timeout_sec = params.value("timeout_sec",
             use_pty ? 0 : skill_config().default_timeout_sec);
         timeout_sec = std::clamp(timeout_sec, 0, skill_config().max_timeout_sec);

        std::vector<std::string> arg_list;
        if (!use_pty && params.contains("args") && params["args"].is_array()) {
            for (const auto &a : params["args"])
                if (a.is_string()) arg_list.push_back(a.get<std::string>());
        }

        std::string full_cmd = cmd;
        for (const auto &a : arg_list) full_cmd += " " + a;

        if (use_pty && is_interactive_shell_request(full_cmd)) {
            return done_error("Refusing to run interactive shell command in PTY job mode (bash/sh/zsh/fish) because it never emits completion sentinel. Run concrete commands (e.g. 'pwd') in the existing session instead.");
        }

        // Non-PTY mode: cmd must be a plain binary name (no spaces).
        if (!use_pty && cmd.find_first_of(" \t\n\r") != std::string::npos)
            return done_error("Non-PTY mode requires a single binary name in 'cmd'; "
                              "pass shell commands via pty=true or put args in 'args'.");

        if (use_pty && session_name.empty())
            return done_error("pty=true requires 'session_name' to identify the PTY session.");

        const std::string uid = user_id.empty() ? "anonymous" : user_id;

        // BUG-30 fix: load driver config from user config; fall back to local_pty.
        DriverConfig dcfg = resolve_driver_config();

        const std::string am_host =
            velix::communication::resolve_service_host("APP_MANAGER", "127.0.0.1");
        const int am_port = velix::utils::get_port("APP_MANAGER", 5175);

        // Approval gate.
        ApprovalResult approval = check_approval(full_cmd);
        if (!approval.approved) return done_blocked(approval);

        // Resolve sandbox working directory.
        fs::path sandbox_cwd;
        try {
            sandbox_cwd = velix::app_manager::resolve_sandbox_cwd(
                cwd,
                resolve_config_path(skill_config().playground_root).string(),
                skill_config().allow_path_escape);
        } catch (const std::exception &ex) {
            return done_error(std::string("Invalid cwd: ") + ex.what());
        }

         if (background) {
             ExecutionContext ctx{
                 uid, full_cmd, cmd, arg_list, session_name,
                 sandbox_cwd, has_explicit_cwd, timeout_sec, use_pty,
                 approval, am_host, am_port, dcfg
             };
             run_background(ctx);
             return;
         }

        if (use_pty) {
            run_pty_foreground(uid, full_cmd, session_name, sandbox_cwd,
                               has_explicit_cwd,
                               timeout_sec, approval, am_host, am_port, dcfg);
            return;
        }

        run_oneshot_direct(cmd, arg_list, sandbox_cwd,
                           timeout_sec, dcfg, approval);
    }

private:
    // ── Driver config resolution ──────────────────────────────────────

    DriverConfig resolve_driver_config() {
        // user_config is a JSON blob injected by the Supervisor into our params.
         // Key: "user_driver_config" — a DriverConfig JSON object.
         if (params.contains("user_driver_config") &&
             params["user_driver_config"].is_object()) {
             try {
                 return DriverConfig::from_json(params["user_driver_config"]);
             } catch (const std::exception &) {
                 // Configuration parsing failed; fall back to legacy driver field
             }
         }
        // Legacy single-field driver hint.
        DriverConfig dcfg;
        dcfg.type = params.value("driver", "local_pty");
        return dcfg;
    }

    // ── PTY foreground execution ──────────────────────────────────────

    void run_pty_foreground(const std::string &uid,
                            const std::string &full_cmd,
                            const std::string &session_name,
                            const fs::path    &sandbox_cwd,
                            bool               force_cwd_prefix,
                            int                timeout_sec,
                            const ApprovalResult &approval,
                            const std::string &am_host,
                            int                am_port,
                             const DriverConfig &dcfg) {
        try {
            // Get full EXECUTE response to check if session was just created
            json exec_resp = am_execute_full(
                uid, full_cmd, timeout_sec, am_host, am_port, session_name, dcfg);
            
            bool is_first_call = exec_resp.value("session_created", false);
            
            // Build exec command with proper cwd logic:
            // - If explicit cwd passed (force_cwd_prefix), always prepend cd
            // - Else if first call, auto-cd to playground_root
            // - Else preserve session state (no cd)
            std::string exec_cmd = full_cmd;
            if (force_cwd_prefix) {
                // Explicit cwd override
                exec_cmd = "cd " + velix::app_manager::shell_quote(sandbox_cwd.string()) +
                          " && " + full_cmd;
            } else if (is_first_call) {
                // First call: auto-cd to playground_root
                exec_cmd = "cd " + velix::app_manager::shell_quote(sandbox_cwd.string()) +
                          " && " + full_cmd;
            }
            // Else: use full_cmd as-is (preserve session state)

            const std::string job_id = exec_resp.value("job_id", "");
            
            json final_resp = am_poll_until_done(
                job_id, timeout_sec, skill_config().poll_interval_ms,
                skill_config().poll_grace_sec, am_host, am_port);

             const std::string job_status = final_resp.value("job_status", "error");
             std::string status_str;
             if (job_status == "finished") {
                 status_str = "ok";
             } else if (job_status == "timeout") {
                 status_str = "timeout";
             } else {
                 status_str = "error";
             }
             json result = {
                 {"status",    status_str},
                 {"exit_code", final_resp.value("exit_code", -1)},
                 {"output",    truncate_output(final_resp.value("output", ""))},
                 {"cwd",       sandbox_cwd.string()},
                {"timed_out", job_status == "timeout"}
            };
            if (!approval.message.empty()) result["approval_note"] = approval.message;
            report_result(parent_pid, result, entry_trace_id);
        } catch (const std::exception &ex) {
            done_error(std::string("Failed to start command: ") + ex.what());
        }
    }

    // ── One-shot direct execution (foreground) ───────────────────────

    void run_oneshot_direct(const std::string &cmd,
                            const std::vector<std::string> &arg_list,
                            const fs::path    &sandbox_cwd,
                            int                timeout_sec,
                            const DriverConfig &dcfg,
                            const ApprovalResult &approval) {
        try {
            // Instantiate driver directly (no ApplicationManager)
            std::unique_ptr<TerminalDriver> driver = 
                velix::app_manager::make_driver(dcfg);

            // Execute command synchronously
            ExecResult exec = driver->run_cmd(cmd, arg_list, sandbox_cwd, timeout_sec, false);

            // Format result
            std::string status_str;
            if (exec.timed_out) {
                status_str = "timeout";
            } else if (exec.exit_code == 0) {
                status_str = "ok";
            } else {
                status_str = "error";
            }

            json result = {
                {"status",    status_str},
                {"exit_code", exec.exit_code},
                {"output",    truncate_output(exec.out)},
                {"stderr",    truncate_output(exec.err)},
                {"cwd",       sandbox_cwd.string()},
                {"timed_out", exec.timed_out}
            };
            if (!approval.message.empty()) result["approval_note"] = approval.message;
            report_result(parent_pid, result, entry_trace_id);
        } catch (const std::exception &ex) {
            done_error(std::string("Execution failed: ") + ex.what());
        }
    }

    // ── Background execution ──────────────────────────────────────────

    void run_background(ExecutionContext ctx) {
         json immediate = {
             {"status",  "background"},
             {"message", "Process started in background. You will be notified on completion."},
             {"cmd",     ctx.full_cmd},
             {"cwd",     ctx.sandbox_cwd.string()}
         };
         report_result(parent_pid, immediate, entry_trace_id, false);

          if (ctx.use_pty) {
              try {
                  // Get full EXECUTE response to check if session was just created
                  json exec_resp = am_execute_full(
                      ctx.uid, ctx.full_cmd, ctx.timeout_sec, ctx.am_host, ctx.am_port, 
                      ctx.session_name, ctx.dcfg);
                  
                  bool is_first_call = exec_resp.value("session_created", false);
                  
                  // Build exec command with proper cwd logic (same as foreground)
                  std::string exec_cmd = ctx.full_cmd;
                  if (ctx.force_cwd_prefix) {
                      exec_cmd = "cd " + velix::app_manager::shell_quote(ctx.sandbox_cwd.string()) +
                                " && " + ctx.full_cmd;
                  } else if (is_first_call) {
                      exec_cmd = "cd " + velix::app_manager::shell_quote(ctx.sandbox_cwd.string()) +
                                " && " + ctx.full_cmd;
                  }
                  
                  const std::string job_id = exec_resp.value("job_id", "");
                  json final_resp = am_poll_until_done(
                      job_id, ctx.timeout_sec, skill_config().poll_interval_ms,
                      skill_config().poll_grace_sec, ctx.am_host, ctx.am_port);

                  const std::string job_status = final_resp.value("job_status", "error");
                  std::string status_str;
                  if (job_status == "finished") {
                      status_str = "ok";
                  } else if (job_status == "timeout") {
                      status_str = "timeout";
                  } else {
                      status_str = "error";
                  }
                  json note = {
                      {"notify_type", "TOOL_RESULT"},
                      {"tool",        "terminal"},
                      {"result", {
                          {"status",    status_str},
                          {"exit_code", final_resp.value("exit_code", -1)},
                          {"output",    truncate_output(final_resp.value("output", ""))},
                          {"cwd",       ctx.sandbox_cwd.string()},
                          {"timed_out", job_status == "timeout"},
                          {"cmd",       ctx.full_cmd},
                          {"background", true}
                      }}
                  };
                  if (!ctx.approval.message.empty()) note["result"]["approval_note"] = ctx.approval.message;
                  send_message(-1, "NOTIFY_HANDLER", note);
              } catch (const std::exception &ex) {
                  send_message(-1, "NOTIFY_HANDLER", {
                      {"notify_type", "TOOL_RESULT"}, {"tool", "terminal"},
                      {"result", {{"status","error"},{"error",ex.what()},
                                  {"cmd",ctx.full_cmd},{"background",true}}}
                  });
              }
              return;
          }

          try {
              // Instantiate driver directly for background one-shot execution
              std::unique_ptr<TerminalDriver> driver = 
                  velix::app_manager::make_driver(ctx.dcfg);
              
              ExecResult exec = driver->run_cmd(
                  ctx.cmd, ctx.arg_list, ctx.sandbox_cwd, ctx.timeout_sec, false);
              
              std::string status_str;
              if (exec.timed_out) {
                  status_str = "timeout";
              } else if (exec.exit_code == 0) {
                  status_str = "ok";
              } else {
                  status_str = "error";
              }
              json note = {
                  {"notify_type", "TOOL_RESULT"}, {"tool", "terminal"},
                  {"result", {
                      {"status",    status_str},
                      {"exit_code", exec.exit_code},
                      {"output",    truncate_output(exec.out)},
                      {"stderr",    truncate_output(exec.err)},
                      {"cwd",       ctx.sandbox_cwd.string()},
                      {"timed_out", exec.timed_out},
                      {"cmd",       ctx.full_cmd},
                      {"background", true}
                  }}
              };
              if (!ctx.approval.message.empty()) note["result"]["approval_note"] = ctx.approval.message;
              send_message(-1, "NOTIFY_HANDLER", note);
          } catch (const std::exception &ex) {
              send_message(-1, "NOTIFY_HANDLER", {
                  {"notify_type", "TOOL_RESULT"}, {"tool", "terminal"},
                  {"result", {{"status","error"},
                              {"error", std::string("Execution failed: ") + ex.what()},
                              {"cmd", ctx.full_cmd}, {"background", true}}}
              });
          }
     }

    // ── Approval gate ─────────────────────────────────────────────────

    ApprovalResult check_approval(const std::string &full_cmd) {
         const std::string uid = user_id.empty() ? "anonymous" : user_id;

         std::string mode = skill_config().approval_mode;
         std::transform(mode.begin(), mode.end(), mode.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
         if (mode != "off" && mode != "manual" && mode != "smart") mode = "smart";

         const std::string exact_key = mode + ":" + uid + ":" + exact_cmd_key(full_cmd);

         if (mode == "off") {
             skill_log("approval_skip_mode_off", {{"command", full_cmd}});
             return approved_ok("approval disabled");
         }

         DetectResult det;
         if (mode == "manual") {
             det = {true, exact_key, "manual approval mode"};
         } else {
             det = detect_dangerous(full_cmd);
             if (!det.is_dangerous) {
                 skill_log("approval_not_required", {{"command", full_cmd}});
                 return approved_ok("");
             }
             det.pattern_key = exact_key;
         }

         skill_log("approval_required",
                   {{"command", full_cmd}, {"approval_mode", mode},
                    {"pattern_key", det.pattern_key}, {"description", det.description}});

         if (is_approved(det.pattern_key)) {
             skill_log("approval_allowlist_hit",
                       {{"command", full_cmd}, {"pattern_key", det.pattern_key}});
             return {true, ApprovalScope::Permanent, "Previously approved",
                     det.pattern_key, det.description};
         }

         return ask_handler(full_cmd, det.pattern_key, det.description);
     }

    ApprovalResult ask_handler(const std::string &full_cmd,
                               const std::string &pattern_key,
                               const std::string &description) {
        const std::string trace = velix::utils::generate_uuid();

        std::mutex      cv_mx;
        std::condition_variable cv;
        bool        got_reply  = false;
        std::string reply_scope = "deny";

        // BUG-27 fix: set on_bus_event BEFORE sending the bus message,
        // and use cv_mx consistently so there is no lost-wakeup window.
        {
            std::scoped_lock lk(cv_mx);
            on_bus_event = [&cv_mx, &cv, &got_reply, &reply_scope, trace](const json &msg) {
                const std::string purpose = msg.value("purpose", "");
                if (purpose != "ASK_USER_REPLY") return;
                const json &pl = msg.value("payload", json::object());
                const std::string msg_trace = pl.value("trace", "");
                if (msg_trace != trace) return;

                {
                    std::scoped_lock ilk(cv_mx);
                    // New protocol: selected_option_id; legacy: scope.
                    const std::string selected = pl.value("selected_option_id",
                        pl.value("scope", "deny"));
                    // Map option IDs to legacy scope values for compatibility.
                    if (selected == "allow") reply_scope = "once";
                    else if (selected == "whitelist") reply_scope = "permanent";
                    else if (selected == "deny") reply_scope = "deny";
                    else reply_scope = selected; // passthrough (once/permanent/deny)
                    got_reply = true;
                }
                cv.notify_one();
            };
        }

        skill_log("ask_user_request_created",
                  {{"trace", trace}, {"command", full_cmd},
                   {"pattern_key", pattern_key}, {"description", description}});

        // Build ask_user payload with options for the gateway.
        json options = json::array();
        options.push_back({{"id", "allow"}, {"label", "Allow"}});
        options.push_back({{"id", "deny"}, {"label", "Deny"}});
        options.push_back({{"id", "whitelist"}, {"label", "Whitelist"}});

        std::string question = "Action Required";
        if (!full_cmd.empty()) question += ": " + full_cmd;
        if (!description.empty()) question += " (" + description + ")";

        send_message(-1, "ASK_USER_REQUEST",
                     {{"trace", trace},
                      {"question", question},
                      {"options", options},
                      {"allow_free_text", false},
                      {"metadata", {{"command", full_cmd},
                                    {"description", description},
                                    {"pattern_key", pattern_key}}}});

        skill_log("ask_user_request_sent",
                  {{"trace", trace},
                   {"timeout_sec", skill_config().approval_timeout}});

        bool timed_out;
        {
            std::unique_lock<std::mutex> lk(cv_mx);
            timed_out = !cv.wait_for(lk,
                std::chrono::seconds(skill_config().approval_timeout),
                [&] { return got_reply; });
        }

        {
            std::scoped_lock lk(cv_mx);
            on_bus_event = nullptr;
        }

        if (timed_out) {
            skill_log("ask_user_wait_timeout", {{"trace", trace}});
            return {false, ApprovalScope::Deny,
                    "Approval timed out after " +
                    std::to_string(skill_config().approval_timeout) + "s — denied.",
                    pattern_key, description};
         }

         ApprovalScope scope;
         if (reply_scope == "once") {
             scope = ApprovalScope::Once;
         } else if (reply_scope == "permanent") {
             scope = ApprovalScope::Permanent;
         } else {
             scope = ApprovalScope::Deny;
         }
         if (scope == ApprovalScope::Deny) {
            skill_log("ask_user_denied", {{"trace", trace}, {"scope", reply_scope}});
            return {false, scope, "User denied: " + description, pattern_key, description};
        }
        if (scope == ApprovalScope::Permanent) approve_permanent(pattern_key);

        skill_log("ask_user_granted",
                  {{"trace", trace}, {"scope", reply_scope},
                   {"pattern_key", pattern_key}});
        return {true, scope, "Approved (" + reply_scope + ")", pattern_key, description};
    }

    // ── Result helpers ────────────────────────────────────────────────

    void done_error(const std::string &msg) {
        report_result(parent_pid,
                      {{"status", "error"}, {"error", msg},
                       {"exit_code", -1}, {"output", ""}},
                      entry_trace_id);
    }

    void done_blocked(const ApprovalResult &ar) {
        report_result(parent_pid,
                      {{"status",      "blocked"},
                       {"error",       ar.message},
                       {"pattern_key", ar.pattern_key},
                       {"description", ar.description},
                       {"exit_code",   -1},
                       {"output",      ""}},
                      entry_trace_id);
    }
};

int main() {
    TerminalTool tool;
    try {
        tool.start();
    } catch (const std::exception &) {
        return 1;
    }
    return 0;
}
