# velix-Core: Personal AI system

**velix-Core** is a high-performance, C++ centered agentic framework. It operates on a **Hub-and-Spoke** model where the `Core` is the central nervous system, and all other modules (LLM, Telegram, Agents, Skills) are independent processes communicating via **TCP Loopback (127.0.0.1)**.

## 1. Fundamental Goals
* **Zero-Dependency Core:** Must compile with `g++` (C++17) on Windows, Linux, and macOS. 
* **Single-Header Vendor Strategy:** All external logic (JSON parsing, HTTP) must be single-header libraries included in the repo (e.g., `nlohmann/json.hpp`).
* **Local-First / Low-VRAM:** Architected for 2048–4096 token windows and 8B parameter models.
* **Strict Modularity:** New capabilities are "drag-and-drop" folders in `/agents` or `/skills`.
* **IPC via TCP:** Standardized JSON over loopback ensures cross-language support (C++, Python, etc.) without OS-specific pipes.
* **Multi-OS Support:** Native performance on Mac, Linux, and Windows.

---

## 2. Project Structure (One-Liners)

```text
/velix-core
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

## 3. Detailed File Responsibilities & Design Choices

### **`communication/` (The Foundation)**
* **`socket_wrapper.hpp`**: Abstracts `winsock2.h` (Win) and `sys/socket.h` (POSIX). 
    * *Design*: Header-only. Handles `WSAStartup` on Windows automatically.
* **`send.cpp` / `recv.cpp`**: Implements robust framed transport with `send_json(socket, json)` and `recv_json(socket)`.
* **Protocol**: All messages are **length-prefixed JSON** (`[4-byte big-endian length][raw JSON bytes]`) to avoid delimiter collisions and partial-frame ambiguity.
* **JSON Validation**: Uses vendored `vendor/nlohmann/json.hpp` (if available) to validate payloads before/after transport.

### **`core/handler.cpp` (The Brain)**
* **Role**: The primary router at **Port 5170**. It receives messages from `chat/`, requests a structured `messages[]` payload from `llm/context_manager`, and handles `EXEC` logic.
* **`EXEC` Logic**: Scans LLM output for `EXEC <name> <params> END_EXEC`. 
* **Design**: Intercepts these blocks. The user never sees raw code; they see results once the Handler re-queries the LLM with the tool's output.



### **`core/executioner.cpp` (The Muscle)**
* **Role**: Spawns sub-processes for Agents and Skills at **Port 5172**.
* **Logic**: Reads `tool_manifest.json` in a tool's folder to decide how to run it (e.g., `python3` vs `./binary`).
* **Execution**: Must run tools asynchronously so the Core doesn't freeze.

### **`llm/scheduler.cpp` (The Traffic Controller)**
* **Role**: Manages the LLM Request Queue at **Port 5171**.
* **Adapters**: Uses `llm/adapters/` to target provider endpoints (`llama.cpp` primary, `Ollama` future).
* **Key Inheritance**: If an Agent calls a Skill, the Skill inherits the Agent's "LLM Key" to bypass the queue.
* **Aging**: Automatically bumps priority of "Heartbeat" tasks to prevent starvation.

### **`core/supervisor.cpp` (The God Process)**
* **Role**: Monitors the PID (Process ID) of every running module and enforces process health.
* **Restart Logic**: If a critical process (e.g., Telegram module) crashes, `supervisor.cpp` restarts it.
* **Heartbeat Service**: Manages scheduled heartbeat jobs by executing tasks in `/jobs` every configurable **X minutes**.

### **`memory/` (The Identity)**
* **`soul.md`**: Fixed personality/behavior.
* **`user.md`**: Persistent facts (e.g., "User's ID is f20230029").
* **`daily_summaries/`**: Long-term memory.
* **`compacter.cpp`**: Summarizes `history/` JSON when tokens exceed 80% capacity.

---

## 4. Core Schemas (`/schema/`)

### **`llm_request.json`**
```json
{
  "request_id": "UUID-12345",
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
  }
}
```

### **`config/model.json` (llama.cpp endpoint config)**
```json
{
  "active_adapter": "llama.cpp",
  "adapters": {
    "llama.cpp": {
      "api_style": "openai-chat-completions",
      "base_url": "http://127.0.0.1:8080/v1",
      "model": "local-model",
      "api_key_env": null,
      "stop_tokens": ["END_EXEC"]
    },
    "ollama": {
      "enabled": false,
      "api_style": "openai-chat-completions",
      "base_url": "http://127.0.0.1:11434/v1",
      "model": "llama3",
      "api_key_env": null,
      "stop_tokens": ["END_EXEC"]
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

### **`tool_manifest.json` (Inside every Skill/Agent folder)**
```json
{
  "name": "web_search",
  "type": "skill",
  "exec_win": "python search.py",
  "exec_linux": "python3 search.py",
  "description": "Searches the web for current info.",
  "params": ["query"]
}
```

### **`config/ports.json` (Discovery)**
```json
{
  "HANDLER": 5170,
  "LLM_SCHEDULER": 5171,
  "EXECUTIONER": 5172,
  "SUPERVISOR": 5173
}
```

---

## 5. The Agentic Workflow (The EXEC Protocol)

1.  **Input**: User sends message via Telegram.
2.  **Context Build**: `context_manager.cpp` assembles structured roles (`system` from `soul.md` + `user.md`, prior turns from `history/`, latest `user` message).
3.  **Compaction**: If history is too long, `compacter.cpp` summarizes and `context_manager` replaces older turns with a compact `system` summary block.
4.  **LLM Call**: `handler` sends the structured `llm_request.json` payload to `scheduler`; `scheduler` maps it to llama.cpp OpenAI-style HTTP and injects model-configured `stop_tokens` from `config/model.json`.
5.  **Interception**: `handler` sees `EXEC`, stops Telegram output, sends request to `executioner`.
6.  **Execution**: `executioner` runs `skills/get_weather/main.py`. Returns JSON result.
7.  **Loopback**: `handler` appends result as an `Observation` and re-triggers the LLM.
8.  **Output**: LLM gives final answer; `handler` sends it to Telegram.

---

## 6. Build Order & Design Constraints

1.  **Level 0**: `communication/` & `schema/` (100% independent).
2.  **Level 1**: `utils/` & `memory/`.
3.  **Level 2**: `llm/scheduler` & `llm/context_manager`.
4.  **Level 3**: `core/executioner`.
5.  **Level 4**: `core/handler`.

**Strict Constraints:**
* **No `std::system()`**: Use `popen` or OS-specific spawns in `executioner.cpp`.
* **Relative Paths**: Use `std::filesystem` (C++17) for all pathing.
* **Stateless Executioner**: Tools must return data via TCP or write to `memory/`.
* **No Global State**: Follow the strict: `Chat -> Handler -> LLM -> Handler -> Executioner -> Handler -> Chat` flow.
