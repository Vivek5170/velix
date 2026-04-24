#ifndef VELIX_PROCESS_HPP
#define VELIX_PROCESS_HPP

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "../../../communication/socket_wrapper.hpp"
#include "../../../communication/json_include.hpp"

using json = nlohmann::json;

namespace velix::core {

enum class ProcessStatus { STARTING, RUNNING, WAITING_LLM, FINISHED, ERROR };

class VelixProcess {
public:
  VelixProcess(std::string name, std::string role);

  // Global singleton pointer for OS Signal Handlers
  static VelixProcess *instance_;

  virtual ~VelixProcess() noexcept;

  // The single entrypoint for the Agent Developer to override
  virtual void run() = 0;

  // --- Creator-Facing SDK Hooks (Synchronous Abstraction) ---
  // These are the primary APIs a process creator should use in run().
  // They may throw std::runtime_error on transport/protocol failures.
  // Catch exceptions in your run() only if you can recover locally.
  // Otherwise let them propagate so the Supervisor receives accurate state.

  // Handles the requester-side LLM -> tool_calls -> LLM loop synchronously.
  // Universal protocol contract (for all SDK languages):
  // - Scheduler input envelope:   message_type="LLM_REQUEST"
  // - Optional stream chunks:     message_type="LLM_STREAM_CHUNK", delta=string
  // - Final scheduler envelope:   message_type="LLM_RESPONSE"
  // - Final payload fields used by SDK: response, tool_calls, assistant_message
  // - SDK must append ToolResultMessage objects:
  //   {"role":"tool","tool_call_id":string,"content":string}
  //
  // Scheduler is source-of-truth for assistant_message and tool_call shape.
  // Throws on scheduler rejection, timeout, malformed responses, or tool crash
  // propagation.
  std::string call_llm(const std::string &convo_id,
                       const std::string &user_message = "",
                       const std::string &system_message = "",
                       const std::string &user_id = "",
                       const std::string &mode = "");

  // Streaming variant for terminal/process UX. The scheduler decides
  // eligibility and emits token chunks when streaming is active; callback is
  // invoked per token chunk.
  std::string
  call_llm_stream(const std::string &convo_id, const std::string &user_message,
                  const std::function<void(const std::string &)> &on_token,
                  const std::string &system_message = "",
                  const std::string &user_id = "",
                  const std::string &mode = "");

  // Allows the developer to explicitly force a tool payload without LLM.
  // Throws on executioner connectivity failure, spawn failure, timeout,
  // or child termination.
  json execute_tool(const std::string &instruction, const json &args);

  // Send an event-style IPC message to another process. This message does
  // not use trace_id routing and is delivered via on_bus_event on receiver.
  void send_message(int target_pid, const std::string &purpose,
                    const json &payload);

  // Main lifecycle initialization. Must be called by main() before process
  // exit. Connects to the Supervisor, sends REGISTER_PID, and spawns the
  // heartbeat loop. Throws if registration/connectivity contracts fail.
  void start(int override_pid = -1, const std::string &parent_tree_id = "");

  // Safe trigger to cleanly exit the `run()` loop and cleanly disconnect the OS
  // socket.
  void shutdown();

  // Signal-safe semantic entrypoint: mark forced termination then shutdown.
  void request_forced_shutdown();

protected:
  // Optional lifecycle hook for SDK users to close custom resources cleanly
  // before runtime threads and sockets are torn down.
  virtual void on_shutdown() {}

  // Optional developer hooks for tool execution lifecycle customization.
  // Override by assigning lambdas/functions in derived process constructors.
  std::function<void(const std::string &, const json &)> on_tool_start;
  std::function<void(const std::string &, const json &)> on_tool_finish;

  // Optional developer hook for non-RPC BUS events.
  std::function<void(const json &)> on_bus_event;

  std::string process_name;
  std::string role;
  std::string tree_id;
  std::string user_id;
  json params; // Injected by Executioner on startup
  int os_pid{-1};
  int velix_pid{-1};
  int parent_pid{-1};
  std::atomic<bool> result_reported{
      false};                 // Tracks if a final tool result was dispatched
  std::string entry_trace_id; // The trace ID that launched this process
  std::atomic<ProcessStatus> status{ProcessStatus::STARTING};

  // Global Supervisor Limits (Synced from REGISTER_PID response)
  int max_memory_mb{0};
  int max_runtime_sec{0};

  // Runtime identity — readable by derived run() implementations
  bool is_root{false};
  bool is_handler{false};

  // Outbound persistent TCP Kernel Socket (1-1 coupling with Supervisor)
  velix::communication::SocketWrapper supervisor_socket;
  std::mutex socket_mutex; // Supervisor requests happen asynchronously to the
                           // heartbeats

  // Outbound persistent TCP Bus Socket (for peer-to-peer IPC and Results)
  velix::communication::SocketWrapper bus_socket;
  std::mutex bus_mutex;
  std::thread bus_listener_thread;

  // Async Result Waiting
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::unordered_map<std::string, json> response_map; // trace_id -> payload
  std::unordered_set<std::string> pending_response_traces;

  // Two-Thread Architecture tracking
  std::thread runtime_io_thread;
  std::atomic<bool> is_running{false};
  std::atomic<bool> force_terminate{false}; // Tripped by OS Signal
  std::atomic<bool> forced_by_signal{false};

  // Fast-waking Kernel IO Sleep Synchronization
  std::mutex sleep_mutex;
  std::condition_variable sleep_cv;

  // RAII Termination Reporter
  struct ResultGuard {
    VelixProcess *proc;
    explicit ResultGuard(VelixProcess *p) : proc(p) {}
    ~ResultGuard() noexcept {
      try {
        if (proc && !proc->result_reported && proc->parent_pid > 0) {
          json completion = {{"status", "completed"},
                             {"exit_reason", "normal"},
                             {"pid", proc->velix_pid}};
          proc->report_result(proc->parent_pid, completion,
                              proc->entry_trace_id);
        }
      } catch (...) {
      }
    }
  };

  // Internal SDK Methods
  void run_kernel_io_loop();
  void bus_listener_loop(); // Handles IPM_PUSH messages from the router
  void shutdown_impl(bool invoke_hook);
  uint64_t get_current_memory_usage_mb() const;
  std::string call_llm_internal(
      const std::string &convo_id, const std::string &user_message,
      const std::string &system_message, const std::string &user_id,
      const std::string &mode, bool stream_requested,
      const std::function<void(const std::string &)> &on_token,
      const std::optional<std::string> &intent_override = std::nullopt,
      const std::optional<json> &tool_result_override = std::nullopt);

  // Resumes a conversation with an out-of-band tool result.
  std::string
  call_llm_resume(const std::string &convo_id, const json &tool_result,
                  const std::string &user_id,
                  const std::function<void(const std::string &)> &on_token);

  json execute_tool_internal(const std::string &instruction, const json &args,
                             const std::optional<std::string> &user_id_override,
                             const std::optional<std::string> &intent_override);
  void report_result(int target_pid, const json &data,
                     const std::string &trace_id = "", bool append = true);
  static int resolve_port(const std::string &service_name, int fallback);
};

} // namespace velix::core

#endif // VELIX_PROCESS_HPP
