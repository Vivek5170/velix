# velix-Core: Runtime Architecture (Single Source of Truth)

`velix-Core` is a local-first, multi-process AI runtime designed as a mini operating system for agents.

This document is the authoritative architecture spec. It preserves the original core principles and applies the updated Scheduler / Executioner / Supervisor model.

---

## 1. Core Philosophy

- **Process Isolation**: Every major runtime role is a separate process.
- **Strict Responsibility Boundaries**: Components must not overlap roles.
- **Security-First Execution**: Process spawning is centralized and controlled.
- **Language Scalability**: Any language can integrate via JSON-over-TCP.
- **Local-First Efficiency**: Designed for constrained local LLM deployments.

---

## 2. Fundamental Principles (Preserved)

- **Zero-Dependency Core**: Build with `g++` / C++17 on Linux, macOS, Windows.
- **Single-Header Vendor Strategy**: Keep third-party logic vendored (e.g., `vendor/nlohmann/json.hpp`).
- **IPC via TCP Loopback**: Standardized transport over `127.0.0.1`.
- **Length-Prefixed Framing**: `[4-byte big-endian length][JSON bytes]` for robust transport.
- **Strict Modularity**: Agents/skills/jobs are folder-based, discoverable extensions.
- **Cross-Language Compatibility**: C++, Python, Rust, Go can participate through schema-compliant JSON.

### Project Structure (Restored Reference)

```text
velix-core
├── core/                   # Central Hub: Handler, Executioner, Supervisor.
├── llm/                    # Intelligence: Scheduler, Context Manager, Compacter, Adapters.
├── communication/          # Networking: Cross-platform TCP send/recv wrappers.
├── schema/                 # Truth: Fixed JSON templates for all IPC.
├── vendor/                 # Vendored single-header third-party libs (e.g., nlohmann/json.hpp).
├── memory/                 # Data: Soul, User facts, and Daily Summaries.
├── chat/                   # Interface: Telegram Bot (Core's primary mouth).
├── agents/                 # Logic: Multi-step task managers (Sub-folders).
├── skills/                 # Tools: Atomic functions (Sub-folders).
├── jobs/                   # Scheduled heartbeat/background jobs managed by Supervisor.
├── config/                 # Settings: Model paths, Ports, API Keys.
└── utils/                  # Helpers: Logger, Timer, String tools.
```

---

## 3. High-Level Runtime Topology

```text
User / Chat (Telegram or other channel)
  -> Handler
  -> Context Manager
  -> Scheduler
  -> Executioner (only when EXEC is emitted)
  -> Scheduler
  -> Handler
  -> Chat response

Supervisor runs as global authority across all processes and trees.
```

Primary modules:

- `core/handler.cpp`
- `llm/context_manager.cpp`
- `llm/scheduler.cpp`
- `core/executioner.cpp`
- `core/supervisor.cpp`

---

## 4. Component Responsibilities

### 4.0 Communication (`communication/`) — Foundation

- `socket_wrapper.hpp`: abstracts `winsock2.h` (Windows) and `sys/socket.h` (POSIX).
- `send.cpp` / `recv.cpp`: framed JSON transport helpers (`send_json`, `recv_json`).
- Protocol: length-prefixed JSON (`[4-byte big-endian length][raw JSON bytes]`).
- JSON validation uses vendored `vendor/nlohmann/json.hpp`.
- Status: communication layer is implemented and treated as stable foundation.

### 4.1 Handler (`core/handler.cpp`)

- Ingress/egress router for chat channels (`chat/telegram_bot.cpp` and future channels).
- Receives user input and requests context assembly.
- Sends structured LLM requests to Scheduler.
- Returns final assistant response to requesting process/channel.
- Does **not** own EXEC orchestration logic.

### 4.2 Context Manager (`llm/context_manager.cpp`)

- Builds `messages[]` from:
  - `memory/soul.md`
  - `memory/user.md`
  - `memory/history.json`
  - latest user input
- Applies compaction/summarization flow when token pressure is high (with `llm/compacter.cpp`).

### 4.3 Scheduler (`llm/scheduler.cpp`) — LLM Queue/Key Manager + EXEC Orchestrator

Scheduler responsibilities:

1. Manage LLM request queue.
2. Allocate limited LLM execution keys to trees.
3. Parse model outputs for EXEC blocks.
4. Coordinate tool execution via Executioner.
5. Re-run LLM after tool results until final response is produced.

- Sole manager for LLM request queueing and dispatch.
- Maintains tree-level fairness and priority.
- Dynamically assigns LLM keys to highest-priority trees in the main queue; keys are released when a tree finishes its active LLM request.
- Calls LLM adapter (`llama.cpp` now; additional adapters later).
- Detects `EXEC ... EXEC_END` blocks in model output.
- Explicitly parses EXEC blocks in model output before returning any result to requester paths.
- Sends `EXEC_REQUEST` messages to Executioner when EXEC blocks are detected in model output.
- Never spawns processes directly.
- On EXEC:
  1. issues execution request to Executioner,
  2. receives tool result,
  3. appends tool output as tool/observation message,
  4. calls LLM again,
  5. repeats until final non-EXEC response.
- If no EXEC is present, immediately returns response to requesting process (typically Handler).
- Users must never see raw EXEC blocks.

### 4.4 Executioner (`core/executioner.cpp`)

- **Only** component allowed to spawn agent/skill/job processes.
- Validates manifest and launch policy.
- Runtime validation before launch:
  - manifest exists,
  - entry file exists,
  - language/runtime is in allow-list,
  - timeout is within allowed bounds.
- Optional stronger validation: verify tool imports/links Velix runtime SDK contract.
- Executes tool process asynchronously with timeout and output capture.
- Routes normalized execution result JSON by request origin:
  - if `request_origin == scheduler` return to Scheduler,
  - else return to requesting parent process.
- Execution results are returned to the originating requester (Scheduler or parent process) using the normalized Tool Result Schema.

### 4.5 Supervisor (`core/supervisor.cpp`)

- Global system authority (“kernel-like” process).
- Maintains global process table and tree lifecycle state.
- Tracks heartbeats and process status.
- Detects crashes and can restart critical services.
- Enforces tree/runtime policies (limits, kill-tree, scheduled jobs).

Tree creation authority belongs exclusively to Supervisor.

Trees may be created by:

1. Handler requesting a new tree for user input.
2. Supervisor scheduled jobs.

Executioner does not create trees; it only attaches processes to an existing caller-provided `tree_id`.

Heartbeat policy:

- `HEARTBEAT_INTERVAL = 5s`
- If no heartbeat is received within `15s`, process is treated as dead.

Tree resource limits (enforced policies):

- `max_processes_per_tree`
- `max_tree_runtime`
- `max_memory_per_tree`
- `max_llm_requests_per_tree`

### 4.6 Memory (`memory/`) — Identity Layer

- `soul.md`: fixed personality/behavior.
- `user.md`: persistent user facts.
- `daily_summaries/`: long-term summarized memory.
- `history.json`: conversation/event history used by Context Manager.
- `llm/compacter.cpp`: summarizes history when context window pressure is high.

### 4.7 Velix Runtime SDK Layer

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

Example:

```text
TREE_HANDLER (system tree)
└ Handler

TREE_01
├ Agent_Research
│  ├ Skill_WebSearch
│  └ Skill_Parse
└ Skill_Calculator
```

Handler lifecycle rule:

- Handler runs in a dedicated system tree (`TREE_HANDLER`).
- User requests received by Handler cause Supervisor to spawn new execution trees.

Per-process metadata tracked by Supervisor includes:

- `pid`
- `tree_id`
- `parent_pid`
- `role`
- `llm_key`
- `status`
- `last_heartbeat`

`status` values are constrained to the following `ProcessStatus` enum:

```text
STARTING
RUNNING
WAITING_LLM
WAITING_EXEC
FINISHED
ERROR
KILLED
```

Suggested internal structure:

```cpp
std::unordered_map<int, ProcessInfo>
```

Tree-level state is tracked separately as `TreeStatus`:

```text
ACTIVE
WAITING_LLM_SLOT
WAITING_EXECUTION
COMPLETED
FAILED
KILLED
```

Tree ID assignment authority:

- Supervisor generates `tree_id` when a new tree is spawned.
- Executioner attaches this `tree_id` to every child process it spawns.
- Other modules must not generate independent tree identifiers.

Execution rules inside a tree:

- Agents may spawn skills.
- Skills may spawn other skills.
- Agents may spawn other agents.
- No process may create a new tree.
- All spawned processes inherit the parent's `tree_id`.

### 5.1 Process Lifecycle (Spawned Process)

```text
Executioner spawns process
  -> process initializes VelixProcess runtime
  -> REGISTER_PID to Supervisor
  -> process executes run()
  -> process sends RESULT
  -> Supervisor marks process complete
  -> parent/scheduler receives result
  -> process exits
```

---

## 6. Scheduler Queueing and LLM Key Policy

Scheduler manages:

1. **Main Tree Priority Queue** (which tree gets served next).
2. **Per-Tree FCFS Queue** (request order within each tree).

Scheduler selects the highest-priority tree that currently holds or can receive an LLM key and dequeues the next request from that tree's FCFS queue.

LLM key model:

- Limited concurrency slots, e.g., `MAX_LLM_KEYS = 5`.
- Each key maps to one concurrent LLM execution slot.
- A tree may hold only one key at a time.
- A tree cannot execute multiple LLM calls simultaneously.
- Scheduler assigns available keys to highest-priority eligible trees and releases keys immediately after active request completion.

### 6.1 LLM Keys vs Tree Limits

LLM keys and tree limits are independent mechanisms.

Tree limits (enforced by Supervisor):

- `max_processes_per_tree`
- `max_tree_runtime`
- `max_memory_per_tree`
- `max_llm_requests_per_tree`

LLM keys (managed by Scheduler):

- represent available concurrent LLM execution slots,
- example: `MAX_LLM_KEYS = 5`.

A tree may exist without an assigned LLM key.
Such trees remain in `WAITING_LLM_SLOT` until Scheduler assigns a key.

Anti-starvation:

- Apply priority aging: `priority += wait_time_factor`.

---

## 7. Runtime Request Flow (Authoritative)

1. Chat source sends user message to Handler.
2. Handler asks Context Manager to build `messages[]` using memory and `history.json`.
3. Handler sends structured request to Scheduler.
4. Scheduler dequeues by priority + fairness and calls LLM adapter.
5. If output includes EXEC:
   - Scheduler calls Executioner,
   - receives execution result,
   - appends tool result to conversation,
   - re-calls LLM.
  - strips raw EXEC blocks from user-visible response path.
6. When Scheduler gets final non-EXEC answer, it returns it to requester.
7. Handler sends final reply back to Telegram (or the originating channel/process).

Multiple EXEC block rule:

- Scheduler processes EXEC blocks sequentially.
- If multiple EXEC blocks are present, they are executed in order.
- After each execution result, Scheduler re-runs the LLM until a final response without EXEC is produced.

Agent-initiated skill calls are also valid in this runtime:

- `Agent -> Executioner -> Skill -> Executioner -> Agent`
- Routing follows the request-origin rule defined in Executioner responsibilities.

---

## 8. Manifest and Extensibility Contract

Each extension folder must define a manifest and entrypoint.

- Agents: `/agents/<name>/agent.json`
- Skills: `/skills/<name>/skill.json`
- Jobs: `/jobs/<name>/job.json` (if jobs module is enabled)

Executioner validates manifest before launch.

Folder contract examples:

```text
agents/
  research_agent/
    agent.json
    main.cpp

skills/
  web_search/
    skill.json
    search.py
```

Executioner resolves tool folder by requested tool name.

### 8.1 Skill Discovery Capability

Runtime should expose a discoverability tool, e.g., `list_skills`, so LLM can choose valid EXEC calls.

Example return:

```text
web_search(query)
weather(city)
```

---

## 9. Security Rules

- No spawning outside Executioner.
- Manifest validation before execution.
- Process-level isolation for crash containment.
- Scheduler-enforced LLM concurrency limits.
- Supervisor-enforced tree limits (runtime/process/memory policies).

### 9.1 Structured Logging Requirement

All core modules must emit structured logs through `utils/logger`.

Required fields:

- `timestamp`
- `module`
- `tree_id`
- `pid`
- `event`

---

## 10. Schemas and Config

Canonical files:

- `schema/llm_request.json`
- `/agents/<name>/agent.json`
- `/skills/<name>/skill.json`
- `/jobs/<name>/job.json`
- `config/model.json`
- `config/supervisor.json`
- `config/ports.json`

Ports discovery remains centralized in `config/ports.json`.

### IPC Message Types (Canonical)

To keep all modules interoperable, core IPC message types are fixed:

```text
REGISTER_PID
HEARTBEAT
LLM_REQUEST
EXEC_REQUEST
EXEC_RESULT
TREE_KILL
TREE_STATUS
```

Modules should reuse these message types rather than introducing ad-hoc variants.

### `schema/llm_request.json` (Reference)

```json
{
  "message_type": "LLM_REQUEST",
  "request_id": "UUID-12345",
  "tree_id": "TREE_01",
  "source_pid": 1001,
  "priority": 1,
  "inherit_key": null,
  "messages": [
    {"role": "system", "content": "Context from soul.md + user.md"},
    {"role": "user", "content": "Past conversation fragment 1"},
    {"role": "assistant", "content": "Past AI response"},
    {"role": "user", "content": "Current message from Telegram"}
  ],
  "sampling_params": {
    "temp": 0.7,
    "top_p": 0.9,
    "max_tokens": 512
  },
  "metadata": {
    "request_origin": "handler",
    "trace_id": "TRACE-UUID-12345",
    "timestamp": "2026-03-28T12:00:00Z"
  }
}
```

### Tool Result Schema (Normalized Runtime Contract)

```json
{
  "tree_id": "TREE_02",
  "pid": 1042,
  "status": "ok",
  "result": {},
  "error": null
}
```

Notes:

- `status` should be `ok` or `error`.
- `error` must be non-null when `status == error`.
- This schema is used for Scheduler and parent-process routing interoperability.

### `config/model.json` (llama.cpp Endpoint Config Reference)

```json
{
  "active_adapter": "llama.cpp",
  "adapters": {
    "llama.cpp": {
      "api_style": "openai-chat-completions",
      "base_url": "http://127.0.0.1:8080/v1",
      "model": "local-model",
      "api_key_env": null,
      "stop_tokens": ["EXEC_END"]
    },
    "ollama": {
      "enabled": false,
      "api_style": "openai-chat-completions",
      "base_url": "http://127.0.0.1:11434/v1",
      "model": "llama3",
      "api_key_env": null,
      "stop_tokens": ["EXEC_END"]
    }
  },
  "request_timeout_ms": 60000,
  "default_sampling_params": {
    "temp": 0.7,
    "top_p": 0.9,
    "max_tokens": 512
  }
}
```

### `schema/manifest_template.json` (Compatibility / Optional)

Canonical per-tool manifests are `agent.json`, `skill.json`, and `job.json` inside each tool folder.

If a shared schema template is needed for validation tooling, the following `schema/manifest_template.json` shape can be used:

```json
{
  "name": "web_search",
  "type": "skill",
  "entry": "search.py",
  "language": "python",
  "exec_win": "python search.py",
  "exec_linux": "python3 search.py",
  "exec_mac": "python3 search.py",
  "description": "Searches the web for current info.",
  "params": ["query"],
  "timeout_ms": 30000,
  "requires": [],
  "version": "1.0.0"
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

## 11. Build Order and Constraints

Build sequence:

1. `communication/` Done
2. `schema/` Done
3. `utils/` Done
4. `memory/`  Done
5. `core/supervisor` Done Partial - No jobs and after tree completion return to handler
6. `llm/context_manager`
7. `llm/scheduler`
8. `core/executioner`
9. `core/handler`

Constraints:

- Avoid `std::system()` in runtime execution paths.
- Use C++17 `std::filesystem` for path handling.
- Keep Executioner stateless (input request -> process run -> normalized result).
- Preserve strict modular dataflow:
  - `Chat -> Handler -> ContextManager -> Scheduler -> (Executioner loop if needed) -> Scheduler -> Handler -> Chat`

---

## 12. Outcome

`velix-Core` behaves as a modular, secure, scheduler-driven local agent runtime.

It is designed to scale from personal assistant usage to multi-agent orchestration while keeping strict process control and predictable execution semantics.
