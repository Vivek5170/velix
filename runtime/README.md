Below is a **single clean Markdown spec** that includes the **correct behavior for agents, tools, and handlers**, including **result reporting and notifying handlers**.
Unnecessary internal details are removed and only **SDK rules, functions, usage, modes, and behavior contracts** are kept.

---

# Velix SDK – Developer Guide

This document describes how developers implement **skills, tools, agents, and handlers** using the Velix SDK.

Velix provides a **process-based runtime** where components communicate through the Velix infrastructure.

Developers interact with the runtime through the **`VelixProcess` SDK class**.

---

# 1. Creating a Velix Skill / Process

Every Velix component must inherit from:

```cpp
class VelixProcess
```

Minimal process example:

```cpp
#include "velix_process.hpp"

using namespace velix::core;

class MyProcess : public VelixProcess {
public:
    MyProcess() : VelixProcess("my_process", "skill") {}

    void run() override {

        std::string response = call_llm(
            "You are helpful",
            "Explain recursion",
            "simple"
        );

        json result = {
            {"status","ok"},
            {"response",response}
        };

        report_result(result);
    }
};

int main() {
    MyProcess proc;
    proc.start();
}
```

The runtime automatically handles:

* process registration
* supervisor communication
* BUS messaging
* heartbeat monitoring
* execution tree membership

Developers only implement logic inside **`run()`**.

---

# 2. Process Roles

Velix processes usually fall into three roles.

### Tool / Skill

Small processes that perform a task.

Properties:

```
short execution
stateless
returns result
```

Tools **must call `report_result()`**.

---

### Agent

Processes that orchestrate reasoning and tool usage.

Properties:

```
call LLM
call tools
coordinate workflow
notify handler when work completes
```

Agents usually send events to handlers.

---

### Handler

Entry points that interact with users.

Examples:

```
terminal handler
telegram handler
api handler
```

Handlers:

```
receive user requests
spawn agents/tools
receive events from agents
deliver responses to users
```

---

# 3. SDK Functions

Velix SDK exposes the following functions.

| Function        | Purpose                    |
| --------------- | -------------------------- |
| run             | process entry point        |
| call_llm        | request LLM response       |
| call_llm_stream | stream LLM tokens          |
| execute_tool    | execute another skill/tool |
| report_result   | return result to caller    |
| send_message    | send event message         |
| on_tool_start   | hook before tool execution |
| on_tool_finish  | hook after tool execution  |
| on_bus_event    | receive event messages     |

---

# 4. `run()`

### Signature

```cpp
virtual void run() = 0;
```

### Purpose

Main logic of the process.

Executed after the runtime initializes the process.

Inside `run()` developers may:

* call LLMs
* execute tools
* send messages
* run loops
* spawn threads

Example:

```cpp
void run() override {

    json result = execute_tool(
        "web_search",
        {{"query","Velix architecture"}}
    );

    std::cout << result.dump() << std::endl;
}
```

---

# 5. `call_llm()`

### Signature

```cpp
std::string call_llm(
    const std::string& convo_id,
    const std::string& user_message = "",
    const std::string& system_message = "",
    const std::string& user_id = "",
    const std::string& mode = ""
);
```

### Purpose

Send a request to the configured LLM.

The function blocks until the response is returned.

---

## Mode: `simple`

Stateless LLM request.

Conditions:

```
mode = "simple"
convo_id must be empty
```

Behavior:

```
no conversation history
single independent request
```

Example:

```cpp
call_llm(
    "",
    "Summarize this text",
    "You are helpful",
    "",
    "simple"
);
```

---

## Mode: `conversation`

Maintains conversation history.

Conditions:

```
mode = "conversation"
convo_id must not be empty
```

Behavior:

```
conversation history stored
future prompts use past context
```

Example:

```cpp
call_llm(
    "session_42",
    "Continue discussion",
    "You are assistant",
    "user_123",
    "conversation"
);
```

---

## Possible Errors

`call_llm()` may throw:

| Error            | Cause              |
| ---------------- | ------------------ |
| runtime_error    | LLM request failed |
| invalid_argument | invalid mode       |
| timeout_error    | LLM timeout        |

---

# 6. `call_llm_stream()`

### Signature

```cpp
std::string call_llm_stream(
    const std::string& convo_id,
    const std::string& user_message,
    const std::function<void(const std::string&)>& on_token,
    const std::string& system_message = "",
    const std::string& user_id = "",
    const std::string& mode = ""
);
```

### Purpose

Stream LLM output token-by-token.

Each token triggers:

```
token_callback(token)
```

The function also returns the final full response.

---

### Example

```cpp
call_llm_stream(
    "",
    "Explain transformers",
    [&](const std::string& token){
        std::cout << token;
    },
    "You are helpful",
    "user_123",
    "simple"
);
```

---

# 7. `execute_tool()`

### Signature

```cpp
json execute_tool(
    const std::string& tool_name,
    const json& args
);
```

### Purpose

Execute another Velix tool.

The function blocks until the tool returns a result.

---

### Example

```cpp
json result = execute_tool(
    "web_search",
    {{"query","Velix runtime"}}
);
```

---

### Errors

| Error            | Cause                      |
| ---------------- | -------------------------- |
| timeout_error    | tool did not return result |
| runtime_error    | tool failed                |
| invalid_argument | tool name invalid          |

---

# 8. `report_result()`

### Signature

```cpp
void report_result(const json& result);
```

### Purpose

Return execution result to the caller.

Used by **tools/skills**.

---

### Example

```cpp
json output = {
    {"status","ok"},
    {"data","search results"}
};

report_result(output);
```

---

### Required Rule

Tools must call:

```
report_result()
```

exactly once before exiting.

If no result is reported:

```
execute_tool() will eventually timeout
```

---

# 9. `send_message()`

### Signature

```cpp
void send_message(
    int target_pid,
    const std::string& purpose,
    const json& payload
);
```

### Purpose

Send an event message to another process.

SDK behavior:

```
purpose is passed exactly as provided by the caller

user_id is always attached automatically
```

Notes:

- `handler_pid` is typically provided by the runtime or startup configuration.
- If you do not have an explicit handler PID, send `target_pid = -1`.
- The Bus will then resolve the message to the registered handler tree root PID, if available.

Developer guidance for agents:

```
use purpose = "NOTIFY_HANDLER" when notifying handlers
```

---

### Example

```cpp
send_message(
    handler_pid,
    "NOTIFY_HANDLER",
    {{"summary","task completed"}}
);
```

If `handler_pid` is not known, you may send `-1` and the Bus will route to the active handler tree root.

---

### Typical Uses

```
agent notifying handler
background job completion
delegated tasks
inter-agent communication
```

---

# 10. `on_bus_event`

### Definition

```cpp
std::function<void(const json&)> on_bus_event;
```

### Purpose

Handle incoming event messages.

Triggered when a message arrives that is **not an RPC response**.

This hook is intended for SDK developers building handlers and agents that need to react to custom bus notifications.

# 11. `on_tool_start` / `on_tool_finish`

### Definition

```cpp
std::function<void(const std::string&, const json&)> on_tool_start;
std::function<void(const std::string&, const json&)> on_tool_finish;
```

### Purpose

These hooks let your SDK process observe tool execution lifecycle events.

- `on_tool_start` is called immediately before a tool request is launched.
- `on_tool_finish` is called after the tool response is received.

`on_tool_finish` receives the tool reply payload.
In normal execution, that payload is the JSON object returned by the tool’s `report_result()` call.
If the tool fails or the execution flow throws, `on_tool_finish` is still invoked with an error payload containing at least:

- `status`: `"error"`
- `message`: the failure message

Typical uses:

- logging tool invocation metadata
- forwarding tool call progress to clients
- emitting UI events to the terminal
- auditing or tracing tool usage

### Example

```cpp
on_tool_start = [&](const std::string &tool,
                    const json &args) {
  std::cout << "[Hook] Tool start: " << tool
            << " args=" << args.dump() << std::endl;
};

on_tool_finish = [&](const std::string &tool,
                     const json &result) {
  if (result.value("status", "") == "error") {
    std::cerr << "[Hook] Tool failed: " << tool
              << " message=" << result.value("message", "unknown")
              << std::endl;
  } else {
    std::cout << "[Hook] Tool finish: " << tool
              << " result=" << result.dump() << std::endl;
  }
};
```

SDK developers can use these hooks in handlers to display tool lifecycle events in the terminal.

---

# 12. `on_shutdown`

### Definition

```cpp
virtual void on_shutdown() {}
```

### Purpose

Override this hook in your process when you need to close resources cleanly before the runtime tears down sockets and exits.

This is the proper hook for:

- closing sockets
- flushing logs
- releasing external resources
- terminating background threads safely

Use it when your process may be forced to shut down and you need a last chance to clean up.

---

### Example

```cpp
void on_shutdown() override {
    if (server.is_open()) {
        server.close();
    }
    if (terminal_client.is_open()) {
        terminal_client.close();
    }
}
```

---

### Event example

```cpp
on_bus_event = [&](const json& msg){

    std::string purpose = msg.value("purpose","");
    std::string sender_user_id = msg.value("user_id", "");

    if(purpose == "NOTIFY_HANDLER") {
        std::cout << "Agent finished work" << std::endl;
    }
};
```

---

### Important Rule

`on_bus_event` must execute quickly.

Do **not run heavy operations inside it**.

Instead dispatch work to worker threads.

---

# 13. Example Agent

Agents typically:

```
use LLM for reasoning
execute tools
notify handler when finished
```

Example:

```cpp
void run() override {

    std::string plan = call_llm(
        "",
        "Search for AI news",
        "You are an agent",
        "",
        "simple"
    );

    json result = execute_tool(
        "web_search",
        {{"query","AI news"}}
    );

    send_message(
        handler_pid,
        "NOTIFY_HANDLER",
        result
    );
}
```

---

# 14. Best Practices

### Tools

```
stateless
fast
single result
```

---

### Agents

```
use LLM for planning
call tools for actions
notify handlers when finished
```

---

### Event Handlers

```
must return quickly
avoid blocking calls
dispatch heavy tasks to worker threads
```

---

# Summary

Velix SDK provides the following primitives:

```
run()
call_llm()
call_llm_stream()
execute_tool()
report_result()
send_message()
on_tool_start()
on_tool_finish()
virtual void on_shutdown()
on_bus_event()
```

Using these functions developers can build:

```
tools
agents
delegation systems
background workers
interactive handlers
multi-agent workflows
```

without modifying the Velix runtime.
