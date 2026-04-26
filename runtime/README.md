Below is a compact, authoritative Velix SDK specification for developers.

Keep this short — it defines required behavior, contracts and the minimal
API surface you need to implement tools, agents and handlers.

---

# Velix SDK – Developer Guide

This document describes how developers implement tools, agents, and handlers
using the Velix SDK.

> NOTE: Examples in this repo primarily use C++. See runtime/sdk/python/README.md
> for Python-specific examples.

Runtime conventions (must follow)
- Env-first configuration: use VELIX_PORT_*, VELIX_SDK_* and the injected
  VELIX_* environment variables (VELIX_PARENT_PID, VELIX_TRACE_ID,
  VELIX_PARAMS, VELIX_USER_ID).
- LLM modes: only `simple`, `conversation`, `user_conversation` are valid.

---

# 1. Creating a Velix Tool / Process

Every Velix component inherits from class VelixProcess. Implement only run()
— the runtime handles registration, supervisor comms, the BUS, and heartbeats.

Minimal C++ process pattern (conceptual):
```cpp
class MyProcess : public VelixProcess { void run() override { /* your logic */ } };
```

# 2. Process Roles

- Tool: short-lived, stateless. MUST call report_result() exactly once.
- Agent: uses LLMs, orchestrates tools, coordinates workflows. LLM calls may automatically trigger tool execution and resume.
- Handler: entry point for users; forwards ask_user requests and delivers replies.

---

# 3. Quick API Reference (what you actually need)

Core primitives:
- run()
- call_llm(convo_id, user_message, system_message, user_id, mode)
- call_llm_stream(..., on_token)
- execute_tool(tool_name, args) → executes a tool via the runtime and waits for result
- report_result(target_pid, data, trace_id = "", append = true)
- call_llm_resume(convo_id, tool_result, user_id, on_token)
- send_message(target_pid, purpose, payload)
- ask_user(question, options, allow_free_text, timeout_sec, metadata)
- on_bus_event(msg) → auto-called on the IO thread for non-RPC events; must be fast and non-blocking. Override to observe or dispatch work to a worker thread; do not mutate core runtime state.
- on_tool_start(tool, args) → auto-called immediately before the runtime issues a tool invocation. Use for logging/metrics or lightweight metadata annotation; do not block and do not rely on modifying args to change runtime behavior.
- on_tool_finish(tool, result) → auto-called after a tool completes or fails (always invoked). Use for logging, analytics or triggering async follow-ups; do not block and do not modify the result expecting it to change the delivered payload.
- on_shutdown() → auto-called during process shutdown. Override to close sockets, flush logs and stop background workers; keep cleanup quick so shutdown proceeds.

These hooks are invoked automatically by the runtime at the points above — implement them by overriding/assigning in your VelixProcess subclass for your preferred behavior.

Rules to follow:
- Tools: call report_result() exactly once before exit. If append=false,
  later send NOTIFY_HANDLER with final result.
- ask_user flows: ask_user() sends ASK_USER_REQUEST with a unique trace and
  blocks until ASK_USER_REPLY with the same trace arrives. ASK_USER_REPLY must
  be delivered to the original requester using send_message(requester_pid,
  "ASK_USER_REPLY", payload). Do NOT use report_result() for ASK_USER replies.
- send_message(): purpose is passed verbatim; use purpose="NOTIFY_HANDLER" for
  handler notifications. If handler PID is unknown, use target_pid = -1.
- on_bus_event(): MUST be non-blocking and quick; spawn worker threads for heavy
  work. Hooks must not throw or mutate core runtime state.

Messaging and correlation:
- All RPC-like flows use trace ids. Preserve trace ids exactly.

Environment and config:
- Use VELIX_PORT_<SERVICE> and VELIX_SDK_<KEY> environment variables.

Failure semantics (short):
- System is fail-fast: missing report_result() causes deadlocks; tool crashes
  and timeouts surface as errors; bus failures are terminal.

Common mistakes to avoid:
- blocking inside on_bus_event
- using report_result() to deliver ASK_USER replies
- not calling report_result() from a tool
- passing invalid LLM mode

---

This file is intentionally concise. For language-specific examples and
expanded SDK usage see runtime/sdk/<language>/README.md (Python/C++/Node/etc.).
