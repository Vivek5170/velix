/**
 * commandline — Velix Tool
 *
 * Features:
 *   • Foreground execution  — blocks, returns full output via report_result
 *   • Background execution  — returns immediately, notifies handler via
 *                             send_message("NOTIFY_HANDLER") when done,
 *                             keeps process alive until that notification fires
 *   • PTY mode              — pseudo-terminal for interactive tools
 *                             (forkpty on Linux/macOS, ConPTY on Windows)
 *   • Dangerous-command approval  — send_message("APPROVAL_REQUEST") to
 *                             handler, block on on_bus_event for reply
 *   • Session + permanent allowlists
 *   • Config from  ../../config/terminal.json
 *
 * ─── Input (VELIX_PARAMS) ────────────────────────────────────────────────────
 *  {
 *    "cmd":         "python3",
 *    "args":        ["server.py"],
 *    "timeout_sec": 60,        // foreground only; ignored in background
 *    "cwd_mode":    "repo",    // playground | tmp | repo
 *    "cwd":         "src",     // sub-path under cwd_mode root
 *    "background":  false,     // true → fire-and-forget, notify handler on
 * finish "pty":         false,     // true → allocate pseudo-terminal "force":
 * false      // true → skip approval check
 *  }
 *
 * ─── Approval protocol ───────────────────────────────────────────────────────
 *  Tool → Handler  purpose="APPROVAL_REQUEST"
 *    { "approval_trace":"<uuid>", "command":"...",
 *      "description":"...", "pattern_key":"..." }
 *
 *  Handler → Tool  purpose="APPROVAL_REPLY"
 *    { "approval_trace":"<uuid>",
 *      "scope": "once"|"session"|"always"|"deny" }
 *
 * ─── Background completion notification ──────────────────────────────────────
 *  Tool → Handler  purpose="NOTIFY_HANDLER"
 *    { "notify_type":"TOOL_RESULT", "tool":"terminal",
 *      "result":{
 *        "status":"ok"|"error"|"timeout", "exit_code":0,
 *        "output":"...", "stderr":"...", "cwd":"...", "timed_out":false,
 *        "cmd":"...", "background":true
 *      }}
 *
 * ─── report_result payload ───────────────────────────────────────────────────
 *  foreground:
 *    { "status":"ok"|"error"|"timeout"|"blocked", "exit_code":0,
 *      "output":"...", "stderr":"...", "cwd":"...", "timed_out":false }
 *  background (immediate):
 *    { "status":"background", "message":"Process started in background.
 *      You will be notified when it completes.", "cmd":"...", "cwd":"..." }
 *  blocked:
 *    { "status":"blocked", "error":"...",
 *      "pattern_key":"...", "description":"..." }
 */

#include "../../runtime/sdk/cpp/velix_process.hpp"
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

// ── POSIX-only headers
// ────────────────────────────────────────────────────────
#if !defined(_WIN32) && !defined(_WIN64)
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h> // forkpty on macOS (links -lutil)
#endif
#if !defined(__APPLE__)
#include <pty.h> // forkpty on Linux (links -lutil)
#endif
#endif

// ── Windows-only headers
// ──────────────────────────────────────────────────────
#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <processthreadsapi.h> // ConPTY
#include <Windows.h>
#include <winternl.h>
// ConPTY requires Windows 10 1809+
#endif

namespace fs = std::filesystem;
using namespace velix::core;

// ============================================================================
//  Config
// ============================================================================

struct TerminalConfig {
  std::string approval_mode = "smart"; // smart | off | manual
  int approval_timeout = 30;

  int default_timeout_sec = 30;
  int max_timeout_sec = 600;
  size_t max_output_chars = 50000;

  std::string playground_root = "agent_playground";
  bool allow_path_escape = false;

  std::string permanent_path = ".velix/terminal/allowlist.txt";
  std::vector<std::string> entries;
};

class TerminalToolException : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

static TerminalConfig &skill_config() {
  static TerminalConfig cfg;
  return cfg;
}

static fs::path &config_root_dir() {
  static fs::path dir = fs::current_path();
  return dir;
}

static std::mutex &skill_log_mutex() {
  static std::mutex mx;
  return mx;
}

static std::mutex &approval_mutex() {
  static std::mutex mx;
  return mx;
}

static std::set<std::string, std::less<>> &session_allowlist() {
  static std::set<std::string, std::less<>> allowlist;
  return allowlist;
}

static std::optional<std::string> read_env_var(const char *name) {
#if defined(_WIN32) || defined(_WIN64)
  char *value = nullptr;
  std::size_t len = 0;
  const errno_t err = _dupenv_s(&value, &len, name);
  if (err != 0 || value == nullptr) {
    return std::nullopt;
  }
  std::string out(value);
  std::free(value);
  return out;
#else
  if (const char *value = std::getenv(name); value != nullptr) {
    return std::string(value);
  }
  return std::nullopt;
#endif
}

static void skill_log(const std::string &stage,
                      const json &fields = json::object()) {
  try {
    std::scoped_lock lock(skill_log_mutex());
    fs::create_directories("logs");
    std::ofstream out("logs/approval_trace.log", std::ios::app);
    if (!out.is_open()) {
      return;
    }
    json record = {
        {"ts_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count()},
        {"stage", stage},
        {"fields", fields}};
    out << record.dump() << "\n";
  } catch (const std::exception &) {
    // Best-effort logging only.
  }
}

static std::string expand_home(const std::string &p) {
  if (p.empty() || p[0] != '~')
    return p;
#if defined(_WIN32) || defined(_WIN64)
  const auto h = read_env_var("USERPROFILE");
#else
  const auto h = read_env_var("HOME");
#endif
  return std::string(h.has_value() ? *h : "/tmp") + p.substr(1);
}

static fs::path resolve_config_path(const std::string &p) {
  fs::path raw(expand_home(p));
  if (raw.is_absolute())
    return raw;
  return fs::absolute(config_root_dir() / raw);
}

static TerminalConfig load_config(const std::string &path) {
  TerminalConfig cfg;
  try {
    fs::path cfg_path = fs::absolute(path);
    config_root_dir() = cfg_path.parent_path().parent_path();
  } catch (const std::exception &) {
    config_root_dir() = fs::current_path();
  }
  std::ifstream f(path);
  if (!f.is_open())
    return cfg;
  std::string raw((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
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
      cfg.approval_mode = gs(a, "mode", cfg.approval_mode);
      cfg.approval_timeout = gi(a, "timeout_sec", cfg.approval_timeout);
    }
    if (j.contains("execution") && j["execution"].is_object()) {
      auto &e = j["execution"];
      cfg.default_timeout_sec =
          gi(e, "default_timeout_sec", cfg.default_timeout_sec);
      cfg.max_timeout_sec = gi(e, "max_timeout_sec", cfg.max_timeout_sec);
      cfg.max_output_chars = gz(e, "max_output_chars", cfg.max_output_chars);
    }
    if (j.contains("sandbox") && j["sandbox"].is_object()) {
      auto &s = j["sandbox"];
      cfg.playground_root = gs(s, "playground_root", cfg.playground_root);
      cfg.allow_path_escape = gb(s, "allow_path_escape", cfg.allow_path_escape);
    }
    if (j.contains("allowlist") && j["allowlist"].is_object()) {
      auto &al = j["allowlist"];
      cfg.permanent_path = gs(al, "permanent_path", cfg.permanent_path);
      if (al.contains("entries") && al["entries"].is_array())
        for (const auto &e : al["entries"])
          if (e.is_string())
            cfg.entries.push_back(e.get<std::string>());
    }
  } catch (const nlohmann::json::exception &) {
  }
  return cfg;
}

// ============================================================================
//  Allowlist
// ============================================================================

static fs::path perm_al_path() {
  return resolve_config_path(skill_config().permanent_path);
}

static std::string exact_cmd_key(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos)
    return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static std::set<std::string, std::less<>> load_perm_al() {
  std::set<std::string, std::less<>> out;
  std::ifstream f(perm_al_path());
  std::string line;
  while (std::getline(f, line)) {
    size_t p = line.find_first_not_of(" \t\r\n");
    if (p != std::string::npos && line[p] != '#')
      out.insert(line.substr(p));
  }
  return out;
}

static void save_perm_al(const std::set<std::string, std::less<>> &keys) {
  auto p = perm_al_path();
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::trunc);
  f << "# Velix commandline permanent approval list\n";
  for (const auto &k : keys)
    f << k << "\n";
}

static bool is_approved(const std::string &key) {
  std::scoped_lock lk(approval_mutex());
  for (const auto &e : skill_config().entries)
    if (e == key)
      return true;
  if (session_allowlist().count(key))
    return true;
  return load_perm_al().count(key) > 0;
}
static void approve_session(const std::string &key) {
  std::scoped_lock lk(approval_mutex());
  session_allowlist().insert(key);
}
static void approve_permanent(const std::string &key) {
  std::scoped_lock lk(approval_mutex());
  auto p = load_perm_al();
  p.insert(key);
  save_perm_al(p);
  session_allowlist().insert(key);
}

// ============================================================================
//  Dangerous patterns
// ============================================================================

struct DangerPattern {
  std::string re, desc;
};

static const std::vector<DangerPattern> DANGEROUS_PATTERNS = {
    {R"(\brm\s+(-[a-z]*\s+)*-[a-z]*r[a-z]*\s+/)",
     "Recursive delete from filesystem root"},
    {R"(\brm\s+(-[a-z]*\s+)*-[a-z]*f[a-z]*\s+/)",
     "Force-delete from filesystem root"},
    {R"(\bmkfs\b)", "Format filesystem (mkfs)"},
    {R"(\bdd\b.*\bof=/dev/)", "Direct disk write via dd"},
    {R"(:\s*\{[^}]*:\s*\|[^}]*:\s*\}[^;]*;)", "Fork bomb pattern"},
    {R"(\bshred\b.*(/dev/|/etc/|/boot/|/sys/|/proc/))",
     "Shred on critical system path"},
    {R"(\bchmod\s+[0-7]*777\s+(/etc|/bin|/usr|/sbin|/))",
     "chmod 777 on system directory"},
    {R"(\bchown\s+\S+\s+(/etc|/bin|/usr|/sbin|/))",
     "chown on system directory"},
    {R"(\bvisudo\b)", "Edit sudoers (visudo)"},
    {R"(\bsudo\s+su\b)", "Escalate to root shell"},
    {R"(>\s*/etc/sudoers)", "Overwrite sudoers file"},
    {R"(>\s*/etc/passwd)", "Overwrite passwd file"},
    {R"(>\s*/etc/shadow)", "Overwrite shadow file"},
    {R"(\bnc\b.*(-e|-c)\b)", "Netcat reverse shell"},
    {R"(\bbash\b.*-i.*>&.*/dev/tcp/)", "Bash reverse shell"},
    {R"(\bpython[23]?\b.*-c.*socket.*connect)", "Python reverse shell"},
    {R"(\bcurl\b.*\|\s*(ba)?sh)", "Pipe curl to shell"},
    {R"(\bwget\b.*-O\s*-.*\|\s*(ba)?sh)", "Pipe wget to shell"},
    {R"(>\s*/dev/sd[a-z])", "Overwrite raw block device"},
    {R"(\btruncate\b.*(/etc/|/boot/|/sys/))", "Truncate system file"},
    {R"(>\s*/etc/cron)", "Overwrite cron config"},
    {R"(>\s*/etc/hosts)", "Overwrite /etc/hosts"},
    {R"(\bkill\s+-9\s+-1\b)", "Kill all processes"},
    {R"(\breboot\b|\bshutdown\b|\bhalt\b|\bpoweroff\b)",
     "System reboot/shutdown"},
    {R"(\bsysctl\s+-w\b)", "Modify kernel parameters"},
    {R"(\bpip\s+uninstall\b.*-y)", "Force-uninstall Python packages"},
    {R"(\bapt(-get)?\s+purge\b)", "apt purge packages"},
    {R"(\byum\s+remove\b)", "yum remove packages"},
    {R"(\bcat\s+/etc/shadow\b)", "Read shadow password file"},
    {R"(\bcat\s+/etc/passwd\b)", "Read passwd file"},
    {R"(\benv\b|\bprintenv\b)", "Dump environment variables"},
};

struct DetectResult {
  bool is_dangerous = false;
  std::string pattern_key, description;
};

static std::string normalize_cmd(const std::string &s) {
  std::string out;
  bool sp = false;
  for (char c : s) {
    if (c == '\t' || c == '\n' || c == '\r')
      c = ' ';
    if (c == ' ') {
      if (!sp) {
        out += ' ';
        sp = true;
      }
    } else {
      sp = false;
      out += (char)tolower((unsigned char)c);
    }
  }
  size_t a = out.find_first_not_of(' ');
  if (a == std::string::npos)
    return "";
  size_t b = out.find_last_not_of(' ');
  return out.substr(a, b - a + 1);
}

static DetectResult detect_dangerous(const std::string &cmd) {
  std::string norm = normalize_cmd(cmd);
  for (const auto &dp : DANGEROUS_PATTERNS) {
    try {
      std::regex re(dp.re, std::regex::icase | std::regex::ECMAScript);
      if (std::regex_search(norm, re))
        return {true, dp.desc, dp.desc};
    } catch (const std::regex_error &) {
    }
  }
  return {};
}

// ============================================================================
//  CWD resolution
// ============================================================================

static fs::path resolve_cwd(const std::string &mode, const std::string &sub) {
  fs::path base;
  if (mode == "tmp")
    base = fs::path(skill_config().playground_root) / "tmp";
  else if (mode == "repo")
    base = fs::path(skill_config().playground_root) / "repo";
  else
    base = fs::path(skill_config().playground_root);
  if (!sub.empty())
    base /= sub;

  fs::path abs = fs::weakly_canonical(fs::absolute(base));
  fs::path root =
      fs::weakly_canonical(fs::absolute(skill_config().playground_root));

  if (!skill_config().allow_path_escape) {
    auto rs = fs::relative(abs, root).native();
    if (rs.size() >= 2 && rs[0] == '.' && rs[1] == '.')
      throw TerminalToolException("cwd escapes sandbox: " + abs.string());
  }
  fs::create_directories(abs);
  return abs;
}

// ============================================================================
//  ExecResult — shared by foreground, background, PTY paths
// ============================================================================

struct ExecResult {
  std::string out, err;
  int exit_code = 0;
  bool timed_out = false;
};

// ============================================================================
//  Output truncation
// ============================================================================

static std::string trunc(const std::string &s) {
  size_t mx = skill_config().max_output_chars;
  if (s.size() <= mx)
    return s;
  size_t head = mx * 2 / 5, tail = mx - head, omit = s.size() - head - tail;
  return s.substr(0, head) + "\n... [TRUNCATED " + std::to_string(omit) +
         " chars] ...\n" + s.substr(s.size() - tail);
}

// ============================================================================
//  POSIX — plain pipe exec  (foreground + background non-PTY)
// ============================================================================

#if !defined(_WIN32) && !defined(_WIN64)

static ExecResult posix_exec(const std::string &cmd,
                             const std::vector<std::string> &args,
                             const fs::path &cwd, int timeout_sec) {
  std::vector<const char *> argv;
  argv.push_back(cmd.c_str());
  for (const auto &a : args)
    argv.push_back(a.c_str());
  argv.push_back(nullptr);

  int so[2], se[2];
  if (::pipe(so) || ::pipe(se))
    throw TerminalToolException("pipe: " + std::string(strerror(errno)));

  pid_t pid = ::fork();
  if (pid < 0)
    throw TerminalToolException("fork: " + std::string(strerror(errno)));

  if (pid == 0) {
    ::close(so[0]);
    ::close(se[0]);
    ::dup2(so[1], STDOUT_FILENO);
    ::dup2(se[1], STDERR_FILENO);
    ::close(so[1]);
    ::close(se[1]);
    if (::chdir(cwd.c_str()) != 0) {
      std::string m = "chdir " + cwd.string() + ": " + strerror(errno) + "\n";
      ::write(STDERR_FILENO, m.c_str(), m.size());
      ::_exit(1);
    }
    ::execvp(cmd.c_str(), const_cast<char *const *>(argv.data()));
    std::string m = std::string("execvp: ") + strerror(errno) + "\n";
    ::write(STDERR_FILENO, m.c_str(), m.size());
    ::_exit(127);
  }
  ::close(so[1]);
  ::close(se[1]);

  ExecResult res;
  auto read_fd = [](int fd, std::string &dst) {
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
      dst.append(buf, n);
    ::close(fd);
  };
  std::thread to(read_fd, so[0], std::ref(res.out));
  std::thread te(read_fd, se[0], std::ref(res.err));

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
  while (true) {
    int st = 0;
    pid_t wp = ::waitpid(pid, &st, WNOHANG);
    if (wp == pid) {
      res.exit_code = WIFEXITED(st)     ? WEXITSTATUS(st)
                      : WIFSIGNALED(st) ? 128 + WTERMSIG(st)
                                        : -1;
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &st, 0);
      res.timed_out = true;
      res.exit_code = 124;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  to.join();
  te.join();
  if (res.timed_out)
    res.err += "\n[commandline] Killed after " + std::to_string(timeout_sec) +
               "s (SIGKILL)";
  return res;
}

// ──────────────────────────────────────────────────────────────────────────────
//  POSIX PTY exec  (forkpty — Linux links -lutil, macOS links -lutil)
//
//  forkpty() allocates a master/slave pty pair, forks, and connects the
//  child's stdin/stdout/stderr to the slave end.  We read from the master
//  on the parent side.  This gives the child a real terminal so curses,
//  readline, Python REPL, etc. all work correctly.
//
//  timeout_sec is honoured: if the deadline passes we SIGHUP then SIGKILL.
// ──────────────────────────────────────────────────────────────────────────────

static ExecResult posix_exec_pty(const std::string &cmd,
                                 const std::vector<std::string> &args,
                                 const fs::path &cwd, int timeout_sec) {
  std::vector<const char *> argv;
  argv.push_back(cmd.c_str());
  for (const auto &a : args)
    argv.push_back(a.c_str());
  argv.push_back(nullptr);

  // Set a reasonable window size so the child doesn't wrap weirdly
  struct winsize ws{};
  ws.ws_row = 24;
  ws.ws_col = 220;

  int master_fd = -1;
  pid_t pid = ::forkpty(&master_fd, nullptr, nullptr, &ws);
  if (pid < 0)
    throw TerminalToolException("forkpty: " + std::string(strerror(errno)));

  if (pid == 0) {
    // Child — we already have a controlling terminal from forkpty
    if (::chdir(cwd.c_str()) != 0) {
      std::string m = "chdir " + cwd.string() + ": " + strerror(errno) + "\n";
      ::write(STDERR_FILENO, m.c_str(), m.size());
      ::_exit(1);
    }
    ::execvp(cmd.c_str(), const_cast<char *const *>(argv.data()));
    std::string m = std::string("execvp: ") + strerror(errno) + "\n";
    ::write(STDERR_FILENO, m.c_str(), m.size());
    ::_exit(127);
  }

  // Parent — read all output from master_fd until child exits or timeout
  // Set master to non-blocking so we can poll for both data and timeout
  ::fcntl(master_fd, F_SETFL, ::fcntl(master_fd, F_GETFL) | O_NONBLOCK);

  ExecResult res;
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);

  while (true) {
    // Try to read available data
    char buf[4096];
    ssize_t n = ::read(master_fd, buf, sizeof(buf));
    if (n > 0)
      res.out.append(buf, n);
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EIO)
      break;
    else if (n == 0 || errno == EIO)
      break; // slave closed (child exited)

    // Check if child has exited
    int st = 0;
    pid_t wp = ::waitpid(pid, &st, WNOHANG);
    if (wp == pid) {
      // Drain any remaining output
      ::fcntl(master_fd, F_SETFL, ::fcntl(master_fd, F_GETFL) & ~O_NONBLOCK);
      while ((n = ::read(master_fd, buf, sizeof(buf))) > 0)
        res.out.append(buf, n);
      res.exit_code = WIFEXITED(st)     ? WEXITSTATUS(st)
                      : WIFSIGNALED(st) ? 128 + WTERMSIG(st)
                                        : -1;
      break;
    }

    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(pid, SIGHUP);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &st, 0);
      res.timed_out = true;
      res.exit_code = 124;
      res.err = "[commandline] PTY process killed after " +
                std::to_string(timeout_sec) + "s";
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ::close(master_fd);
  return res;
}

#endif // POSIX

// ============================================================================
//  Windows exec  (CreateProcess — plain pipes)
// ============================================================================

#if defined(_WIN32) || defined(_WIN64)

static ExecResult win_exec(const std::string &cmd,
                           const std::vector<std::string> &args,
                           const fs::path &cwd, int timeout_sec) {
  // Build command line string
  std::string cmdline = "\"" + cmd + "\"";
  for (const auto &a : args)
    cmdline += " \"" + a + "\"";

  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE hStdoutR, hStdoutW, hStderrR, hStderrW;
  if (!CreatePipe(&hStdoutR, &hStdoutW, &sa, 0) ||
      !CreatePipe(&hStderrR, &hStderrW, &sa, 0))
    throw TerminalToolException("CreatePipe failed");
  SetHandleInformation(hStdoutR, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(hStderrR, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = hStdoutW;
  si.hStdError = hStderrW;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi{};
  std::string cwds = cwd.string();
  if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW, nullptr, cwds.c_str(), &si, &pi)) {
    CloseHandle(hStdoutR);
    CloseHandle(hStdoutW);
    CloseHandle(hStderrR);
    CloseHandle(hStderrW);
    throw TerminalToolException("CreateProcess failed: " +
                   std::to_string(GetLastError()));
  }
  CloseHandle(hStdoutW);
  CloseHandle(hStderrW);

  ExecResult res;
  auto read_handle = [](HANDLE h, std::string &dst) {
    char buf[4096];
    DWORD n;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
      dst.append(buf, n);
    CloseHandle(h);
  };
  std::thread to(read_handle, hStdoutR, std::ref(res.out));
  std::thread te(read_handle, hStderrR, std::ref(res.err));

  DWORD ms = (timeout_sec <= 0) ? INFINITE : (DWORD)(timeout_sec * 1000);
  DWORD wr = WaitForSingleObject(pi.hProcess, ms);
  if (wr == WAIT_TIMEOUT) {
    TerminateProcess(pi.hProcess, 124);
    res.timed_out = true;
    res.exit_code = 124;
    res.err += "\n[commandline] Process killed after " +
               std::to_string(timeout_sec) + "s";
  } else {
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    res.exit_code = (int)ec;
  }
  to.join();
  te.join();
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return res;
}

// ──────────────────────────────────────────────────────────────────────────────
//  Windows ConPTY exec  (requires Windows 10 1809+)
//
//  We create a pseudo-console (HPCON) with CreatePseudoConsole(),
//  spawn the child attached to it, and read its output from the read-end
//  of the output pipe.  This gives the child a real Windows console so
//  any console-mode application (python REPL, PowerShell, etc.) works.
// ──────────────────────────────────────────────────────────────────────────────

static ExecResult win_exec_pty(const std::string &cmd,
                               const std::vector<std::string> &args,
                               const fs::path &cwd, int timeout_sec) {
  // Resolve CreatePseudoConsole at runtime — not available on old Windows
  typedef HRESULT(WINAPI * PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE,
                                                    DWORD, HPCON *);
  typedef void(WINAPI * PFN_ClosePseudoConsole)(HPCON);
  HMODULE hKernel = GetModuleHandleA("kernel32.dll");
  auto fnCreate =
      (PFN_CreatePseudoConsole)GetProcAddress(hKernel, "CreatePseudoConsole");
  auto fnClose =
      (PFN_ClosePseudoConsole)GetProcAddress(hKernel, "ClosePseudoConsole");
  if (!fnCreate || !fnClose)
    // Fallback to plain pipes on older Windows
    return win_exec(cmd, args, cwd, timeout_sec);

  // Build command line
  std::string cmdline = "\"" + cmd + "\"";
  for (const auto &a : args)
    cmdline += " \"" + a + "\"";

  // Create pipe pair for PTY I/O
  HANDLE hPipeInR, hPipeInW, hPipeOutR, hPipeOutW;
  CreatePipe(&hPipeInR, &hPipeInW, nullptr, 0);
  CreatePipe(&hPipeOutR, &hPipeOutW, nullptr, 0);

  COORD sz{220, 24};
  HPCON hPC{};
  HRESULT hr = fnCreate(sz, hPipeInR, hPipeOutW, 0, &hPC);
  CloseHandle(hPipeInR);
  CloseHandle(hPipeOutW); // PTY owns these ends now

  ExecResult res;
  if (FAILED(hr)) {
    CloseHandle(hPipeInW);
    CloseHandle(hPipeOutR);
    res.err =
        "[commandline] ConPTY creation failed, falling back to plain exec";
    auto r2 = win_exec(cmd, args, cwd, timeout_sec);
    r2.err = res.err + "\n" + r2.err;
    return r2;
  }

  // Build STARTUPINFOEX with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
  std::vector<char> attrBuf(attrSize);
  LPPROC_THREAD_ATTRIBUTE_LIST attrList =
      (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
  InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
  UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                            hPC, sizeof(hPC), nullptr, nullptr);

  STARTUPINFOEXW siex{};
  siex.StartupInfo.cb = sizeof(siex);
  siex.lpAttributeList = attrList;

  std::wstring wcmd(cmdline.begin(), cmdline.end());
  std::wstring wcwd(cwd.native().begin(), cwd.native().end());

  PROCESS_INFORMATION pi{};
  bool ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                           EXTENDED_STARTUPINFO_PRESENT, nullptr, wcwd.c_str(),
                           &siex.StartupInfo, &pi);

  DeleteProcThreadAttributeList(attrList);
  CloseHandle(hPipeInW); // not needed by parent

  if (!ok) {
    fnClose(hPC);
    CloseHandle(hPipeOutR);
    throw TerminalToolException("CreateProcessW (ConPTY) failed: " +
                                 std::to_string(GetLastError()));
  }

  // Read output
  std::thread to([&] {
    char buf[4096];
    DWORD n;
    while (ReadFile(hPipeOutR, buf, sizeof(buf), &n, nullptr) && n > 0)
      res.out.append(buf, n);
    CloseHandle(hPipeOutR);
  });

  DWORD ms = (timeout_sec <= 0) ? INFINITE : (DWORD)(timeout_sec * 1000);
  DWORD wr = WaitForSingleObject(pi.hProcess, ms);
  if (wr == WAIT_TIMEOUT) {
    TerminateProcess(pi.hProcess, 124);
    res.timed_out = true;
    res.exit_code = 124;
    res.err = "[commandline] PTY process killed after " +
              std::to_string(timeout_sec) + "s";
  } else {
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    res.exit_code = (int)ec;
  }
  to.join();
  fnClose(hPC);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return res;
}

#endif // WINDOWS

// ============================================================================
//  Platform dispatcher
// ============================================================================

static ExecResult run_cmd(const std::string &cmd,
                          const std::vector<std::string> &args,
                          const fs::path &cwd, int timeout_sec, bool use_pty) {
#if !defined(_WIN32) && !defined(_WIN64)
  return use_pty ? posix_exec_pty(cmd, args, cwd, timeout_sec)
                 : posix_exec(cmd, args, cwd, timeout_sec);
#endif
#if defined(_WIN32) || defined(_WIN64)
  return use_pty ? win_exec_pty(cmd, args, cwd, timeout_sec)
                 : win_exec(cmd, args, cwd, timeout_sec);
#endif
}

static bool looks_like_shell_command(const std::string &cmd,
                                     const std::vector<std::string> &args) {
  if (!args.empty()) {
    return false;
  }
  return cmd.find(' ') != std::string::npos ||
         cmd.find('\t') != std::string::npos ||
         cmd.find('|') != std::string::npos ||
         cmd.find('&') != std::string::npos ||
         cmd.find(';') != std::string::npos;
}

static std::pair<std::string, std::vector<std::string>>
normalize_exec_request(const std::string &cmd,
                       const std::vector<std::string> &args) {
  if (!looks_like_shell_command(cmd, args)) {
    return {cmd, args};
  }
#if defined(_WIN32) || defined(_WIN64)
  return {"cmd.exe", {"/C", cmd}};
#else
  return {"/bin/sh", {"-lc", cmd}};
#endif
}

// ============================================================================
//  Approval types
// ============================================================================

enum class ApprovalScope { Once, Session, Always, Deny };
struct ApprovalResult {
  bool approved = false;
  ApprovalScope scope = ApprovalScope::Deny;
  std::string message, pattern_key, description;
};

// ============================================================================
//  TerminalTool
// ============================================================================

class TerminalTool : public VelixProcess {
public:
  TerminalTool() : VelixProcess("terminal", "tool") {}

  void run() override {
    // ── 0. Config ───────────────────────────────────────────────────────
    skill_config() = load_config("../../config/terminal.json");

    // ── 1. Parse params ─────────────────────────────────────────────────
    std::string cmd = params.value("cmd", "");
    int timeout_sec = params.value("timeout_sec", skill_config().default_timeout_sec);
    timeout_sec = std::clamp(timeout_sec, 1, skill_config().max_timeout_sec);
    std::string cwd_mode = params.value("cwd_mode", "playground");
    std::string sub_cwd = params.value("cwd", "");
    bool use_pty = params.value("pty", false);
    bool background = params.value("background", false);
    bool force = params.value("force", false);

    std::vector<std::string> arg_list;
    if (params.contains("args") && params["args"].is_array())
      for (const auto &a : params["args"])
        if (a.is_string())
          arg_list.push_back(a.get<std::string>());

    // ── 2. Validate ─────────────────────────────────────────────────────
    if (cmd.empty())
      return done_error("Parameter 'cmd' is required.");

    std::string full_cmd = cmd;
    for (const auto &a : arg_list)
      full_cmd += " " + a;

    auto exec_request = normalize_exec_request(cmd, arg_list);
    const std::string exec_cmd = exec_request.first;
    const std::vector<std::string> exec_args = exec_request.second;

    // ── 3. Approval ─────────────────────────────────────────────────────
    ApprovalResult approval = check_approval(full_cmd, force);
    if (!approval.approved)
      return done_blocked(approval);

    // ── 4. CWD ──────────────────────────────────────────────────────────
    fs::path cwd;
    try {
      cwd = resolve_cwd(cwd_mode, sub_cwd);
    } catch (const std::exception &ex) {
      return done_error("CWD error: " + std::string(ex.what()));
    }

    // ── 5a. BACKGROUND ───────────────────────────────────────────────────
    // Reply immediately so the calling LLM knows context, then run on a
    // worker thread.  Keep the process alive until the worker is done
    // (we hold run() by joining), then send NOTIFY_HANDLER and exit.
    if (background) {
      // Immediate acknowledgement to caller
      report_result(parent_pid,
                    {{"status", "background"},
                     {"message", "Process started in background. "
                                 "You will be notified when it completes."},
                     {"cmd", full_cmd},
                     {"cwd", cwd.string()}},
                    entry_trace_id, false);

      // Run on worker — we hold a reference to *this via lambda capture,
      // which is safe because run() (and therefore the process) doesn't
      // return until the thread finishes.
      std::thread worker([this, exec_cmd, exec_args, cwd, timeout_sec, use_pty,
                          full_cmd, approval]() {
        ExecResult exec;
        try {
          exec = run_cmd(exec_cmd, exec_args, cwd, timeout_sec, use_pty);
        } catch (const std::exception &ex) {
          send_message(
              -1, "NOTIFY_HANDLER",
              {{"notify_type", "TOOL_RESULT"},
               {"tool", "terminal"},
               {"result",
                {{"status", "error"},
                 {"error", std::string("Execution failed: ") + ex.what()},
                 {"cmd", full_cmd},
                 {"background", true}}}});
          return;
        }

        json note = {{"notify_type", "TOOL_RESULT"},
                     {"tool", "terminal"},
                     {"result",
                      {{"status", exec.timed_out        ? "timeout"
                                              : exec.exit_code == 0 ? "ok"
                                                                    : "error"},
                       {"exit_code", exec.exit_code},
                       {"output", trunc(exec.out)},
                       {"stderr", trunc(exec.err)},
                       {"cwd", cwd.string()},
                       {"timed_out", exec.timed_out},
                       {"cmd", full_cmd},
                       {"background", true}}}};
        if (!approval.message.empty())
          note["result"]["approval_note"] = approval.message;

        send_message(-1, "NOTIFY_HANDLER", note);
      });

      worker.join(); // keep process alive until worker completes
      return;
    }

    // ── 5b. FOREGROUND ───────────────────────────────────────────────────
    ExecResult exec;
    try {
      exec = run_cmd(exec_cmd, exec_args, cwd, timeout_sec, use_pty);
    } catch (const std::exception &ex) {
      return done_error("Execution failed: " + std::string(ex.what()));
    }

    json result = {{"status", exec.timed_out        ? "timeout"
                              : exec.exit_code == 0 ? "ok"
                                                    : "error"},
                   {"exit_code", exec.exit_code},
                   {"output", trunc(exec.out)},
                   {"stderr", trunc(exec.err)},
                   {"cwd", cwd.string()},
                   {"timed_out", exec.timed_out}};
    if (!approval.message.empty())
      result["approval_note"] = approval.message;

    report_result(parent_pid, result, entry_trace_id);
  }

private:
  // ── Approval ──────────────────────────────────────────────────────────

  ApprovalResult check_approval(const std::string &full_cmd, bool force) {
    const std::string uid = user_id.empty() ? std::string("anonymous") : user_id;
    std::string mode = skill_config().approval_mode;
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (mode != "off" && mode != "manual" && mode != "smart")
      mode = "smart";
    const std::string exact_key = mode + ":" + uid + ":" + exact_cmd_key(full_cmd);

    if (force) {
      skill_log("approval_skip_force", {{"command", full_cmd}});
      return approved_ok("force flag set");
    }
    if (mode == "off") {
      skill_log("approval_skip_mode_off", {{"command", full_cmd}});
      return approved_ok("approval disabled");
    }

    bool must_prompt = (mode == "manual");
    DetectResult det;
    if (!must_prompt) {
      det = detect_dangerous(full_cmd);
      if (!det.is_dangerous) {
        skill_log("approval_not_required", {{"command", full_cmd}});
        return approved_ok("");
      }
      det.pattern_key = exact_key;
    } else {
      det = {true, exact_key, "manual approval mode"};
    }

    skill_log("approval_required",
              {{"command", full_cmd},
               {"approval_mode", mode},
               {"pattern_key", det.pattern_key},
               {"description", det.description}});

    if (is_approved(det.pattern_key)) {
      skill_log("approval_allowlist_hit",
                {{"command", full_cmd}, {"pattern_key", det.pattern_key}});
      return {true, ApprovalScope::Session, "Previously approved",
              det.pattern_key, det.description};
    }

    return ask_handler(full_cmd, det.pattern_key, det.description);
  }

  // ── Bus-based approval ─────────────────────────────────────────────────
  //
  //  on_bus_event is called by bus_listener_thread.
  //  We must not do heavy work inside it — per SDK rules.
  //  So we just set shared state + notify the cv, and do all the
  //  allowlist writes / scope logic back on the run() thread.
  //
  //  The assignment to on_bus_event is protected by bus_mutex (the SDK
  //  uses bus_mutex around send_json, and bus_listener_thread calls
  //  on_bus_event while holding NO SDK mutex — so the only race is
  //  between our assignment and the callback invocation).
  //  We guard that with approval_mx below.

  ApprovalResult ask_handler(const std::string &full_cmd,
                             const std::string &pattern_key,
                             const std::string &description) {
    std::string trace = velix::utils::generate_uuid();
    skill_log("approval_request_created",
          {{"approval_trace", trace},
           {"command", full_cmd},
           {"pattern_key", pattern_key},
           {"description", description}});

    std::mutex approval_mx;
    std::condition_variable approval_cv;
    bool got_reply = false;
    std::string reply_scope = "deny";

    // Register hook under approval_mx so bus_listener_thread
    // always sees either nullptr or a fully constructed lambda.
    {
      std::scoped_lock lk(approval_mx);
      on_bus_event = [&approval_mx, &approval_cv, &got_reply, &reply_scope,
                      trace](const json &msg) {
        // Called from bus_listener_thread — must be fast.
        if (msg.value("purpose", "") != "APPROVAL_REPLY")
          return;
        const json &pl = msg.value("payload", json::object());
        if (pl.value("approval_trace", "") != trace)
          return;

        std::scoped_lock ilk(approval_mx);
        reply_scope = pl.value("scope", "deny");
        got_reply = true;
        skill_log("approval_reply_received",
                  {{"approval_trace", trace}, {"scope", reply_scope}});
        approval_cv.notify_one();
        // Heavy work (allowlist writes etc.) happens on run() thread
        // after wait_for returns — not here.
      };
    }

    send_message(-1, "APPROVAL_REQUEST",
                 {{"approval_trace", trace},
                  {"command", full_cmd},
                  {"description", description},
                  {"pattern_key", pattern_key}});
    skill_log("approval_request_sent",
          {{"approval_trace", trace},
           {"timeout_sec", skill_config().approval_timeout}});

    bool timed_out_flag;
    {
      std::unique_lock<std::mutex> lk(approval_mx);
      skill_log("approval_wait_start", {{"approval_trace", trace}});
      timed_out_flag = !approval_cv.wait_for(
          lk, std::chrono::seconds(skill_config().approval_timeout),
          [&] { return got_reply; });
    }

    // Safely clear the hook
    {
      std::scoped_lock lk(approval_mx);
      on_bus_event = nullptr;
    }

    if (timed_out_flag) {
      skill_log("approval_wait_timeout", {{"approval_trace", trace}});
      return {false, ApprovalScope::Deny,
              "Approval timed out after " +
                  std::to_string(skill_config().approval_timeout) + "s — denied.",
              pattern_key, description};
    }

    ApprovalScope scope = reply_scope == "once"      ? ApprovalScope::Once
          : reply_scope == "session" ? ApprovalScope::Session
          : reply_scope == "always"  ? ApprovalScope::Always
                 : ApprovalScope::Deny;

    if (scope == ApprovalScope::Deny) {
      skill_log("approval_denied",
                {{"approval_trace", trace}, {"scope", reply_scope}});
      return {false, scope, "User denied: " + description, pattern_key,
              description};
    }

    // Allowlist writes here, safely on run() thread
    if (scope == ApprovalScope::Session)
      approve_session(pattern_key);
    if (scope == ApprovalScope::Always)
      approve_permanent(pattern_key);

    skill_log("approval_granted",
              {{"approval_trace", trace},
               {"scope", reply_scope},
               {"pattern_key", pattern_key}});

    return {true, scope, "Approved (" + reply_scope + ")", pattern_key,
            description};
  }

  // ── Result helpers ────────────────────────────────────────────────────

  static ApprovalResult approved_ok(const std::string &msg) {
    return {true, ApprovalScope::Once, msg};
  }
  void done_error(const std::string &msg) {
    report_result(parent_pid,
                  {{"status", "error"},
                   {"error", msg},
                   {"exit_code", -1},
                   {"output", ""},
                   {"stderr", ""}},
                  entry_trace_id);
  }
  void done_blocked(const ApprovalResult &ar) {
    report_result(parent_pid,
                  {{"status", "blocked"},
                   {"error", ar.message},
                   {"pattern_key", ar.pattern_key},
                   {"description", ar.description},
                   {"exit_code", -1},
                   {"output", ""},
                   {"stderr", ""}},
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