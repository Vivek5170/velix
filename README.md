# velix-Core: Runtime Architecture (Single Source of Truth)

`velix-Core` is a local-first, multi-process AI runtime designed as a mini operating system for agents.

This document is the authoritative architecture spec. It incorporates the finalized two-tier conversation system, the direct Supervisor-controlled process lifecycle, and the updated Scheduler / Executioner model.

---

## 1. Core Philosophy

- **Process Isolation**: Every major runtime role is a separate process.
- **Strict Responsibility Boundaries**: Components must not overlap roles.
- **Security-First Execution**: Process spawning is centralized and controlled by the Executioner, while process lifecycle and tree access are entirely governed by the Supervisor.
- **Language Scalability**: Any language can integrate via JSON-over-TCP sockets.
- **Local-First Efficiency**: Designed natively for constrained local LLM deployments.

---

## 2. Fundamental Principles

- **Zero-Dependency Core**: Built with `g++` / C++17 on Linux/macOS/Windows.
- **Single-Header Vendor Strategy**: Keep third-party logic vendored (e.g., `vendor/nlohmann/json.hpp`).
- **IPC via TCP Loopback**: Standardized transport over `127.0.0.1`.
- **Length-Prefixed Framing**: `[4-byte big-endian length][JSON bytes]` for robust socket transport.
- **Strict Modularity**: Agents, skills, and jobs are folder-based, discoverable extensions.
- **Cross-Language Compatibility**: C++, Python, Rust, Go can participate through schema-compliant JSON.

### Project Structure

```text
velix-core
├── core/                   # Central OS & Execution Hub:
│   ├── supervisor.cpp      # OS Layer: Spawns handler, tracks lifecycles, and enforces security.
│   ├── executioner.cpp     # Orchestrator: Dynamically spawns skill/agent child processes.
│   └── handler.cpp         # System Root: Interfaces with chat clients and user identities.
├── llm/                    # Intelligence & Routing:
│   ├── scheduler.cpp       # Thread pool queue, priority management, and tool routing.
│   ├── conversation_manager.hpp/cpp  # Strict deterministic disk I/O and state caching.
│   └── compacter.cpp       # Summarization background worker for long context windows.
├── communication/          # Networking abstractions for local TCP.
├── config/                 # Environment logic, Supervisor rules, and TCP port mapping.
├── schema/                 # Strictly defined IPC contracts (llm_request, ipc_message, etc).
├── vendor/                 # Single-header dependencies (nlohmann/json.hpp).
├── memory/                 # Immutable / Long-Term Data:
│   ├── conversations/      # Two-tier dynamic state (users/ vs proc/).
│   ├── soul.md             # Fixed personality behaviors.
│   └── user.md             # Persistent user facts.
├── chat/                   # Interfaces (e.g., Telegram Bot).
├── agents/                 # Macro logic: Multi-step orchestrators (Sub-folders).
└── skills/                 # Atomic tools: Python scripts and system interactions.
```

---

## 3. High-Level Runtime Topology

```text
User / Chat (Telegram or other channel)
  -> Handler (Spawned directly by Supervisor on startup)
  -> Conversation Manager (Disk I/O, Identity, Compaction)
  -> Scheduler (LLM Queue, Priority, and EXEC Parsing)
  -> Executioner (Only when an EXEC block is emitted by LLM)
  -> Target Skill/Agent (Runs, registers with Supervisor, returns Tool Result)
  -> Executioner -> Scheduler 
  -> Handler
  -> Chat response

Supervisor runs as the root authority (pid 0 equivalent) across all trees.
```

---

## 4. Component Responsibilities

### 4.1 Supervisor (`core/supervisor.cpp`) — The OS Kernel
- **Global system authority**. The root process of the Velix ecosystem.
- Directly spawns the **Handler** on startup via POSIX (`fork`/`exec`) and tracks its OS-level PID.
- Operates a watchdog thread to auto-restart the Handler if it crashes (`handler_needs_restart_`).
- Maintains the global process table and tree lifecycle state. Tracks periodic component heartbeats (`15s` timeout).
- **Access Control:** Verifies every `LLM_REQUEST` before the Scheduler allows it into the active queue.
  - Enforces tree boundary rules for `user` conversations.
  - Enforces strict pid-locking for ephemeral `process` conversations.
- Cleans up ephemeral conversation metadata automatically when a leaf process dies.

### 4.2 Handler (`core/handler.cpp`)
- Ingress/egress router for chat channels (`chat/telegram_bot.cpp` and future interfaces).
- Runs inside the privileged `TREE_HANDLER` system tree.
- Receives human input, resolves the `user_id`, and passes it to the Scheduler.
- Does **not** know its own conversation ID; relies entirely on the Conversation Manager to deterministically resolve persistent communication states from the user ID.
- Does **not** own EXEC orchestration logic.

### 4.3 Conversation Manager (`llm/conversation_manager.cpp`)
- Manages strict two-tier storage:
  1. **User Convos (`type="user"`)**: Permanent, multi-turn stores placed in `memory/conversations/users/{user_id}.json`. Creation is idempotent and immune to Handler restarts.
  2. **Process Convos (`type="process"`)**: Ephemeral scratchpads placed in `memory/conversations/proc/{pid}/`. Deleted entirely when the creator process dies or via an hourly background sweep `cleanup_loop()`.
- Replaces incoming `messages` cleanly via lock-free state operations natively wrapped in `worker_loop` threads to prevent multi-second compilation locks.

### 4.4 Scheduler (`llm/scheduler.cpp`) — Execution & Priority Manager
1. **Threadpool Management**: Runs `max_llm_keys` worker threads. Serializes concurrent requests affecting the same conversation using the `convo_id/tree_id` as a `queue_key`.
2. **Conversation Hydration**: Calls `ConversationManager` safely inside the worker pool to load conversation histories.
3. **Execution Routing**: Parses the raw LLM output for `EXEC ... END_EXEC` payload blocks.
   - For simple responses, returns immediately to Handler.
   - For tool usages, dispatches an async `EXEC_REQUEST` to the Executioner and suspends the LLM completion loop.

### 4.5 Executioner (`core/executioner.cpp`)
- **Only** component allowed to dynamically spawn agent/skill processes.
- Validates the target `manifest.json`.
- Injects `tree_id` inheritance so spawned child processes accurately reflect the root request that initiated them.
- Provides strict containerization via timeout checks and resource monitoring.

### 4.6 Velix Runtime SDK Layer

Runtime class hierarchy:

```text
VelixProcess
 ├ VelixAgent
 ├ VelixSkill
 └ VelixJob
```

Responsibilities of `VelixProcess`:

- register PID with Supervisor,
- send periodic heartbeat,
- receive validated parameters,
- execute `run()` contract,
- return result,
- report structured errors,
- access LLM only through Scheduler APIs,
- use runtime-provided IPC helper APIs to communicate with Scheduler, Executioner, and Supervisor.

This layer is mandatory for reliable drag-and-drop extensibility.

---

## 5. Execution Tree Model

Each user request or scheduled job runs inside an isolated **Tree**.

```text
TREE_HANDLER (System tree)
└ Handler (Root process)

TREE_01 (Ephemeral task tree)
├ Agent_Research
│  ├ Skill_WebSearch
│  └ Skill_Parse
└ Skill_Calculator
```

**Execution rules inside a tree:**
- Any process can request a temporary `process` conversation.
- Only members of `TREE_HANDLER` can request interaction with a `user` conversation.
- Spawned processes inherit the parent's `tree_id`.
- Processes terminate gracefully via `PROCESS_TERMINATE` IPC sent to the Supervisor.

---

## 6. IPC Protocol and Canonical Schemas

All IPC modules operate over defined TCP JSON strings. Interoperable message types:
`REGISTER_PID`, `HEARTBEAT`, `LLM_REQUEST`, `EXEC_REQUEST`, `EXEC_RESULT`, `TREE_KILL`, `TREE_STATUS`.

### `schema/llm_request.json` (Reference)

```json
{
  "message_type": "LLM_REQUEST",
  "mode": "conversation",
  "request_id": "UUID-12346",
  "user_id": "alice",
  "source_pid": 1000,
  "tree_id": "TREE_HANDLER",
  "priority": 1,
  "messages": [
    { "role": "user", "content": "Hello, I am Alice." }
  ]
}
```
*Note: The Scheduler handles missing internal values (like `convo_type` and deterministic `convo_id` resolution).*

### Tool Result Schema

```json
{
  "tree_id": "TREE_02",
  "pid": 1042,
  "status": "ok",
  "result": { "timezone": "EST", "temp": "45" },
  "error": null
}
```

### `config/ports.json` (Discovery Reference)

```json
{
  "HANDLER": 5170,
  "LLM_SCHEDULER": 5171,
  "EXECUTIONER": 5172,
  "SUPERVISOR": 5173
}
```

---

## 7. Build Order and Constraints

### Status Map
1. `communication/` — **Complete**
2. `schema/` — **Complete** (Updated to v2 Architecture)
3. `utils/` — **Complete**
4. `memory/` — **Complete**
5. `core/supervisor` — **Complete** (OS root lifecycle + LLM verification)
6. `llm/conversation_manager` — **Complete** (Dual storage mapping + Deadlock fixes)
7. `llm/scheduler` — **Complete** (Convo serialization + `call_llm` HTTP curl wrapping)
8. `core/executioner` — *Pending*
9. `core/handler` — *Pending*

**Design constraints:**
- Avoid `std::system()` in runtime execution paths. Use API-based forks.
- Preserve strict modular dataflow: Wait states must never lock global shared caches.
- Preserve deterministic identity: `user_id` translates exclusively to `user_{id}` on disk to survive handler application crashes.

---

## 8. Outcome

`velix-Core` behaves as a modular, secure, scheduler-driven local agent runtime. It is designed to scale dynamically from personal assistant usage to complex multi-agent orchestration while isolating memory leaks, runaway loops, and malicious skill logic safely under the Supervisor's complete jurisdiction.
