#include "velix_process.hpp"

#include "../utils/logger.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#include <psapi.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#include <csignal>
#endif

namespace velix {
namespace core {

VelixProcess* VelixProcess::instance_ = nullptr;

#ifdef _WIN32
BOOL WINAPI os_signal_handler(DWORD fdwCtrlType) {
  if (fdwCtrlType == CTRL_C_EVENT || fdwCtrlType == CTRL_CLOSE_EVENT || fdwCtrlType == CTRL_BREAK_EVENT) {
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

    if (runtime_io_thread.joinable()) {
      runtime_io_thread.join();
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
  try {
    std::ifstream file("config/ports.json");
    if (file.is_open()) {
      json ports;
      file >> ports;
      return ports.value(service_name, fallback);
    }
  } catch (...) {
  }
  return fallback;
}

void VelixProcess::start(int override_pid,
                         const std::string & /*parent_tree_id*/) {
  int source_pid = override_pid;
  std::string intent = (source_pid > 0) ? "JOIN_PARENT_TREE" : "NEW_TREE";

  supervisor_socket.create_tcp_socket();
  try {
    const int sup_port = resolve_port("SUPERVISOR", 5173);
    supervisor_socket.connect("127.0.0.1", static_cast<uint16_t>(sup_port));
  } catch (const std::exception &e) {
    LOG_ERROR(
        "VelixProcess SDK: Failed to connect outbound supervisor socket: " +
        std::string(e.what()));
    throw;
  }

  // Register the agent explicitly with the OS kernel matching exact schema
  json payload = {
      {"register_intent", intent},
      {"role", role},
      {"os_pid", os_pid},
      {"process_name", process_name},
      {"control_port", -1}, // Abandoned via 2-thread persistent socket arch
      {"status", "STARTING"},
      {"memory_mb", get_current_memory_usage_mb()}};

  json reg_msg = {{"message_type", "REGISTER_PID"}, {"payload", payload}};

  if (source_pid > 0) {
    reg_msg["source_pid"] = source_pid;
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

    // Explicitly disconnect the handshake socket because our Architecture is
    // strictly stateless HTTP-style JSON over TCP.
    supervisor_socket.close();
  }

  status.store(ProcessStatus::RUNNING);
  is_running = true;

  LOG_INFO("VelixProcess SDK Kernel Registered. PID: " +
           std::to_string(velix_pid) + " | Role: " + role);

  // Spawn the detached async Kernel IO Thread
  runtime_io_thread = std::thread(&VelixProcess::run_kernel_io_loop, this);

  // Transfer thread to Developer logic (blocking is fine)
  try {
    this->run();
  } catch (const std::exception &e) {
    LOG_ERROR("VelixProcess Sandbox Crashed: " + std::string(e.what()));
  }

  shutdown();
}

void VelixProcess::run_kernel_io_loop() {
  using namespace std::chrono;

  while (is_running && !force_terminate) {
    // Dispatch the Periodic 5s Synchronous Status + Memory Heartbeat
    // json heartbeat = {
    //     {"message_type", "HEARTBEAT"},
    //     {"pid", velix_pid},
    //     {"memory_mb", get_current_memory_usage_mb()},
    //     {"status", status.load() == ProcessStatus::RUNNING ? "RUNNING" :
    //     (status.load() == ProcessStatus::WAITING_LLM ? "WAITING_LLM" :
    //     "STARTING")}
    // };

    json heartbeat = {
        {"message_type", "HEARTBEAT"},
        {"pid", velix_pid},
        {"payload",
         {{"status",
           status.load() == ProcessStatus::RUNNING
               ? "RUNNING"
               : (status.load() == ProcessStatus::WAITING_LLM ? "WAITING_LLM"
                                                              : "STARTING")},
          {"memory_mb", get_current_memory_usage_mb()}}}};

    try {
      const int sup_port = resolve_port("SUPERVISOR", 5173);
      velix::communication::SocketWrapper hb_socket;
      hb_socket.create_tcp_socket();
      hb_socket.connect("127.0.0.1", static_cast<uint16_t>(sup_port));
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

    // Sleep for 5 seconds between heartbeats without eating CPU cycles, 
    // instantly waking if the OS SIGTERM trips the CV in shutdown().
    std::unique_lock<std::mutex> sleep_lock(sleep_mutex);
    sleep_cv.wait_for(sleep_lock, std::chrono::seconds(5), [this] { return force_terminate.load(); });
  }

  // Final Death Rattle: Broadcast immediate KILLED/FINISHED trace so the Supervisor doesn't hang.
  if (velix_pid > 0) {
    try {
      json final_heartbeat = {
          {"message_type", "HEARTBEAT"},
          {"pid", velix_pid},
          {"memory_mb", get_current_memory_usage_mb()},
          {"status", force_terminate.load() ? "KILLED" : "FINISHED"}
      };
      const int sup_port = resolve_port("SUPERVISOR", 5173);
      velix::communication::SocketWrapper hb_socket;
      hb_socket.create_tcp_socket();
      hb_socket.connect("127.0.0.1", static_cast<uint16_t>(sup_port));
      velix::communication::send_json(hb_socket, final_heartbeat.dump());
    } catch (...) {}
  }
}

// -------------------------------------------------------------
// Call LLM Orchestration
// -------------------------------------------------------------

std::string
VelixProcess::send_llm_request_stateless(const json &request_payload) {
  json envelope = {{"message_type", "LLM_REQUEST"},
                   {"request_id", "req_" + std::to_string(velix_pid) + "_" +
                                      std::to_string(std::random_device{}())},
                   {"tree_id", tree_id},
                   {"source_pid", velix_pid},
                   {"priority", 1},
                   {"payload", request_payload}};

  // Agent directly hits the stateless GPU boundary
  const int sched_port = resolve_port("SCHEDULER", 5171);
  velix::communication::SocketWrapper scheduler_socket;
  scheduler_socket.create_tcp_socket();
  scheduler_socket.connect("127.0.0.1", static_cast<uint16_t>(sched_port));
  scheduler_socket.set_timeout_ms(120000); // Massive 2 min inference timeout
  velix::communication::send_json(scheduler_socket, envelope.dump());

  const std::string raw = velix::communication::recv_json(scheduler_socket);
  return raw;
}

json VelixProcess::send_executioner_request(const std::string &instruction,
                                            const json &args) {
  json exec_req = {
      {"message_type", "EXEC_REQUEST"},
      {"tree_id", tree_id},
      {"source_pid", velix_pid},
      {"instruction", instruction},
      {"payload", args} // typically { "exec_blocks": ["..."] }
  };

  // Agent hits the Sandbox boundary directly
  const int exec_port = resolve_port("EXECUTIONER", 5172);
  velix::communication::SocketWrapper exec_socket;
  exec_socket.create_tcp_socket();
  exec_socket.connect("127.0.0.1", static_cast<uint16_t>(exec_port));
  exec_socket.set_timeout_ms(30000); // 30 sec default tool timeout
  velix::communication::send_json(exec_socket, exec_req.dump());

  const std::string raw = velix::communication::recv_json(exec_socket);
  return json::parse(raw);
}

std::string VelixProcess::call_llm(const std::string &convo_id,
                                   const std::string &user_message,
                                   const std::string &system_message) {
  status.store(ProcessStatus::WAITING_LLM);

  json payload = {{"mode", "conversation"},
                  {"convo_id", convo_id},
                  {"owner_pid", velix_pid}};
  // Wait! The user asked `call_llm` to take `messages` directly. Let's adapt
  // it.

  if (!user_message.empty() || !system_message.empty()) {
    json messages = json::array();
    if (!system_message.empty())
      messages.push_back({{"role", "system"}, {"content", system_message}});
    if (!user_message.empty())
      messages.push_back({{"role", "user"}, {"content", user_message}});
    payload["messages"] = messages;
  }

  // The Infinite Action Loop Native State Machine
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
      // Final Text Generated Correctly by LLM
      status.store(ProcessStatus::RUNNING);
      return reply.value("response", "");
    }

    // -------------------------------------------------------------------
    // 🚨 NATIVE REACT AUTONOMOUS TOOL EXECUTION
    // The developer's App thread blocks while the SDK resolves tools
    // automatically.
    // -------------------------------------------------------------------
    status.store(ProcessStatus::RUNNING); // Waiting for executioner

    std::vector<std::string> executed_tool_outputs;
    json exec_blocks = reply["exec_blocks"];

    // Tell Executioner to parse and launch these native process plugins
    json exec_args = {{"exec_blocks", exec_blocks}, {"convo_id", convo_id}};
    try {
      json tool_result = send_executioner_request("batch_execute", exec_args);

      // Loop and hit LLM again! The tool payload sent the tool output
      // string into the exact same persistent log memory location
      // automatically!
      payload.erase("messages"); // Don't resend user payload! Just poll
                                 // conversation again.
      loop_count++;
    } catch (const std::exception &e) {
      LOG_WARN("Agent Tool Execution loop crashed: " + std::string(e.what()));
      break;
    }
    status.store(ProcessStatus::WAITING_LLM);
  }

  status.store(ProcessStatus::ERROR);
  return "Failure: Agent state machine exceeded max 10 iterations without "
         "generating response.";
}

json VelixProcess::execute_tool(const std::string &instruction,
                                const json &args) {
  status.store(ProcessStatus::RUNNING);
  return send_executioner_request(instruction, args);
}

} // namespace core
} // namespace velix
