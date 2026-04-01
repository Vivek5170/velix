#include "velix_process.hpp"

#include "../../../communication/network_config.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "../../../utils/config_utils.hpp"
#include "../../../utils/logger.hpp"
#include "../../../utils/string_utils.hpp"

#ifdef _WIN32
#include <psapi.h>
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace velix {
namespace core {

VelixProcess *VelixProcess::instance_ = nullptr;

#ifdef _WIN32
BOOL WINAPI os_signal_handler(DWORD fdwCtrlType) {
  if (fdwCtrlType == CTRL_C_EVENT || fdwCtrlType == CTRL_CLOSE_EVENT ||
      fdwCtrlType == CTRL_BREAK_EVENT) {
    if (VelixProcess::instance_) {
      VelixProcess::instance_->shutdown();
    }
    return TRUE;
  }
  return FALSE;
}
#else
void posix_signal_handler(int /*signum*/) {
  if (VelixProcess::instance_) {
    // Graceful flip
    VelixProcess::instance_->shutdown();
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

VelixProcess::~VelixProcess() {
  shutdown();
  VelixProcess::instance_ = nullptr;
}

void VelixProcess::shutdown() {
  if (is_running.load()) {
    is_running.store(false);
    force_terminate.store(true);

    // Wake up the idle IO thread immediately so it can send the final heartbeat
    sleep_cv.notify_all();
    queue_cv.notify_all(); // Wake up any threads waiting for tool results

    if (runtime_io_thread.joinable()) {
      runtime_io_thread.join();
    }

    if (bus_socket.is_open()) {
      bus_socket.close();
    }
    if (bus_listener_thread.joinable()) {
      bus_listener_thread.join();
    }
  }
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

  // Set the global instance for signal handling
  instance_ = this;
#ifdef _WIN32
  SetConsoleCtrlHandler(os_signal_handler, TRUE);
#else
  signal(SIGTERM, posix_signal_handler);
  signal(SIGINT, posix_signal_handler);
#endif

  // Discover context from Environment (Injected by Executioner)
  char *env_parent_pid = std::getenv("VELIX_PARENT_PID");
  if (env_parent_pid) {
    parent_pid = std::atoi(env_parent_pid);
  }
  is_root = (parent_pid <= 0);

  char *env_trace_id = std::getenv("VELIX_TRACE_ID");
  if (env_trace_id) {
    entry_trace_id = env_trace_id;
  }

  char *env_params = std::getenv("VELIX_PARAMS");
  if (env_params) {
    try {
      params = json::parse(env_params);
    } catch (...) {
      params = json::object();
    }
  }

  const int sup_port = resolve_port("SUPERVISOR", 5173);
  bool connected = false;
  const int retry_limit = velix::utils::get_config("SDK_RETRY_LIMIT", 3);
  const int retry_delay = velix::utils::get_config("SDK_RETRY_DELAY_MS", 500);

  for (int i = 0; i < retry_limit; ++i) {
    try {
      supervisor_socket.create_tcp_socket();
        supervisor_socket.connect(
          velix::communication::resolve_service_host("SUPERVISOR", "127.0.0.1"),
          static_cast<uint16_t>(sup_port));
      connected = true;
      break;
    } catch (...) {
      if (i == retry_limit - 1)
        throw;
      std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay * (i + 1)));
    }
  }

  if (!connected) {
    throw std::runtime_error(
        "VelixProcess SDK: Failed to connect to supervisor after attempts.");
  }

  // Register the agent explicitly with the OS kernel matching exact schema
  json payload = {
      {"register_intent", is_root ? "NEW_TREE" : "JOIN_PARENT_TREE"},
      {"role", role},
      {"os_pid", os_pid},
      {"process_name", process_name},
      {"trace_id", entry_trace_id},
      {"status", "STARTING"},
      {"memory_mb", static_cast<double>(get_current_memory_usage_mb())}};

  json reg_msg = {{"message_type", "REGISTER_PID"}, {"payload", payload}};
  if (!is_root) {
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

    // Global limits
    max_memory_mb = reply.value("max_memory_mb", 2048);
    max_runtime_sec = reply.value("max_runtime_sec", 300);

    supervisor_socket.close();
  }

  // 2. Data Handshake: Connect and Register with Velix Bus
  {
    const int bus_port = resolve_port("BUS", 5174);
    try {
      bus_socket.create_tcp_socket();
        bus_socket.connect(
          velix::communication::resolve_service_host("BUS", "127.0.0.1"),
          static_cast<uint16_t>(bus_port));
      bus_socket.set_timeout_ms(1000);

      json bus_reg = {{"message_type", "BUS_REGISTER"}, {"pid", velix_pid}};
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
           (is_root ? " (ROOT)" : ""));

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
      // The guard will still fire with "completed" or we could refine its
      // reason here
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
  if (velix_pid > 0 && !force_terminate.load()) {
    try {
      json final_heartbeat = {
          {"message_type", "HEARTBEAT"},
          {"pid", velix_pid},
          {"payload",
           {{"memory_mb", static_cast<double>(get_current_memory_usage_mb())},
            {"status", force_terminate.load() ? "KILLED" : "FINISHED"}}}};
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

std::string
VelixProcess::send_llm_request_stateless(const json &request_payload) {
  // 1. Generate unique Trace ID for this specific network attempt
  const std::string trace_id = velix::utils::generate_uuid();
  const int sched_port = resolve_port("SCHEDULER", 5171);

  velix::communication::SocketWrapper scheduler_socket;

  // 2. Robust Connection Retry Bridge
  bool connected = false;
  const int retry_limit = velix::utils::get_config("SDK_RETRY_LIMIT", 3);
  const int retry_delay = velix::utils::get_config("SDK_RETRY_DELAY_MS", 500);

  for (int i = 0; i < retry_limit; ++i) {
    try {
      scheduler_socket.create_tcp_socket();
        scheduler_socket.connect(
          velix::communication::resolve_service_host("SCHEDULER", "127.0.0.1"),
          static_cast<uint16_t>(sched_port));
      connected = true;
      break;
    } catch (...) {
      if (i == retry_limit - 1)
        throw std::runtime_error("VelixProcess SDK: Failed to connect to "
                                 "scheduler after retry attempts.");
      std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay * (i + 1)));
    }
  }

  // 3. Build Protocol Envelope matching schema/llm_request.json
  json envelope = request_payload;
  envelope["message_type"] = "LLM_REQUEST";
  envelope["request_id"] = "req_" + std::to_string(velix_pid) + "_" +
                           velix::utils::generate_uuid().substr(0, 8);
  envelope["trace_id"] = trace_id;
  envelope["tree_id"] = tree_id;
  envelope["source_pid"] = velix_pid;
  envelope["priority"] = envelope.value("priority", 1);
  envelope["mode"] = envelope.value("mode", "simple");

  // 4. Dispatch and Wait
  const int llm_timeout = velix::utils::get_config("SDK_LLM_TIMEOUT_MS", 120000);
  scheduler_socket.set_timeout_ms(llm_timeout);
  velix::communication::send_json(scheduler_socket, envelope.dump());

  return velix::communication::recv_json(scheduler_socket);
}

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

          if (!trace_id.empty()) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            response_map[trace_id] = payload;
            queue_cv.notify_all();
          }
        }
      } catch (const std::exception &e) {
        const std::string err = e.what();
        // Socket timeout: keep looping so shutdown can flip is_running.
        if (err.find("errno 11") != std::string::npos ||
            err.find("errno 35") != std::string::npos ||
            err.find("timed out") != std::string::npos ||
            err.find("timeout") != std::string::npos) {
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

json VelixProcess::exec_velix_process(const std::string &name,
                                      const json &params,
                                      const std::string &trace_id) {
  const std::string actual_trace =
      trace_id.empty() ? velix::utils::generate_uuid() : trace_id;

  json launch_req = {{"message_type", "EXEC_VELIX_PROCESS"},
                     {"trace_id", actual_trace},
                     {"tree_id", tree_id},
                     {"source_pid", velix_pid},
                     {"name", name},
                     {"params", params}};

  const int exec_port = resolve_port("EXECUTIONER", 5172);
  velix::communication::SocketWrapper exec_socket;
  try {
    exec_socket.create_tcp_socket();
    exec_socket.connect(
      velix::communication::resolve_service_host("EXECUTIONER", "127.0.0.1"),
      static_cast<uint16_t>(exec_port));
    const int exec_timeout = velix::utils::get_config("SDK_EXEC_TIMEOUT_MS", 5000);
    exec_socket.set_timeout_ms(exec_timeout);
    velix::communication::send_json(exec_socket, launch_req.dump());

    // Phase 1: Wait for immediate launcher "ok"
    const std::string raw = velix::communication::recv_json(exec_socket);
    json ack = json::parse(raw);
    if (ack.value("status", "") != "ok") {
      throw std::runtime_error(
          "Velix Launcher Failure: " +
          ack.value("message", ack.value("error", "unknown rejection")));
    }
  } catch (const std::exception &e) {
    if (std::string(e.what()).find("Velix Launcher Failure") !=
        std::string::npos)
      throw;
    throw std::runtime_error("Velix Executioner Link Failed: " +
                             std::string(e.what()));
  }

  // Phase 2: Reactive Wait on the Velix Bus for the actual skill output
  json result;
  {
    std::unique_lock<std::mutex> lock(queue_mutex);
    const int bus_wait = velix::utils::get_config("SDK_BUS_WAIT_MIN", 60);
    bool success = queue_cv.wait_for(lock, std::chrono::minutes(bus_wait), [&] {
      return response_map.count(actual_trace) > 0;
    });

    if (!success) {
      throw std::runtime_error("Velix Package Timeout: " + name +
                               " exceeded reactive limit of " + std::to_string(bus_wait) + " mins");
    }

    result = response_map[actual_trace];
    response_map.erase(actual_trace);

    // Reactive Fail-Fast for Terminal Kernel Events (Crashes/Killed)
    if (result.value("status", "") == "error" &&
        result.value("error", "") == "child_terminated") {
      const std::string reason = result.value("reason", "unknown_termination");
      throw std::runtime_error("Velix Tool Crash: " + name +
                               " terminated by Supervisor (Reason: " + reason +
                               ")");
    }
  }
  return result;
}

void VelixProcess::report_result(int target_pid, const json &data,
                                 const std::string &trace_id) {
  if (!bus_socket.is_open())
    return;

  json relay = {{"message_type", "IPM_RELAY"},
                {"target_pid", target_pid},
                {"trace_id", trace_id},
                {"payload", data}};

  std::lock_guard<std::mutex> lock(bus_mutex);
  velix::communication::send_json(bus_socket, relay.dump());

  // Mark as reported to prevent the RAII guard from double-reporting on exit
  result_reported = true;
}

std::string VelixProcess::call_llm(const std::string &convo_id,
                                   const std::string &user_message,
                                   const std::string &system_message) {
  status.store(ProcessStatus::WAITING_LLM);

  json payload = {{"mode", "conversation"},
                  {"convo_id", convo_id},
                  {"owner_pid", velix_pid}};

  if (!user_message.empty() || !system_message.empty()) {
    json messages = json::array();
    if (!system_message.empty())
      messages.push_back({{"role", "system"}, {"content", system_message}});
    if (!user_message.empty())
      messages.push_back({{"role", "user"}, {"content", user_message}});
    payload["messages"] = messages;
  }

  int loop_count = 0;
  while (is_running && loop_count < 10) {
    std::string raw_reply = send_llm_request_stateless(payload);
    json reply = json::parse(raw_reply);

    if (reply.value("status", "error") != "ok") {
      status.store(ProcessStatus::ERROR);
      throw std::runtime_error("Scheduler rejected: " +
                               reply.value("error", ""));
    }

    if (!reply.value("exec_required", false)) {
      status.store(ProcessStatus::RUNNING);
      return reply.value("response", "");
    }

    std::string response_text = reply.value("response", "");
    const std::string trace_id = reply.value("trace_id", "");
    bool tool_executed = false;

    // 🚨 NATIVE TAG-BASED TOOL EXTRACTION (EXEC ... END_EXEC)
    // Optimized for small local LLMs that embed tool use in text.
    size_t start_pos = response_text.find("EXEC");
    while (start_pos != std::string::npos) {
      size_t end_pos = response_text.find("END_EXEC", start_pos);
      if (end_pos != std::string::npos) {
        std::string tool_json_str =
            response_text.substr(start_pos + 4, end_pos - (start_pos + 4));

        try {
          json tool_call = json::parse(tool_json_str);
          std::string tool_name = tool_call.value("name", "");
          json tool_args = tool_call.value("arguments", json::object());

          if (!tool_name.empty()) {
            status.store(ProcessStatus::RUNNING);
            json tool_res = exec_velix_process(tool_name, tool_args, trace_id);
            tool_executed = true;

            payload["messages"].push_back({{"role", "tool"},
                                           {"content", tool_res.dump()},
                                           {"tool_call_id", trace_id}});
          }
        } catch (const std::runtime_error &re) {
          status.store(ProcessStatus::ERROR);
          throw; // propagate crash
        } catch (const std::exception &e) {
          LOG_WARN("Failed to execute tool between EXEC tags: " +
                   std::string(e.what()));
        }

        start_pos = response_text.find("EXEC", end_pos);
      } else {
        break;
      }
    }

    if (reply.contains("tool_calls") && reply["tool_calls"].is_array()) {
      for (const auto &call : reply["tool_calls"]) {
        std::string tool_name = call.value("name", "");
        json tool_args = call.value("arguments", json::object());
        if (!tool_name.empty()) {
          try {
            json tool_res = exec_velix_process(tool_name, tool_args, trace_id);
            tool_executed = true;

            if (!payload.contains("messages"))
              payload["messages"] = json::array();
            payload["messages"].push_back({{"role", "tool"},
                                           {"content", tool_res.dump()},
                                           {"tool_call_id", trace_id}});
          } catch (const std::runtime_error &re) {
            status.store(ProcessStatus::ERROR);
            throw; // propagate crash
          } catch (const std::exception &e) {
            LOG_WARN("Failed to execute structured tool call: " +
                     std::string(e.what()));
          }
        }
      }
    }

    if (!tool_executed) {
      status.store(ProcessStatus::RUNNING);
      return response_text;
    }

    payload.erase("messages");
    loop_count++;
    status.store(ProcessStatus::WAITING_LLM);
  }

  status.store(ProcessStatus::ERROR);
  return "Failure: Agent state machine exceeded max 10 iterations.";
}

json VelixProcess::execute_tool(const std::string &instruction,
                                const json &args) {
  try {
    status.store(ProcessStatus::RUNNING);
    return exec_velix_process(instruction, args);
  } catch (const std::exception &e) {
    status.store(ProcessStatus::ERROR);
    throw; // propagate fatal kernel/lifecycle error
  }
}

} // namespace core
} // namespace velix
