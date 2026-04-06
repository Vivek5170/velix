#include "velix_process.hpp"

#include "../../../communication/network_config.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <string_view>
#include <stdexcept>
#include <vector>

#include "../../../utils/config_utils.hpp"
#include "../../../utils/logger.hpp"
#include "../../../utils/string_utils.hpp"

#ifdef _WIN32
#include <Psapi.h>
#include <Windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace velix {
namespace core {

namespace {
bool is_supported_intent_value(std::string_view intent) {
  return intent == "JOIN_PARENT_TREE" || intent == "NEW_TREE";
}

std::optional<std::string> read_env_var(const char *name) {
#ifdef _WIN32
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

// Helper function to parse environment variables
void parse_environment_variables(int &parent_pid, std::string &entry_trace_id,
                                 std::string &launch_intent, json &params,
                                 std::string &user_id) {
  if (const auto env_parent_pid = read_env_var("VELIX_PARENT_PID");
      env_parent_pid.has_value()) {
    parent_pid = std::atoi(env_parent_pid->c_str());
  }

  if (const auto env_trace_id = read_env_var("VELIX_TRACE_ID");
      env_trace_id.has_value()) {
    entry_trace_id = *env_trace_id;
  }

  if (const auto env_intent = read_env_var("VELIX_INTENT");
      env_intent.has_value()) {
    launch_intent = *env_intent;
  }

  if (const auto env_params = read_env_var("VELIX_PARAMS");
      env_params.has_value()) {
    try {
      params = json::parse(*env_params);
    } catch (const nlohmann::json::parse_error &) {
      params = json::object();
    }
  }

  if (const auto env_user_id = read_env_var("VELIX_USER_ID");
      env_user_id.has_value()) {
    user_id = *env_user_id;
  }
}

// Helper function for intent validation
std::string validate_launch_intent(std::string_view role, int parent_pid,
                                   std::string_view launch_intent) {
  if (launch_intent != "JOIN_PARENT_TREE" && launch_intent != "NEW_TREE") {
    if (role == "handler") {
      return "JOIN_PARENT_TREE";
    }
    return (parent_pid > 0) ? "JOIN_PARENT_TREE" : "NEW_TREE";
  }
  return std::string(launch_intent);
}

bool is_transient_socket_error(std::string_view err) {
  return err.find("timed out") != std::string::npos ||
         err.find("timeout") != std::string::npos ||
         err.find("errno 11") != std::string::npos ||
         err.find("errno 35") != std::string::npos ||
         err.find("errno 110") != std::string::npos ||
         err.find("Resource temporarily unavailable") != std::string::npos;
}

// Helper function for connection retries
void connect_with_retries(velix::communication::SocketWrapper &socket,
                          const std::string &service_name, int port,
                          int retry_limit, int retry_delay) {
  for (int i = 0; i < retry_limit; ++i) {
    try {
      socket.create_tcp_socket();
      socket.connect(
          velix::communication::resolve_service_host(service_name, "127.0.0.1"),
          static_cast<uint16_t>(port));
      return;
    } catch (...) {
      if (i == retry_limit - 1) {
        throw;
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(retry_delay * (i + 1)));
    }
  }
}

std::string resolve_effective_mode(std::string_view mode,
                                   std::string_view convo_id,
                                   std::string_view effective_user_id,
                                   bool is_handler) {
  if (!mode.empty()) {
    return std::string(mode);
  }
  if (convo_id.empty() && effective_user_id.empty()) {
    return "simple";
  }
  if (!effective_user_id.empty() && is_handler) {
    return "user_conversation";
  }
  return "conversation";
}

std::string receive_scheduler_response(
    velix::communication::SocketWrapper &scheduler_socket,
    bool request_streaming, int llm_timeout,
    const std::atomic<bool> &is_running,
    const std::function<void(const std::string &)> &on_token) {
  if (!request_streaming) {
    return velix::communication::recv_json(scheduler_socket);
  }

  const int max_chunks =
      velix::utils::get_config("SDK_STREAM_MAX_CHUNKS", 100000);
  int chunk_count = 0;
  const auto stream_deadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(llm_timeout + 10000);

  while (chunk_count++ < max_chunks) {
    if (!is_running.load()) {
      throw std::runtime_error("process shutdown during streaming");
    }

    if (std::chrono::steady_clock::now() > stream_deadline) {
      throw std::runtime_error("scheduler stream deadline exceeded");
    }

    std::string raw;
    try {
      raw = velix::communication::recv_json(scheduler_socket);
    } catch (const velix::communication::SocketTimeoutException &) {
      continue;
    } catch (const std::exception &e) {
      const std::string err = e.what();
      if (is_transient_socket_error(err)) {
        continue;
      }
      throw;
    }

    json message;
    try {
      message = json::parse(raw);
    } catch (...) {
      continue;
    }

    if (message.value("message_type", std::string("")) == "LLM_STREAM_CHUNK") {
      const std::string delta = message.value("delta", std::string(""));
      if (!delta.empty() && on_token) {
        on_token(delta);
      }
      continue;
    }

    return raw;
  }

  throw std::runtime_error("scheduler stream chunk limit exceeded");
}

json receive_executioner_ack(velix::communication::SocketWrapper &exec_socket,
                             int exec_timeout_ms,
                             const std::atomic<bool> &is_running) {
  const int poll_timeout_ms = std::max(250, std::min(exec_timeout_ms, 2000));
  exec_socket.set_timeout_ms(poll_timeout_ms);

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(exec_timeout_ms + 2000);

  while (std::chrono::steady_clock::now() < deadline) {
    if (!is_running.load()) {
      throw std::runtime_error("process shutdown during executioner ack wait");
    }

    try {
      const std::string raw = velix::communication::recv_json(exec_socket);
      return json::parse(raw);
    } catch (const velix::communication::SocketTimeoutException &) {
      continue;
    } catch (const std::exception &e) {
      const std::string err = e.what();
      if (is_transient_socket_error(err)) {
        continue;
      }
      throw;
    }
  }

  throw std::runtime_error("executioner ack deadline exceeded");
}
} // namespace

VelixProcess *VelixProcess::instance_ = nullptr;

#ifdef _WIN32
BOOL WINAPI os_signal_handler(DWORD fdwCtrlType) {
  if (fdwCtrlType == CTRL_C_EVENT || fdwCtrlType == CTRL_CLOSE_EVENT ||
      fdwCtrlType == CTRL_BREAK_EVENT) {
    if (VelixProcess::instance_) {
      VelixProcess::instance_->request_forced_shutdown();
    }
    return TRUE;
  }
  return FALSE;
}
#else
void posix_signal_handler(int /*signum*/) {
  if (VelixProcess::instance_) {
    VelixProcess::instance_->request_forced_shutdown();
  }
}
#endif

VelixProcess::VelixProcess(std::string name, std::string r)
    : process_name(std::move(name)), role(std::move(r)) {
#ifdef _WIN32
  os_pid = static_cast<int>(GetCurrentProcessId());
#else
  os_pid = static_cast<int>(getpid());
#endif
}

VelixProcess::~VelixProcess() noexcept {
  try {
    shutdown_impl(false);
  } catch (...) {
  }
  VelixProcess::instance_ = nullptr;
}

void VelixProcess::shutdown() {
  shutdown_impl(true);
}

void VelixProcess::shutdown_impl(bool invoke_hook) {
  if (is_running.load()) {
    is_running.store(false);
    force_terminate.store(true);

    if (invoke_hook) {
      try {
        on_shutdown();
      } catch (const std::exception &e) {
        LOG_WARN("VelixProcess on_shutdown() failed: " +
                 std::string(e.what()));
      } catch (...) {
        LOG_WARN("VelixProcess on_shutdown() failed with unknown exception");
      }
    }

    // Wake up the idle IO thread immediately so it can send the final heartbeat
    sleep_cv.notify_all();
    queue_cv.notify_all(); // Wake up any threads waiting for tool results

    if (bus_socket.is_open()) {
      bus_socket.close();
    }

    if (runtime_io_thread.joinable()) {
      runtime_io_thread.join();
    }

    if (bus_listener_thread.joinable()) {
      bus_listener_thread.join();
    }
  }
}

void VelixProcess::request_forced_shutdown() {
  forced_by_signal.store(true);
  force_terminate.store(true);
  shutdown();
}

uint64_t VelixProcess::get_current_memory_usage_mb() const {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return static_cast<uint64_t>(pmc.WorkingSetSize / (1024 * 1024));
  }
  return 0;
#else
  uint64_t rss = 0;
  std::ifstream statm("/proc/self/statm");
  if (statm) {
    uint64_t dummy;
    statm >> dummy >> rss;
    // rss is in pages. Usually 4096 bytes per page.
    return (rss * sysconf(_SC_PAGESIZE)) / (1024 * 1024);
  }
  return 0;
#endif
}

// static
int VelixProcess::resolve_port(const std::string &service_name, int fallback) {
  if (service_name == "SCHEDULER") {
    return velix::utils::get_port(
        "LLM_SCHEDULER", velix::utils::get_port("SCHEDULER", fallback));
  }
  return velix::utils::get_port(service_name, fallback);
}

void VelixProcess::start(int override_pid,
                         const std::string & /*parent_tree_id*/) {
  if (is_running.exchange(true)) {
    return;
  }
  force_terminate.store(false);
  forced_by_signal.store(false);

  // Set the global instance for signal handling
  instance_ = this;
#ifdef _WIN32
  SetConsoleCtrlHandler(os_signal_handler, TRUE);
#else
  signal(SIGTERM, posix_signal_handler);
  signal(SIGINT, posix_signal_handler);
#endif

  // Discover context from Environment (Injected by Executioner)
  parent_pid = -1;
  std::string launch_intent = "";
  is_root = true;
  is_handler = false;
  entry_trace_id.clear();
  params = json::object();
  user_id.clear();

  parse_environment_variables(parent_pid, entry_trace_id, launch_intent, params,
                              user_id);
  launch_intent = validate_launch_intent(role, parent_pid, launch_intent);
  is_root = (launch_intent == "NEW_TREE");

  const int sup_port = resolve_port("SUPERVISOR", 5173);
  const int retry_limit = velix::utils::get_config("SDK_RETRY_LIMIT", 3);
  const int retry_delay = velix::utils::get_config("SDK_RETRY_DELAY_MS", 500);

  connect_with_retries(supervisor_socket, "SUPERVISOR", sup_port, retry_limit,
                       retry_delay);

  // Register the agent explicitly with the OS kernel matching exact schema
  json payload = {
      {"register_intent", launch_intent},
      {"role", role},
      {"os_pid", os_pid},
      {"process_name", process_name},
      {"trace_id", entry_trace_id},
      {"status", "STARTING"},
      {"memory_mb", static_cast<double>(get_current_memory_usage_mb())}};

  json reg_msg = {{"message_type", "REGISTER_PID"}, {"payload", payload}};
  if (launch_intent == "JOIN_PARENT_TREE" && parent_pid > 0) {
    reg_msg["source_pid"] = parent_pid;
  }

  {
    std::lock_guard<std::mutex> lock(socket_mutex);
    velix::communication::send_json(supervisor_socket, reg_msg.dump());
    const std::string raw_reply =
        velix::communication::recv_json(supervisor_socket);
    const json reply = json::parse(raw_reply);

    if (reply.value("status", "") != "ok") {
      throw std::runtime_error("VelixProcess supervisor registration failed: " +
                               reply.value("error", "unknown"));
    }

    // The Kernel dynamically assigns us our true Velix PID and Tree ID!
    json process_obj = reply.value("process", json::object());
    velix_pid = process_obj.value("pid", -1);
    tree_id = process_obj.value("tree_id", "UNKNOWN");
    if (reply.contains("is_root") && reply["is_root"].is_boolean()) {
      is_root = reply["is_root"].get<bool>();
    }
    if (reply.contains("is_handler") && reply["is_handler"].is_boolean()) {
      is_handler = reply["is_handler"].get<bool>();
    }

    // Global limits
    max_memory_mb = reply.value("max_memory_mb", 2048);
    max_runtime_sec = reply.value("max_runtime_sec", 300);

    supervisor_socket.close();
  }

  // 2. Data Handshake: Connect and Register with Velix Bus
  {
    const int bus_port = resolve_port("BUS", 5174);
    try {
      connect_with_retries(bus_socket, "BUS", bus_port, retry_limit,
                           retry_delay);
      bus_socket.set_timeout_ms(1000);

      json bus_reg = {{"message_type", "BUS_REGISTER"},
                      {"pid", velix_pid},
                      {"tree_id", tree_id},
                      {"is_root", is_root}};
      velix::communication::send_json(bus_socket, bus_reg.dump());
      velix::communication::recv_json(bus_socket); // OK ack

      bus_listener_thread = std::thread(&VelixProcess::bus_listener_loop, this);
    } catch (const std::exception &e) {
      LOG_WARN("Failed to connect to Velix Bus: " + std::string(e.what()));
    }
  }

  status.store(ProcessStatus::RUNNING);

  LOG_INFO("Velix Node Started | PID: " + std::to_string(os_pid) + " -> " +
           std::to_string(velix_pid) + " | Role: " + role +
           (is_root ? " (ROOT)" : "") + " | Parent PID: " +
           std::to_string(parent_pid) + " | Launch Intent: " + launch_intent);

  // Spawn the detached async Kernel IO Thread
  runtime_io_thread = std::thread(&VelixProcess::run_kernel_io_loop, this);

  // Transfer thread to Developer logic (blocking is fine)
  {
    // RAII Guard: Ensure a result is reported even on crash/exit
    ResultGuard guard(this);
    try {
      this->run();
    } catch (const std::exception &e) {
      LOG_ERROR("VelixProcess Sandbox Crashed: " + std::string(e.what()));
    }
  }

  shutdown();
}

void VelixProcess::run_kernel_io_loop() {
  using namespace std::chrono;

  while (is_running && !force_terminate) {
    json heartbeat = {
        {"message_type", "HEARTBEAT"},
        {"pid", velix_pid},
        {"payload",
         {{"status",
           status.load() == ProcessStatus::RUNNING
               ? "RUNNING"
               : (status.load() == ProcessStatus::WAITING_LLM ? "WAITING_LLM"
                                                              : "STARTING")},
          {"memory_mb", static_cast<double>(get_current_memory_usage_mb())}}}};

    try {
      const int sup_port = resolve_port("SUPERVISOR", 5173);
      velix::communication::SocketWrapper hb_socket;
      hb_socket.create_tcp_socket();
      hb_socket.connect(
          velix::communication::resolve_service_host("SUPERVISOR", "127.0.0.1"),
          static_cast<uint16_t>(sup_port));
      velix::communication::send_json(hb_socket, heartbeat.dump());
      velix::communication::recv_json(
          hb_socket); // read the {"status": "ok"} acknowledgment
    } catch (...) {
      if (is_running) {
        LOG_WARN("Lost supervisor connection natively during heartbeat. Engine "
                 "terminating.");
        is_running = false;
        std::exit(1);
      }
    }

    // Sleep between heartbeats without eating CPU cycles.
    std::unique_lock<std::mutex> sleep_lock(sleep_mutex);
    const int hb_interval = velix::utils::get_config("SDK_HEARTBEAT_SEC", 5);
    sleep_cv.wait_for(sleep_lock, std::chrono::seconds(hb_interval),
                      [this] { return force_terminate.load(); });
  }

  // Final Death Rattle: Broadcast immediate KILLED/FINISHED trace so the
  // Supervisor doesn't hang.
  if (velix_pid > 0) {
    try {
      json final_heartbeat = {
          {"message_type", "HEARTBEAT"},
          {"pid", velix_pid},
          {"payload",
           {{"memory_mb", static_cast<double>(get_current_memory_usage_mb())},
            {"status", forced_by_signal.load() ? "KILLED" : "FINISHED"}}}};
      const int sup_port = resolve_port("SUPERVISOR", 5173);
      velix::communication::SocketWrapper hb_socket;
      hb_socket.create_tcp_socket();
      hb_socket.connect(
          velix::communication::resolve_service_host("SUPERVISOR", "127.0.0.1"),
          static_cast<uint16_t>(sup_port));
      velix::communication::send_json(hb_socket, final_heartbeat.dump());
    } catch (...) {
    }
  }
}

// -------------------------------------------------------------
// Call LLM Orchestration
// -------------------------------------------------------------

void VelixProcess::bus_listener_loop() {
  try {
    while (is_running && bus_socket.is_open()) {
      try {
        std::string raw = velix::communication::recv_json(bus_socket);
        json msg = json::parse(raw);

        const std::string msg_type = msg.value("message_type", "");
        if (msg_type == "IPM_PUSH" || msg_type == "CHILD_TERMINATED") {
          const std::string trace_id = msg.value("trace_id", "");
          const json payload = msg.value("payload", json::object());
          bool routed_to_rpc = false;

          if (!trace_id.empty()) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (pending_response_traces.count(trace_id) > 0) {
              response_map[trace_id] = payload;
              queue_cv.notify_all();
              routed_to_rpc = true;
            }
          }

          if (!routed_to_rpc && on_bus_event) {
            try {
              on_bus_event(msg);
            } catch (const std::exception &e) {
              LOG_WARN("on_bus_event hook failed: " + std::string(e.what()));
            } catch (...) {
              LOG_WARN("on_bus_event hook failed with unknown exception");
            }
          }
        }
      } catch (const velix::communication::SocketTimeoutException &) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      } catch (const std::exception &e) {
        const std::string err = e.what();
        if (is_transient_socket_error(err)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }
        throw;
      }
    }
  } catch (const std::exception &e) {
    LOG_WARN("Velix Bus connection lost: " + std::string(e.what()));
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      is_running = false;
      queue_cv.notify_all();
    }
  } catch (...) {
    LOG_WARN("Velix Bus connection lost due to unknown error");
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      is_running = false;
      queue_cv.notify_all();
    }
  }
}

json VelixProcess::execute_tool(const std::string &instruction,
                                const json &args) {
  return execute_tool_internal(instruction, args, std::nullopt, std::nullopt);
}

json VelixProcess::execute_tool_internal(
    const std::string &instruction, const json &args,
    const std::optional<std::string> &user_id_override,
    const std::optional<std::string> &intent_override) {
  std::string effective_user_id = user_id;
  if (is_handler && user_id_override.has_value() &&
      !user_id_override.value().empty()) {
    // Handler-only per-call override to avoid shared mutable state races.
    effective_user_id = user_id_override.value();
  }

  std::function<void(const json &)> call_on_tool_finish =
      [&](const json &tool_result) {
        if (!on_tool_finish) {
          return;
        }
        try {
          json hook_result = tool_result;
          if (!effective_user_id.empty() &&
              (!hook_result.is_object() || !hook_result.contains("user_id"))) {
            if (!hook_result.is_object()) {
              hook_result = json::object();
            }
            hook_result["user_id"] = effective_user_id;
          }
          on_tool_finish(instruction, hook_result);
        } catch (const std::exception &e) {
          LOG_WARN("on_tool_finish hook failed: " + std::string(e.what()));
        } catch (...) {
          LOG_WARN("on_tool_finish hook failed with unknown exception");
        }
      };

  std::string actual_trace;
  bool pending_trace_registered = false;
  auto clear_pending_trace = [&]() {
    if (!pending_trace_registered || actual_trace.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(queue_mutex);
    pending_response_traces.erase(actual_trace);
    pending_trace_registered = false;
  };

  try {
    status.store(ProcessStatus::RUNNING);

    if (on_tool_start) {
      try {
        json hook_args = args;
        if (!effective_user_id.empty() &&
            (!hook_args.is_object() || !hook_args.contains("user_id"))) {
          if (!hook_args.is_object()) {
            hook_args = json::object();
          }
          hook_args["user_id"] = effective_user_id;
        }
        on_tool_start(instruction, hook_args);
      } catch (const std::exception &e) {
        LOG_WARN("on_tool_start hook failed: " + std::string(e.what()));
      } catch (...) {
        LOG_WARN("on_tool_start hook failed with unknown exception");
      }
    }

    actual_trace = velix::utils::generate_uuid();
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      pending_response_traces.insert(actual_trace);
      pending_trace_registered = true;
    }

    std::string effective_intent = "JOIN_PARENT_TREE";
    if (is_handler && intent_override.has_value() &&
        is_supported_intent_value(intent_override.value())) {
      effective_intent = intent_override.value();
    }

    json launch_req = {{"message_type", "EXEC_VELIX_PROCESS"},
                       {"trace_id", actual_trace},
                       {"tree_id", tree_id},
                       {"source_pid", velix_pid},
                       {"is_handler", is_handler},
                       {"name", instruction},
                       {"params", args},
                       {"intent", effective_intent}};

    // Runtime metadata stays outside tool params.
    if (!effective_user_id.empty()) {
      launch_req["user_id"] = effective_user_id;
    }

    const int exec_port = resolve_port("EXECUTIONER", 5172);
    const int exec_timeout =
        velix::utils::get_config("SDK_EXEC_TIMEOUT_MS", 120000);
    const int exec_retry_limit =
        velix::utils::get_config("SDK_EXEC_RETRY_LIMIT", 3);
    const int exec_retry_delay =
        velix::utils::get_config("SDK_EXEC_RETRY_DELAY_MS", 300);
    const int connect_retry_limit =
        velix::utils::get_config("SDK_RETRY_LIMIT", 3);
    const int connect_retry_delay =
        velix::utils::get_config("SDK_RETRY_DELAY_MS", 500);

    std::string last_exec_error = "unknown";
    bool launch_acked = false;

    for (int attempt = 0; attempt < std::max(1, exec_retry_limit); ++attempt) {
      try {
        velix::communication::SocketWrapper exec_socket;
        connect_with_retries(exec_socket, "EXECUTIONER", exec_port,
                             connect_retry_limit, connect_retry_delay);

        velix::communication::send_json(exec_socket, launch_req.dump());
        json ack =
            receive_executioner_ack(exec_socket, exec_timeout, is_running);

        if (ack.value("status", "") == "ok") {
          launch_acked = true;
          break;
        }

        const std::string launcher_error =
            ack.value("message", ack.value("error", "unknown rejection"));
        last_exec_error = "Velix Launcher Failure: " + launcher_error;

        const bool retryable_busy =
            launcher_error.find("busy") != std::string::npos;
        if (retryable_busy && attempt + 1 < exec_retry_limit) {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(exec_retry_delay * (attempt + 1)));
          continue;
        }

        throw std::runtime_error(last_exec_error);
      } catch (const std::exception &e) {
        last_exec_error = e.what();
        const bool retryable =
            is_transient_socket_error(last_exec_error) ||
            last_exec_error.find("executioner ack deadline exceeded") !=
                std::string::npos ||
            last_exec_error.find("busy") != std::string::npos;

        if (attempt + 1 < exec_retry_limit && retryable) {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(exec_retry_delay * (attempt + 1)));
          continue;
        }
        break;
      }
    }

    if (!launch_acked) {
      if (last_exec_error.find("Velix Launcher Failure") != std::string::npos) {
        throw std::runtime_error(last_exec_error);
      }
      throw std::runtime_error("Velix Executioner Link Failed: " +
                               last_exec_error);
    }

    // Phase 2: Reactive Wait on the Velix Bus for the actual skill output
    json result;
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      const int bus_wait = velix::utils::get_config("SDK_BUS_WAIT_MIN", 60);
      bool success =
          queue_cv.wait_for(lock, std::chrono::minutes(bus_wait), [&] {
            return response_map.count(actual_trace) > 0;
          });

      if (!success) {
        pending_response_traces.erase(actual_trace);
        pending_trace_registered = false;
        throw std::runtime_error("Velix Package Timeout: " + instruction +
                                 " exceeded reactive limit of " +
                                 std::to_string(bus_wait) + " mins");
      }

      result = response_map[actual_trace];
      response_map.erase(actual_trace);
      pending_response_traces.erase(actual_trace);
      pending_trace_registered = false;

      // Reactive Fail-Fast for Terminal Kernel Events (Crashes/Killed)
      if (result.value("status", "") == "error" &&
          result.value("error", "") == "child_terminated") {
        const std::string reason =
            result.value("reason", "unknown_termination");
        throw std::runtime_error(
            "Velix Tool Crash: " + instruction +
            " terminated by Supervisor (Reason: " + reason + ")");
      }
    }

    call_on_tool_finish(result);

    return result;
  } catch (const std::exception &e) {
    status.store(ProcessStatus::ERROR);
    clear_pending_trace();
    json error_payload = {{"status", "error"}, {"message", e.what()}};
    call_on_tool_finish(error_payload);
    throw;
  }
}

void VelixProcess::report_result(int target_pid, const json &data,
                                 const std::string &trace_id, bool append) {
  if (!bus_socket.is_open())
    return;

  json relay = {{"message_type", "IPM_RELAY"},
                {"target_pid", target_pid},
                {"trace_id", trace_id},
                {"payload", data}};

  {
    std::lock_guard<std::mutex> lock(bus_mutex);
    velix::communication::send_json(bus_socket, relay.dump());
  }

  if (append) {
    // Normal path: wake the waiting execute_tool_internal() so the LLM loop
    // can continue with this result appended to the conversation.
    result_reported = true;
  } else {
    // Async path: the tool is reporting out-of-band. Remove the trace from the
    // pending set so execute_tool_internal() does NOT resume waiting for it.
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (!trace_id.empty()) {
      pending_response_traces.erase(trace_id);
      response_map.erase(trace_id);
    }
  }
}

void VelixProcess::send_message(int target_pid, const std::string &purpose,
                                const json &payload) {
  if (!bus_socket.is_open())
    return;

  json relay = {{"message_type", "IPM_RELAY"}};
  relay["target_pid"] = target_pid;
  relay["purpose"] = purpose;
  relay["user_id"] = user_id;
  relay["payload"] = payload;

  std::lock_guard<std::mutex> lock(bus_mutex);
  velix::communication::send_json(bus_socket, relay.dump());
}

std::string VelixProcess::call_llm(const std::string &convo_id,
                                   const std::string &user_message,
                                   const std::string &system_message,
                                   const std::string &user_id,
                                   const std::string &mode) {
  return call_llm_internal(convo_id, user_message, system_message, user_id,
                           mode, false, nullptr, std::nullopt);
}

std::string VelixProcess::call_llm_stream(
    const std::string &convo_id, const std::string &user_message,
    const std::function<void(const std::string &)> &on_token,
    const std::string &system_message, const std::string &user_id,
    const std::string &mode) {
  return call_llm_internal(convo_id, user_message, system_message, user_id,
                           mode, true, on_token, std::nullopt);
}

std::string VelixProcess::call_llm_resume(
    const std::string &convo_id, const json &tool_result,
    const std::string &user_id,
    const std::function<void(const std::string &)> &on_token) {
  return call_llm_internal(convo_id, "", "", user_id, "", true, on_token,
                           std::nullopt, tool_result);
}

std::string VelixProcess::call_llm_internal(
    const std::string &convo_id, const std::string &user_message,
    const std::string &system_message, const std::string &user_id,
    const std::string &mode, bool stream_requested,
    const std::function<void(const std::string &)> &on_token,
    const std::optional<std::string> &intent_override,
    const std::optional<json> &tool_result_override) {
  status.store(ProcessStatus::WAITING_LLM);

  // Resolve per-call user context without mutating shared process state.
  const std::string effective_user_id =
      user_id.empty() ? this->user_id : user_id;
  const std::optional<std::string> effective_intent_override =
      (is_handler && intent_override.has_value() &&
       is_supported_intent_value(intent_override.value()))
          ? intent_override
          : std::nullopt;

  const std::string effective_mode =
      resolve_effective_mode(mode, convo_id, effective_user_id, is_handler);

  std::string active_convo_id = convo_id;
  json base_payload = {{"mode", effective_mode},
                       {"owner_pid", velix_pid},
                       {"stream", stream_requested}};

  if (effective_mode != "simple") {
    base_payload["user_id"] = effective_user_id;
  }

  std::string pending_user_message = user_message;
  std::string pending_system_message = system_message;
  json pending_tool_messages = json::array();
  if (tool_result_override.has_value()) {
    if (tool_result_override.value().is_array()) {
      pending_tool_messages = tool_result_override.value();
    } else {
      pending_tool_messages.push_back(tool_result_override.value());
    }
  }

  auto dispatch_llm_request = [&](const json &request_payload,
                                  bool request_streaming) -> std::string {
    const std::string trace_id = velix::utils::generate_uuid();
    const int sched_port = resolve_port("SCHEDULER", 5171);
    velix::communication::SocketWrapper scheduler_socket;
    const int retry_limit = velix::utils::get_config("SDK_RETRY_LIMIT", 3);
    const int retry_delay = velix::utils::get_config("SDK_RETRY_DELAY_MS", 500);
    try {
      connect_with_retries(scheduler_socket, "SCHEDULER", sched_port,
                           retry_limit, retry_delay);
    } catch (...) {
      throw std::runtime_error("VelixProcess SDK: Failed to connect to "
                               "scheduler after retry attempts.");
    }

    json envelope = request_payload;
    envelope["message_type"] = "LLM_REQUEST";
    envelope["request_id"] = "req_" + std::to_string(velix_pid) + "_" +
                             velix::utils::generate_uuid().substr(0, 8);
    envelope["trace_id"] = trace_id;
    envelope["tree_id"] = tree_id;
    envelope["source_pid"] = velix_pid;
    envelope["priority"] = envelope.value("priority", 1);

    const int llm_timeout =
        velix::utils::get_config("SDK_LLM_TIMEOUT_MS", 305000);
    // Use a short per-recv poll timeout so a stalled chunk is retried quickly.
    // The stream_deadline inside receive_scheduler_response() enforces the
    // overall wall-clock cap.
    const int stream_poll_timeout_ms =
        velix::utils::get_config("SDK_STREAM_POLL_TIMEOUT_MS", 30000);
    scheduler_socket.set_timeout_ms(stream_poll_timeout_ms);
    velix::communication::send_json(scheduler_socket, envelope.dump());

    return receive_scheduler_response(scheduler_socket, request_streaming,
                                      llm_timeout, is_running, on_token);
  };

  int loop_count = 0;
  while (is_running && loop_count < 10) {
    json payload = base_payload;
    payload["convo_id"] = active_convo_id;

    if (!pending_system_message.empty()) {
      payload["system_message"] = pending_system_message;
    }
    if (!pending_user_message.empty()) {
      payload["user_message"] = pending_user_message;
    }

    if (pending_tool_messages.is_array() && !pending_tool_messages.empty()) {
      // Honour the caller's stream flag on tool-resume turns so the next
      // assistant reply still streams token-by-token instead of being
      // buffered and dumped all at once after the full reply completes.
      payload["stream"] = stream_requested;
      if (pending_tool_messages.size() == 1) {
        payload["tool_message"] = pending_tool_messages[0];
      } else {
        payload["tool_messages"] = pending_tool_messages;
      }
    } else {
      payload["stream"] = stream_requested;
    }

    pending_system_message.clear();
    pending_user_message.clear();
    pending_tool_messages = json::array();

    const bool request_streaming = payload.value("stream", false);
    std::string raw_reply = dispatch_llm_request(payload, request_streaming);
    json reply = json::parse(raw_reply);

    if (reply.value("status", "error") != "ok") {
      status.store(ProcessStatus::ERROR);
      throw std::runtime_error("Scheduler rejected: " +
                               reply.value("error", ""));
    }

    if (reply.contains("convo_id") && reply["convo_id"].is_string()) {
      const std::string returned_convo_id =
          reply["convo_id"].get<std::string>();
      if (!returned_convo_id.empty()) {
        active_convo_id = returned_convo_id;
      }
    }

    const std::string response_text = reply.value("response", "");

    const json normalized_tool_calls =
        (reply.contains("tool_calls") && reply["tool_calls"].is_array())
            ? reply["tool_calls"]
            : json::array();

    if (normalized_tool_calls.empty()) {
      status.store(ProcessStatus::RUNNING);
      return response_text;
    }

    if (!reply.contains("assistant_message") ||
        !reply["assistant_message"].is_object()) {
      status.store(ProcessStatus::ERROR);
      throw std::runtime_error(
          "Scheduler protocol violation: missing assistant_message object");
    }

    status.store(ProcessStatus::RUNNING);
    std::vector<std::future<json>> tool_futures;
    tool_futures.reserve(normalized_tool_calls.size());

    for (const auto &tool_call : normalized_tool_calls) {
      tool_futures.push_back(
          std::async(std::launch::async, [this, tool_call, effective_user_id,
                                          effective_intent_override]() {
            if (!tool_call.is_object()) {
              throw std::runtime_error("Malformed tool_call: expected object");
            }

            const json fn = tool_call.value("function", json::object());
            const std::string tool_name = fn.value("name", std::string(""));
            if (tool_name.empty()) {
              throw std::runtime_error(
                  "Malformed tool_call: missing function.name");
            }

            const json tool_args = fn.value("arguments", json::object());
            const std::string tool_call_id =
                tool_call.value("id", std::string(""));
            if (tool_call_id.empty()) {
              throw std::runtime_error("Malformed tool_call: missing id");
            }

            json tool_res;
            try {
              tool_res = execute_tool_internal(
                  tool_name, tool_args,
                  effective_user_id.empty()
                      ? std::optional<std::string>{}
                      : std::optional<std::string>{effective_user_id},
                  effective_intent_override);
            } catch (const std::exception &e) {
              tool_res = json{{"status", "error"},
                              {"error", "tool_execution_failed"},
                              {"message", e.what()},
                              {"tool", tool_name}};
            }

            return json{{"role", "tool"},
                        {"tool_call_id", tool_call_id},
                        {"content", tool_res.dump()}};
          }));
    }

    json tool_messages = json::array();
    for (auto &future : tool_futures) {
      try {
        tool_messages.push_back(future.get());
      } catch (const std::runtime_error &) {
        status.store(ProcessStatus::ERROR);
        throw;
      } catch (const std::exception &e) {
        LOG_WARN("Failed to execute structured tool call: " +
                 std::string(e.what()));
      }
    }

    if (tool_messages.empty()) {
      status.store(ProcessStatus::RUNNING);
      return response_text;
    }

    pending_tool_messages = tool_messages;

    loop_count++;
    status.store(ProcessStatus::WAITING_LLM);
  }

  status.store(ProcessStatus::ERROR);
  return "Failure: Agent state machine exceeded max 10 iterations.";
}

} // namespace core
} // namespace velix