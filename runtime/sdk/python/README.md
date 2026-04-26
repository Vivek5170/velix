# Velix Python SDK – Developer Guide

This document describes how to build Velix **tools, agents** using the Python SDK. The Python SDK mirrors the logic of the C++ implementation but follows Pythonic conventions.

> [!IMPORTANT]
> Valid LLM modes are strictly: `simple`, `conversation`, and `user_conversation`. 
> The `chat` alias is no longer supported.

---

## 1. Creating a Velix Process

Every Python component must inherit from the `VelixProcess` class.

Note: `run()` executes on the main thread and may block; this is expected.

> Note: Velix adopts an "env-first" runtime model. The executioner injects
> runtime configuration (service ports and SDK settings) into environment
> variables using the conventions `VELIX_PORT_<SERVICE>` and `VELIX_SDK_<KEY>`.
> SDKs should prefer these environment variables over reading files at runtime.
> See the top-level docs for `ASK_USER_REQUEST` / `ASK_USER_REPLY` protocol.

### Minimal Process Example

```python
from runtime.sdk.python.velix_process import VelixProcess

class MyProcess(VelixProcess):
    def __init__(self):
        super().__init__(name="my_python_process", role="tool")

    def run(self):
        # Request reasoning from the LLM
        response = self.call_llm(
            convo_id="",
            user_message="Explain decorators in Python",
            mode="simple"
        )

        # Return the result to the caller. Pass the parent's PID so the
        # runtime can route the reply. If this tool was launched with a
        # trace_id, preserve that trace when calling report_result(trace_id=...).
        self.report_result(self.parent_pid, {
            "status": "ok",
            "response": response
        })

if __name__ == "__main__":
    # In practice, your process is started by the executioner.
    # Instantiate and start the runtime.
    MyProcess().start()
```

---

## 2. Core SDK Functions

The Python SDK exposes the same primitives as the C++ version:

| Method | Purpose |
| :--- | :--- |
| `run()` | The main entry point (override this). |
| `call_llm()` | Blocks until the LLM returns a full response. |
| `call_llm_stream()` | Executes a streaming LLM turn with a token callback. |
| `call_llm_resume()` | Resumes reasoning after a background task completes. |
| `execute_tool(instruction, args)` | Executes a tool via the Velix runtime (remote RPC). Launches via the Executioner and waits on the Bus by trace_id. (instruction == tool name) |
| `report_result(target_pid, data, trace_id="", append=True)` | Reports the tool's output back to the caller. Pass target_pid (usually parent_pid). |
| `send_message(target_pid, purpose, payload)` | Sends an event (IPM) to another process (or the Handler). |

---

## 3. Tool Reporting Patterns

### Synchronous (Standard)
For most tools that finish quickly, just report the result and exit.

```python
def run(self):
    data = self.do_actual_work()
    # If this tool was launched with a trace_id, preserve that trace when
    # calling report_result(trace_id=...). Pass the parent PID so the
    # runtime can route the reply.
    self.report_result(self.parent_pid, {"status": "ok", "output": data})
```

### Asynchronous (Background)
If your tool starts a long-running process (like a build or a browser session), you must use the `append=False` pattern.

1.  **Acknowledge Start**: Call `report_result` immediately to inform the LLM/Caller that the work is in progress.
2.  **Report Completion**: Use `send_message` with `notify_type: "TOOL_RESULT"` once the background task finishes.

```python
def run(self):
    # Start background work on a thread and immediately acknowledge.
    threading.Thread(target=self._background_task, daemon=True).start()

    # Acknowledge start without appending to conversation history.
    self.report_result(self.parent_pid, {"status": "background", "message": "Task started..."}, append=False)

def _background_task(self):
    result_data = self.do_work()

    # Forward completion to the handler tree root so the handler can resume
    # the conversation. Preserve user_id in payload where relevant.
    self.send_message(
        target_pid=-1,
        purpose="NOTIFY_HANDLER",
        payload={
            "notify_type": "TOOL_RESULT",
            "tool": self.process_name,
            "result": {"status": "ok", "output": result_data},
        },
    )
```

---

## 4. LLM Orchestration

summary = self.call_llm(
    convo_id="",
    user_message="Summarize this logs file",
    mode="simple"
)
```

### Mode: `conversation`
Used for standard multi-turn reasoning owned by the calling process.

```python
reply = self.call_llm(
    convo_id="unique_id",
    user_message="What was the last step?",
    mode="conversation"
)
```

### Mode: `user_conversation`
Used for handler-driven session history mapping. If `convo_id` is empty, the system resolves the active conversation for the given `user_id`.

```python
reply = self.call_llm(
    convo_id="", # Auto-resolved from user_id
    user_message="Continue for this user",
    user_id="alice",
    mode="user_conversation"
)
```

## ask_user usage (Python)

To prompt a human from inside a running Python Velix process:

```python
result = self.ask_user(
    "Approve running this command?",
    options=[{"id":"allow","label":"Allow"}, {"id":"deny","label":"Deny"}],
    allow_free_text=False,
    timeout_sec=600,
    metadata={"command":"pwd"}
)

if result.get("status") == "answered":
    choice = result.get("selected_option_id")
    # handle choice
elif result.get("status") == "timeout":
    # fallback
```


---

## 5. Event Callbacks

You can register callbacks to observe system events or customize behavior:

```python
class MyAgent(VelixProcess):
    def __init__(self):
        super().__init__(name="agent", role="agent")
        
        # Monitor tool calls
        self.on_tool_start = self._log_start
        self.on_tool_finish = self._log_finish
        
        # Handle custom messages
        self.on_bus_event = self._handle_event

    def _log_start(self, tool, args):
        print(f"Executing {tool} with {args}")

    def _handle_event(self, msg):
        if msg.get("purpose") == "SYSTEM_ALERT":
            self.stop_work()
```

---

## 6. Best Practices

1.  **Don't Block `on_bus_event`**: These callbacks run on the IO thread. If you need to perform heavy reasoning, spawn a separate worker thread.
2.  **Use `TOOL_RESULT` for Resumption**: Always use the standardized `notify_type` and include the result in the `result` key.
3.  **Trace Consistency**: When calling `report_result`, the SDK automatically attaches the `entry_trace_id` for you.
    If the tool was invoked with a trace_id, that trace_id MUST be preserved when calling `report_result(trace_id=...)`.
4.  **User Routing**: For any event intended for a user session (NOTIFY_HANDLER, TOOL_START), always include the `user_id` in the payload or message metadata to avoid dropped events.

5.  **Hooks timing & thread context**:
    - `on_bus_event` runs on the Bus listener thread and must be non-blocking.
    - `on_tool_start` fires immediately before the Executioner is launched.
    - `on_tool_finish` fires after execution completes (including failures).

6.  **on_tool_finish error shape**: on_tool_finish is always called (success or failure). On failure the SDK will pass an error-shaped JSON such as {"status":"error", ...} to the hook.

7.  **ResultGuard**: If a tool exits without calling `report_result()`, the SDK's ResultGuard may automatically emit a fallback completion result using the entry_trace_id so the caller is not left permanently blocked.
