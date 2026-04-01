# Velix Runtime SDK Guide

This folder contains the Velix process SDK contract used by skills and agents.

## What You Implement

If you are writing a process with the C++ SDK, extend `VelixProcess` and implement `run()`.

Minimal shape:

```cpp
class MySkill : public velix::core::VelixProcess {
public:
	MySkill() : VelixProcess("my_skill", "skill") {}

	void run() override {
		// your deterministic skill logic
	}
};

int main() {
	MySkill skill;
	skill.start();
	return 0;
}
```

## Exposed SDK APIs

Primary methods available to your class:

1. `start()`
- Starts managed lifecycle.
- Registers with Supervisor.
- Starts heartbeat + bus listener.
- Executes your `run()` implementation.

2. `shutdown()`
- Stops managed lifecycle.
- Closes runtime sockets and background loops.

3. `call_llm(convo_id, user_message, system_message)`
- Sends conversation-mode request to Scheduler.
- Handles LLM -> tool execution loop when EXEC/tool calls are emitted.
- Returns final assistant text.

4. `execute_tool(name, args)`
- Sends `EXEC_VELIX_PROCESS` to Executioner.
- Waits reactively for child result over Bus.
- Returns tool payload.

5. `report_result(target_pid, data, trace_id)`
- Relays structured result payload to parent through Bus.

## Errors You Should Expect

The C++ SDK methods may throw `std::runtime_error` for lifecycle and transport issues.

Typical categories:

1. `start()`
- Supervisor connectivity failures.
- Registration rejection/invalid reply.
- Required runtime context missing or malformed.

2. `call_llm(...)`
- Scheduler rejection.
- LLM timeout/network failures.
- Malformed LLM response.
- Tool-call execution failures propagated from Executioner/Bus.

3. `execute_tool(...)`
- Executioner connectivity failures.
- Spawn/rejection failures.
- Tool result timeout.
- Child terminated signal from Supervisor (`child_terminated`).

Recommended pattern:

- Let fatal errors propagate (default).
- Catch only if you can recover locally and still emit a valid result.

## Deterministic Workflow Guaranteed by SDK

When you extend `VelixProcess` and call `start()`, runtime order is deterministic:

1. Capture OS PID and runtime env context.
2. Register process (`REGISTER_PID`) to Supervisor.
3. Receive assigned Velix PID + Tree ID.
4. Register on Bus (`BUS_REGISTER`).
5. Start heartbeat loop (`HEARTBEAT`).
6. Execute developer `run()`.
7. Emit final lifecycle signals and terminate cleanly.

Completion relay behavior:

- Explicit `report_result(...)` is recommended for custom payloads.
- If your process exits without explicit report in child-flow scenarios, SDK fallback attempts a default completion relay.

## Canonical Message Roles

Use canonical internal roles:

- `system`
- `user`
- `assistant`
- `tool`

Legacy aliases such as `agent` are normalized to `assistant` at runtime boundaries.

## Runtime Inputs and Ports

Executioner-provided environment variables:

- `VELIX_PARENT_PID`
- `VELIX_TRACE_ID`
- `VELIX_PARAMS`
- `VELIX_PROCESS_NAME`
- `VELIX_TREE_ID`

Port discovery via `config/ports.json` defaults:

- `SUPERVISOR`: 5173
- `LLM_SCHEDULER`: 5171
- `EXECUTIONER`: 5172
- `BUS`: 5174

Framing protocol:

- 4-byte big-endian payload length
- UTF-8 JSON bytes

## Language SDK Templates

Reference templates are in `runtime/sdk/`:

- `cpp/`
- `python/`
- `node/`
- `go/`
- `rust/`

These templates follow the same runtime contract and are intended to be packaged per language ecosystem.
