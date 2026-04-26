Velix SDK (C++) — VelixProcess Reference

Purpose
This file is the concise, developer-facing C++ API reference for VelixProcess:
what you implement, what the SDK guarantees, and the exact contracts for
messaging, tool execution and reply shapes. It is not a runtime design doc.

Base class (essential surface)

namespace velix::core {

class VelixProcess {
public:
    VelixProcess(std::string name, std::string role);
    virtual ~VelixProcess() noexcept;

    // Lifecycle
    void start(int override_pid = -1, const std::string& parent_tree_id = "");
    void shutdown();

    // Developer entrypoint: implement your process logic here.
    // run() executes on the main thread and may block; this is expected.
    virtual void run() = 0;

    // Optional hooks you may set (function or override):
    // on_bus_event: called for non-RPC bus messages (listener thread).
    std::function<void(const json&)> on_bus_event;
    // on_tool_start: called immediately before a tool is launched.
    std::function<void(const std::string&, const json&)> on_tool_start;
    // on_tool_finish: called after a tool finishes (success or failure).
    std::function<void(const std::string&, const json&)> on_tool_finish;

protected:
    // Override for last-chance cleanup during shutdown.
    virtual void on_shutdown();
};

}

Lifecycle
- start(): registers with Supervisor, connects to Bus, starts IO/listener
  threads and then calls run() on the current thread. When run() returns the
  runtime performs coordinated cleanup.
- shutdown(): graceful shutdown; wakes blocked waits and calls on_shutdown().

LLM functions
- call_llm(convo_id, user_message, system_message, user_id, mode)
  Signature: std::string call_llm(const std::string&, const std::string&, const std::string&, const std::string&, const std::string&);
  Behavior: sends an LLM_REQUEST to the Scheduler and blocks until a final
  response. The SDK manages conversation state, tool-calls, retries and errors.
  Modes: "simple", "conversation", "user_conversation" (see code for
  effective-mode resolution rules).

- call_llm_stream(..., on_token, ...)
  Streams token deltas to on_token and returns the final assistant response.

- call_llm_resume(convo_id, tool_result, user_id, on_token)
  Injects tool_result into the conversation and resumes the LLM loop.

Tool execution (RPC over the runtime)
- execute_tool(tool_name, args)
  Signature: json execute_tool(const std::string& tool_name, const json& args);
  Semantics (important): execute_tool launches the named Velix tool via the
  Executioner service and waits for the result over the Bus. Treat this as an
  RPC/remote call — it is NOT a local function call.

  What happens under the hood:
  - SDK generates a unique trace_id and registers it in pending_response_traces.
  - Sends EXEC_VELIX_PROCESS to Executioner (includes trace_id, tree_id, user_id, params).
  - Blocks on a condition variable until the Bus delivers an IPM_RELAY with a
    matching trace_id and payload (the tool result JSON).

  Failure modes: executioner rejection, launch ack failure, timeout, or child
  termination — these surface as exceptions from execute_tool.

Result reporting (tools -> caller)
- report_result(target_pid, data, trace_id = "", append = true)
  Signature: void report_result(int target_pid, const json& data, const std::string& trace_id = "", bool append = true);
  Purpose: tools use this to deliver their result back to the requester via
  an IPM_RELAY on the Bus.
  
  Modes:
  - append = true: normal RPC response. Wakes the waiting execute_tool and
    (if applicable) resumes the LLM orchestration loop.
  - append = false: async/background mode. The SDK removes the trace from the
    pending set so execute_tool will not resume; the tool must later notify
    the handler with an explicit message (e.g. NOTIFY_HANDLER/TOOL_RESULT).

  If the tool was invoked with a trace_id, that trace_id MUST be preserved
  when calling report_result(). This is required for proper correlation in
  async flows.

Rules (critical):
- A tool MUST call report_result() exactly once before exiting. Missing this
  call will leave the caller blocked indefinitely (deadlock).

Messaging helpers
- send_message(target_pid, purpose, payload)
  Signature: void send_message(int target_pid, const std::string& purpose, const json& payload);
  Sends an IPM_RELAY with purpose and payload. The runtime attaches user_id.
  Use target_pid = -1 to route to the active handler when PID is unknown.

- ask_user(question, options, allow_free_text, timeout_sec, metadata)
  Signature: json ask_user(const std::string& question, const json& options, bool allow_free_text, int timeout_sec, const json& metadata);
  Behavior: creates a trace, sends ASK_USER_REQUEST and blocks until an
  ASK_USER_REPLY with the same trace is received or timeout expires.

  Return value (examples):
  - answered: {"status":"answered","selected_option_id":"opt_1","free_text":null}
  - timeout:  {"status":"timeout"}
  - cancelled: {"status":"cancelled"}

  ask_user handler contract (must be followed by handlers):
  - The handler that receives ASK_USER_REQUEST must forward the user's reply
    to the original requester using send_message(requester_pid, "ASK_USER_REPLY", payload).
  - The forwarded payload MUST include the original trace field exactly as
    received. The SDK matches replies by trace_id.
  - DO NOT use report_result() to deliver ASK_USER replies — report_result is
    intended for tool RPC replies and does not convey purpose; using it will
    not wake ask_user properly.

Hooks (precise semantics)
- on_bus_event(const json& msg)
  - Trigger: called automatically on the Bus listener thread for incoming
    Bus messages that are NOT routed to a blocking RPC (no matching trace).
  - Rules: must be fast, non-blocking, and thread-safe. Offload heavy work to
    worker threads. Do not mutate core runtime structures (pending_response_traces, etc.).

- on_tool_start(const std::string& tool, const json& args)
  - Trigger: called immediately before the SDK sends EXEC_VELIX_PROCESS to
    the Executioner. Receives tool name and args (SDK may add user_id).
  - Use: logging, metrics. Should not block or throw.

- on_tool_finish(const std::string& tool, const json& result)
  - Trigger: called after tool execution completes (always called on success
    or failure). The result contains the tool payload. On failure the SDK
    wraps the error into an error-shaped JSON (for example {"status":"error", ...}).
  - Use: logging, analytics, fire-and-forget follow-ups. Must not block or
    mutate the result to affect delivery semantics.

- on_shutdown()
  - Trigger: called during shutdown before sockets/threads are torn down.
  - Use: quick cleanup only.

These hooks are auto-invoked by the SDK. Assign them or override where
appropriate in your VelixProcess subclass.

Execution model (concise)
- The SDK contains a built-in orchestration loop for agents:
  call_llm → Scheduler reply (may contain tool_calls) → SDK executes tool_calls
  (via execute_tool, possibly in parallel) → SDK injects tool results into the
  conversation and resumes the LLM call automatically.
- Tool calls within a single LLM reply are executed in parallel (std::async in
  current implementation). The loop is bounded by MAX_ITERATIONS; exceeding it
  causes the call to error.

Configuration helpers
- int resolve_port(const std::string& service_name, int fallback)
  - Reads VELIX_PORT_<SERVICE> environment variables (env-first).
- int resolve_sdk_config(const std::string& key, int fallback)
  - Reads VELIX_SDK_<KEY> then SDK_<KEY> as fallback.

Failure semantics (summary)
- Velix is fail-fast. Common traps:
  - forgetting report_result() in a tool -> deadlock
  - losing Bus connection -> is_running set false; shutdown begins
  - tool crash / child termination -> execute_tool throws
  - scheduler/executioner rejects or times out -> execute_tool / call_llm throws

Minimal examples

Tool (simple):
```cpp
class MyTool : public velix::core::VelixProcess {
public:
  MyTool() : VelixProcess("my_tool", "tool") {}
  void run() override {
    // Compute result and report back to the parent requester
    json result = { {"status", "ok"}, {"data", "hello"} };
    // parent_pid is set by the runtime during start(); report_result wakes the caller.
    report_result(parent_pid, result);
  }
};
```

Handler snippet (ask_user):
```cpp
// In a handler process that receives ASK_USER_REQUEST on the Bus:
void handle_incoming_ask(const json& req) {
  // req contains: trace, question, options, allow_free_text, timeout_sec, metadata
  json reply = { {"trace", req["trace"]}, {"status", "answered"}, {"selected_option_id", "opt_1"} };
  // Forward to requester exactly as a Bus relay with purpose ASK_USER_REPLY
  send_message(req.value("source_pid", -1), "ASK_USER_REPLY", reply);
}
```

Appendix — expected Bus reply shapes (practical)
- execute_tool result (IPM_RELAY payload): any JSON; SDK expects a JSON with
  success or error semantics. Example success: {"status":"ok","value":{...}}
  Example error: {"status":"error","error":"child_terminated","reason":"oom"}

 - ASK_USER_REPLY payload (forwarded by handler):
   Required: {"trace": "<trace-id>", "status": "answered|timeout|cancelled"}
   If answered: include either {"selected_option_id":"<id>"} or {"free_text":"..."}

Developer quick checklist
- Tools: call report_result(target_pid, data, trace_id) exactly once.
- Handlers: forward ASK_USER_REPLY using send_message(requester_pid, "ASK_USER_REPLY", payload) preserving trace.
- Hooks: keep on_bus_event/on_tool_start/on_tool_finish/on_shutdown non-blocking and resilient.

This file is the canonical C++ SDK reference. Keep it short and copy-pasteable
for developer consumption.
