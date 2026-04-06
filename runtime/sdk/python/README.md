# Velix Python SDK – Developer Guide

This document describes how to build Velix **skills, tools, and agents** using the Python SDK. The Python SDK mirrors the logic of the C++ implementation but follows Pythonic conventions.

> [!IMPORTANT]
> Valid LLM modes are strictly: `simple`, `conversation`, and `user_conversation`. 
> The `chat` alias is no longer supported.

---

## 1. Creating a Velix Process

Every Python component must inherit from the `VelixProcess` class.

### Minimal Process Example

```python
from runtime.sdk.python.velix_process import VelixProcess

class MyProcess(VelixProcess):
    def __init__(self):
        super().__init__(name="my_python_process", role="skill")

    def run(self):
        # Request reasoning from the LLM
        response = self.call_llm(
            convo_id="",
            user_message="Explain decorators in Python",
            mode="simple"
        )

        # Return the result to the caller
        # The SDK automatically handles the current trace_id for you.
        self.report_result({
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
| `execute_tool()` | Synchronously executes another tool/skill. |
| `report_result()` | Reports the tool's output back to the caller. |
| `send_message()` | Sends an event (IPM) to another process (or the Handler). |

---

## 3. Tool Reporting Patterns

### Synchronous (Standard)
For most tools that finish quickly, just report the result and exit.

```python
def run(self):
    data = self.do_actual_work()
    self.report_result({"status": "ok", "output": data})
```

### Asynchronous (Background)
If your tool starts a long-running process (like a build or a browser session), you must use the `append=False` pattern.

1.  **Acknowledge Start**: Call `report_result` immediately to inform the LLM/Caller that the work is in progress.
2.  **Report Completion**: Use `send_message` with `notify_type: "TOOL_RESULT"` once the background task finishes.

```python
def run(self):
    # 1. Start background work
    self.start_background_thread()
    
    # 2. Acknowledge immediately (append=False prevents history pollution)
    self.report_result(
        {"status": "background", "message": "Task started..."},
        append=False
    )

def on_work_finished(self, result_data):
    # 3. Resume the LLM reasoning loop later
    self.send_message(
        target_pid=-1,  # Route to the Handler
        purpose="NOTIFY_HANDLER",
        payload={
            "notify_type": "TOOL_RESULT",
            "tool": self.name,
            "result": {"status": "ok", "output": result_data}
        }
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
4.  **User Routing**: For any event intended for a user session (NOTIFY_HANDLER, TOOL_START), always include the `user_id` in the payload or message metadata to avoid dropped events.
